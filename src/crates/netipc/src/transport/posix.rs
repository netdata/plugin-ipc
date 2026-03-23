//! L1 POSIX UDS SEQPACKET transport.
//!
//! Connection lifecycle, handshake with profile/limit negotiation,
//! and send/receive with transparent chunking over AF_UNIX SEQPACKET sockets.
//! Wire-compatible with the C implementation in netipc_uds.c.

use crate::protocol::{
    self, align8, ChunkHeader, Header, Hello, HelloAck, FLAG_BATCH, HEADER_SIZE, KIND_REQUEST,
    KIND_RESPONSE, MAGIC_CHUNK, MAGIC_MSG, MAX_PAYLOAD_DEFAULT, PROFILE_BASELINE, VERSION,
};
use std::collections::HashSet;
use std::ffi::CString;
use std::io;
use std::os::unix::io::RawFd;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

const DEFAULT_BACKLOG: i32 = 16;
const DEFAULT_BATCH_ITEMS: u32 = 1;
const DEFAULT_PACKET_SIZE_FALLBACK: u32 = 65536;
const HELLO_PAYLOAD_SIZE: usize = 44;
const HELLO_ACK_PAYLOAD_SIZE: usize = 48;

// ---------------------------------------------------------------------------
//  Errors
// ---------------------------------------------------------------------------

/// Transport-level errors.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum UdsError {
    /// Socket path exceeds sun_path limit.
    PathTooLong,
    /// socket()/bind()/listen() syscall failed.
    Socket(i32),
    /// connect() failed.
    Connect(i32),
    /// accept() failed.
    Accept(i32),
    /// send()/sendmsg() failed.
    Send(i32),
    /// recv() failed or peer disconnected.
    Recv(i32),
    /// Handshake protocol error.
    Handshake(String),
    /// Authentication token rejected.
    AuthFailed,
    /// No common profile between client and server.
    NoProfile,
    /// Wire protocol violation.
    Protocol(String),
    /// A live server already owns this socket path.
    AddrInUse,
    /// Chunk header mismatch during reassembly.
    Chunk(String),
    /// Memory allocation failed.
    Alloc,
    /// Payload or batch count exceeds negotiated limit.
    LimitExceeded,
    /// Invalid argument.
    BadParam(String),
    /// Duplicate message_id on send.
    DuplicateMsgId(u64),
    /// Unknown message_id on receive.
    UnknownMsgId(u64),
}

impl std::fmt::Display for UdsError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            UdsError::PathTooLong => write!(f, "socket path exceeds sun_path limit"),
            UdsError::Socket(e) => write!(f, "socket syscall failed: errno {e}"),
            UdsError::Connect(e) => write!(f, "connect failed: errno {e}"),
            UdsError::Accept(e) => write!(f, "accept failed: errno {e}"),
            UdsError::Send(e) => write!(f, "send failed: errno {e}"),
            UdsError::Recv(e) => write!(f, "recv failed: errno {e}"),
            UdsError::Handshake(s) => write!(f, "handshake error: {s}"),
            UdsError::AuthFailed => write!(f, "authentication token rejected"),
            UdsError::NoProfile => write!(f, "no common transport profile"),
            UdsError::Protocol(s) => write!(f, "protocol violation: {s}"),
            UdsError::AddrInUse => write!(f, "address already in use by live server"),
            UdsError::Chunk(s) => write!(f, "chunk error: {s}"),
            UdsError::Alloc => write!(f, "memory allocation failed"),
            UdsError::LimitExceeded => write!(f, "negotiated limit exceeded"),
            UdsError::BadParam(s) => write!(f, "bad parameter: {s}"),
            UdsError::DuplicateMsgId(id) => write!(f, "duplicate message_id: {id}"),
            UdsError::UnknownMsgId(id) => write!(f, "unknown response message_id: {id}"),
        }
    }
}

impl std::error::Error for UdsError {}

// ---------------------------------------------------------------------------
//  Role
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Role {
    Client = 1,
    Server = 2,
}

// ---------------------------------------------------------------------------
//  Configuration
// ---------------------------------------------------------------------------

/// Client connection configuration.
#[derive(Debug, Clone)]
pub struct ClientConfig {
    pub supported_profiles: u32,
    pub preferred_profiles: u32,
    pub max_request_payload_bytes: u32,
    pub max_request_batch_items: u32,
    pub max_response_payload_bytes: u32,
    pub max_response_batch_items: u32,
    pub auth_token: u64,
    /// 0 = auto-detect from SO_SNDBUF.
    pub packet_size: u32,
}

impl Default for ClientConfig {
    fn default() -> Self {
        Self {
            supported_profiles: PROFILE_BASELINE,
            preferred_profiles: 0,
            max_request_payload_bytes: 0,
            max_request_batch_items: 0,
            max_response_payload_bytes: 0,
            max_response_batch_items: 0,
            auth_token: 0,
            packet_size: 0,
        }
    }
}

/// Server configuration for listen + accept.
#[derive(Debug, Clone)]
pub struct ServerConfig {
    pub supported_profiles: u32,
    pub preferred_profiles: u32,
    pub max_request_payload_bytes: u32,
    pub max_request_batch_items: u32,
    pub max_response_payload_bytes: u32,
    pub max_response_batch_items: u32,
    pub auth_token: u64,
    /// 0 = auto-detect from SO_SNDBUF.
    pub packet_size: u32,
    /// listen() backlog, 0 = default (16).
    pub backlog: i32,
}

impl Default for ServerConfig {
    fn default() -> Self {
        Self {
            supported_profiles: PROFILE_BASELINE,
            preferred_profiles: 0,
            max_request_payload_bytes: 0,
            max_request_batch_items: 0,
            max_response_payload_bytes: 0,
            max_response_batch_items: 0,
            auth_token: 0,
            packet_size: 0,
            backlog: 0,
        }
    }
}

// ---------------------------------------------------------------------------
//  Session
// ---------------------------------------------------------------------------

/// A connected UDS SEQPACKET session (client or server side).
pub struct UdsSession {
    fd: RawFd,
    role: Role,

    // Negotiated limits
    pub max_request_payload_bytes: u32,
    pub max_request_batch_items: u32,
    pub max_response_payload_bytes: u32,
    pub max_response_batch_items: u32,
    pub packet_size: u32,
    pub selected_profile: u32,
    pub session_id: u64,

    // Internal receive buffer for chunked reassembly
    recv_buf: Vec<u8>,
    pkt_buf: Vec<u8>,

    // In-flight message_id set (client-side only)
    inflight_ids: HashSet<u64>,
}

impl UdsSession {
    /// Get the raw fd for poll/epoll integration.
    pub fn fd(&self) -> RawFd {
        self.fd
    }

    /// Get the session role.
    pub fn role(&self) -> Role {
        self.role
    }

    /// Return the most recently reassembled payload stored in the internal
    /// receive buffer.
    pub fn received_payload(&self, len: usize) -> &[u8] {
        &self.recv_buf[..len]
    }

    /// Connect to a server at `{run_dir}/{service_name}.sock`.
    /// Performs the full handshake. Blocks until connected + handshake done.
    pub fn connect(
        run_dir: &str,
        service_name: &str,
        config: &ClientConfig,
    ) -> Result<Self, UdsError> {
        let path = build_socket_path(run_dir, service_name)?;

        let fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET, 0) };
        if fd < 0 {
            return Err(UdsError::Socket(errno()));
        }

        let result = connect_and_handshake(fd, &path, config);
        if result.is_err() {
            unsafe {
                libc::close(fd);
            }
        }
        result
    }

    /// Send one logical message. `hdr` is the 32-byte outer header (caller
    /// fills kind, code, flags, item_count, message_id; this function sets
    /// magic/version/header_len/payload_len).
    ///
    /// If the total message (32 + payload_len) exceeds packet_size, the
    /// message is chunked transparently.
    pub fn send(&mut self, hdr: &mut Header, payload: &[u8]) -> Result<(), UdsError> {
        if self.fd < 0 {
            return Err(UdsError::BadParam("session closed".into()));
        }

        // Client-side: track in-flight message_ids for requests
        if self.role == Role::Client && hdr.kind == KIND_REQUEST {
            if !self.inflight_ids.insert(hdr.message_id) {
                return Err(UdsError::DuplicateMsgId(hdr.message_id));
            }
        }

        // Fill envelope fields
        hdr.magic = MAGIC_MSG;
        hdr.version = VERSION;
        hdr.header_len = protocol::HEADER_LEN;
        hdr.payload_len = payload.len() as u32;

        let tracked = self.role == Role::Client && hdr.kind == KIND_REQUEST;
        let msg_id = hdr.message_id;

        let result = self.send_inner(hdr, payload);

        // Rollback: remove message_id from in-flight set on send failure
        if result.is_err() && tracked {
            self.inflight_ids.remove(&msg_id);
        }

        result
    }

    /// Inner send logic, separated so the caller can rollback on failure.
    fn send_inner(&mut self, hdr: &mut Header, payload: &[u8]) -> Result<(), UdsError> {
        let total_msg = HEADER_SIZE + payload.len();

        // Single packet?
        if total_msg <= self.packet_size as usize {
            let mut hdr_buf = [0u8; HEADER_SIZE];
            hdr.encode(&mut hdr_buf);
            return raw_send_iov(self.fd, &hdr_buf, payload);
        }

        // Chunked send
        let chunk_payload_budget = self.packet_size as usize - HEADER_SIZE;
        if chunk_payload_budget == 0 {
            return Err(UdsError::BadParam("packet_size too small".into()));
        }

        let first_chunk_payload = payload.len().min(chunk_payload_budget);
        let remaining_after_first = payload.len() - first_chunk_payload;

        let continuation_chunks = if remaining_after_first > 0 {
            (remaining_after_first + chunk_payload_budget - 1) / chunk_payload_budget
        } else {
            0
        };
        let chunk_count = 1 + continuation_chunks as u32;

        // Send first chunk: outer header + first part of payload
        let mut hdr_buf = [0u8; HEADER_SIZE];
        hdr.encode(&mut hdr_buf);
        raw_send_iov(self.fd, &hdr_buf, &payload[..first_chunk_payload])?;

        // Send continuation chunks
        let mut offset = first_chunk_payload;
        for ci in 1..chunk_count {
            let remaining = payload.len() - offset;
            let this_chunk = remaining.min(chunk_payload_budget);

            let chk = ChunkHeader {
                magic: MAGIC_CHUNK,
                version: VERSION,
                flags: 0,
                message_id: hdr.message_id,
                total_message_len: total_msg as u32,
                chunk_index: ci,
                chunk_count,
                chunk_payload_len: this_chunk as u32,
            };

            let mut chk_buf = [0u8; HEADER_SIZE];
            chk.encode(&mut chk_buf);
            raw_send_iov(self.fd, &chk_buf, &payload[offset..offset + this_chunk])?;

            offset += this_chunk;
        }

        Ok(())
    }

    /// Receive one logical message. Blocks until a complete message arrives.
    ///
    /// On success, returns (header, payload_view). The payload view is valid
    /// until the next receive call on this session.
    pub fn receive<'a>(&'a mut self, buf: &'a mut [u8]) -> Result<(Header, &'a [u8]), UdsError> {
        if self.fd < 0 {
            return Err(UdsError::BadParam("session closed".into()));
        }

        // Read first packet
        let n = raw_recv(self.fd, buf)?;

        if n < HEADER_SIZE {
            return Err(UdsError::Protocol("packet too short for header".into()));
        }

        let hdr = Header::decode(&buf[..n])
            .map_err(|e| UdsError::Protocol(format!("header decode: {e}")))?;

        // Validate payload_len against negotiated directional limit.
        // Server receives requests; client receives responses.
        let max_payload = if self.role == Role::Server {
            self.max_request_payload_bytes
        } else {
            self.max_response_payload_bytes
        };
        if hdr.payload_len > max_payload {
            return Err(UdsError::LimitExceeded);
        }

        // Validate item_count against negotiated directional batch limit.
        let max_batch = if self.role == Role::Server {
            self.max_request_batch_items
        } else {
            self.max_response_batch_items
        };
        if hdr.item_count > max_batch {
            return Err(UdsError::LimitExceeded);
        }

        // Client-side: validate response message_id is in-flight
        if self.role == Role::Client && hdr.kind == KIND_RESPONSE {
            if !self.inflight_ids.remove(&hdr.message_id) {
                return Err(UdsError::UnknownMsgId(hdr.message_id));
            }
        }

        let total_msg = HEADER_SIZE + hdr.payload_len as usize;

        // Non-chunked: entire message in one packet
        if n >= total_msg {
            let payload = &buf[HEADER_SIZE..HEADER_SIZE + hdr.payload_len as usize];

            // Validate batch directory
            if hdr.flags & FLAG_BATCH != 0 && hdr.item_count > 1 {
                let dir_bytes = hdr.item_count as usize * 8;
                let dir_aligned = align8(dir_bytes);
                if payload.len() < dir_aligned {
                    return Err(UdsError::Protocol("batch directory exceeds payload".into()));
                }
                let packed_area_len = (payload.len() - dir_aligned) as u32;
                protocol::batch_dir_validate(
                    &payload[..dir_bytes],
                    hdr.item_count,
                    packed_area_len,
                )
                .map_err(|e| UdsError::Protocol(format!("batch directory: {e:?}")))?;
            }

            return Ok((hdr, payload));
        }

        // Chunked: first packet has partial payload
        let first_payload_bytes = n - HEADER_SIZE;

        // Grow recv_buf to hold full payload
        let needed = hdr.payload_len as usize;
        if self.recv_buf.len() < needed {
            self.recv_buf.resize(needed, 0);
        }

        // Copy first chunk's payload
        self.recv_buf[..first_payload_bytes]
            .copy_from_slice(&buf[HEADER_SIZE..HEADER_SIZE + first_payload_bytes]);

        let mut assembled = first_payload_bytes;
        let chunk_payload_budget = self.packet_size as usize - HEADER_SIZE;

        // Expected chunk count
        let remaining_after_first = hdr.payload_len as usize - first_payload_bytes;
        let expected_continuations = if remaining_after_first > 0 && chunk_payload_budget > 0 {
            (remaining_after_first + chunk_payload_budget - 1) / chunk_payload_budget
        } else {
            0
        };
        let expected_chunk_count = 1 + expected_continuations as u32;

        if self.pkt_buf.len() < self.packet_size as usize {
            self.pkt_buf.resize(self.packet_size as usize, 0);
        }

        let mut ci = 1u32;
        while assembled < hdr.payload_len as usize {
            let cn = raw_recv(self.fd, &mut self.pkt_buf)?;

            if cn < HEADER_SIZE {
                return Err(UdsError::Chunk("continuation too short".into()));
            }

            let chk = ChunkHeader::decode(&self.pkt_buf[..cn])
                .map_err(|e| UdsError::Chunk(format!("chunk header: {e}")))?;

            // Validate chunk header
            if chk.message_id != hdr.message_id {
                return Err(UdsError::Chunk("message_id mismatch".into()));
            }
            if chk.chunk_index != ci {
                return Err(UdsError::Chunk(format!(
                    "chunk_index mismatch: expected {ci}, got {}",
                    chk.chunk_index
                )));
            }
            if chk.chunk_count != expected_chunk_count {
                return Err(UdsError::Chunk("chunk_count mismatch".into()));
            }
            if chk.total_message_len != total_msg as u32 {
                return Err(UdsError::Chunk("total_message_len mismatch".into()));
            }

            let chunk_data = cn - HEADER_SIZE;
            if chunk_data != chk.chunk_payload_len as usize {
                return Err(UdsError::Chunk("chunk_payload_len mismatch".into()));
            }
            if assembled + chunk_data > hdr.payload_len as usize {
                return Err(UdsError::Chunk("chunk exceeds payload_len".into()));
            }

            self.recv_buf[assembled..assembled + chunk_data]
                .copy_from_slice(&self.pkt_buf[HEADER_SIZE..HEADER_SIZE + chunk_data]);
            assembled += chunk_data;
            ci += 1;
        }

        let payload = &self.recv_buf[..hdr.payload_len as usize];

        // Validate batch directory
        if hdr.flags & FLAG_BATCH != 0 && hdr.item_count > 1 {
            let dir_bytes = hdr.item_count as usize * 8;
            let dir_aligned = align8(dir_bytes);
            if payload.len() < dir_aligned {
                return Err(UdsError::Protocol("batch directory exceeds payload".into()));
            }
            let packed_area_len = (payload.len() - dir_aligned) as u32;
            protocol::batch_dir_validate(&payload[..dir_bytes], hdr.item_count, packed_area_len)
                .map_err(|e| UdsError::Protocol(format!("batch directory: {e:?}")))?;
        }

        Ok((hdr, payload))
    }
}

impl Drop for UdsSession {
    fn drop(&mut self) {
        if self.fd >= 0 {
            unsafe {
                libc::close(self.fd);
            }
            self.fd = -1;
        }
    }
}

// ---------------------------------------------------------------------------
//  Listener
// ---------------------------------------------------------------------------

/// A listening UDS SEQPACKET endpoint.
pub struct UdsListener {
    fd: RawFd,
    config: ServerConfig,
    path: PathBuf,
    next_session_id: AtomicU64,
}

impl UdsListener {
    /// Create a listener on `{run_dir}/{service_name}.sock`.
    /// Performs stale endpoint recovery.
    pub fn bind(run_dir: &str, service_name: &str, config: ServerConfig) -> Result<Self, UdsError> {
        let path = build_socket_path(run_dir, service_name)?;

        // Stale recovery
        match check_and_recover_stale(&path) {
            StaleResult::LiveServer => return Err(UdsError::AddrInUse),
            StaleResult::Stale | StaleResult::NotExist => { /* proceed */ }
        }

        let fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET, 0) };
        if fd < 0 {
            return Err(UdsError::Socket(errno()));
        }

        // Bind
        if let Err(e) = bind_unix(fd, &path) {
            unsafe {
                libc::close(fd);
            }
            return Err(e);
        }

        let backlog = if config.backlog > 0 {
            config.backlog
        } else {
            DEFAULT_BACKLOG
        };

        if unsafe { libc::listen(fd, backlog) } < 0 {
            let e = errno();
            unsafe {
                libc::close(fd);
            }
            let _ = std::fs::remove_file(&path);
            return Err(UdsError::Socket(e));
        }

        Ok(UdsListener {
            fd,
            config,
            path: PathBuf::from(&path),
            next_session_id: AtomicU64::new(1),
        })
    }

    /// Get the raw fd for poll/epoll integration.
    pub fn fd(&self) -> RawFd {
        self.fd
    }

    /// Accept one client. Performs the full handshake.
    /// Blocks until a client connects and the handshake completes.
    pub fn accept(&self) -> Result<UdsSession, UdsError> {
        let client_fd =
            unsafe { libc::accept(self.fd, std::ptr::null_mut(), std::ptr::null_mut()) };
        if client_fd < 0 {
            return Err(UdsError::Accept(errno()));
        }

        let session_id = self.next_session_id.fetch_add(1, Ordering::Relaxed);
        match server_handshake(client_fd, &self.config, session_id) {
            Ok(session) => Ok(session),
            Err(e) => {
                unsafe {
                    libc::close(client_fd);
                }
                Err(e)
            }
        }
    }
}

impl Drop for UdsListener {
    fn drop(&mut self) {
        if self.fd >= 0 {
            unsafe {
                libc::close(self.fd);
            }
            self.fd = -1;
        }
        if self.path.exists() {
            let _ = std::fs::remove_file(&self.path);
        }
    }
}

// ---------------------------------------------------------------------------
//  Internal helpers
// ---------------------------------------------------------------------------

fn errno() -> i32 {
    io::Error::last_os_error().raw_os_error().unwrap_or(0)
}

/// Validate service_name: only [a-zA-Z0-9._-], non-empty, not "." or "..".
fn validate_service_name(name: &str) -> Result<(), UdsError> {
    if name.is_empty() {
        return Err(UdsError::BadParam("empty service name".into()));
    }
    if name == "." || name == ".." {
        return Err(UdsError::BadParam(
            "service name cannot be '.' or '..'".into(),
        ));
    }
    for c in name.bytes() {
        match c {
            b'a'..=b'z' | b'A'..=b'Z' | b'0'..=b'9' | b'.' | b'_' | b'-' => {}
            _ => {
                return Err(UdsError::BadParam(format!(
                    "service name contains invalid character: {:?}",
                    c as char
                )))
            }
        }
    }
    Ok(())
}

/// Build `{run_dir}/{service_name}.sock` and validate length.
fn build_socket_path(run_dir: &str, service_name: &str) -> Result<String, UdsError> {
    validate_service_name(service_name)?;

    let path = format!("{run_dir}/{service_name}.sock");

    // sun_path is typically 108 bytes on Linux, 104 on macOS
    let max_sun_path =
        std::mem::size_of::<libc::sockaddr_un>() - std::mem::size_of::<libc::sa_family_t>();

    if path.len() >= max_sun_path {
        return Err(UdsError::PathTooLong);
    }
    Ok(path)
}

/// Get SO_SNDBUF as packet size.
fn detect_packet_size(fd: RawFd) -> u32 {
    let mut val: libc::c_int = 0;
    let mut len: libc::socklen_t = std::mem::size_of::<libc::c_int>() as libc::socklen_t;

    let rc = unsafe {
        libc::getsockopt(
            fd,
            libc::SOL_SOCKET,
            libc::SO_SNDBUF,
            &mut val as *mut _ as *mut libc::c_void,
            &mut len,
        )
    };

    if rc < 0 || val <= 0 {
        DEFAULT_PACKET_SIZE_FALLBACK
    } else {
        val as u32
    }
}

/// Highest set bit in a bitmask (0 if empty).
fn highest_bit(mask: u32) -> u32 {
    if mask == 0 {
        return 0;
    }
    1u32 << (31 - mask.leading_zeros() as u32)
}

fn apply_default(val: u32, def: u32) -> u32 {
    if val == 0 {
        def
    } else {
        val
    }
}

// ---------------------------------------------------------------------------
//  Low-level I/O
// ---------------------------------------------------------------------------

/// Send header + payload as one SEQPACKET message using sendmsg.
fn raw_send_iov(fd: RawFd, hdr: &[u8], payload: &[u8]) -> Result<(), UdsError> {
    let total = hdr.len() + payload.len();

    let mut iov = [
        libc::iovec {
            iov_base: hdr.as_ptr() as *mut libc::c_void,
            iov_len: hdr.len(),
        },
        libc::iovec {
            iov_base: payload.as_ptr() as *mut libc::c_void,
            iov_len: payload.len(),
        },
    ];

    let iovcnt = if payload.is_empty() { 1 } else { 2 };

    let msg = libc::msghdr {
        msg_name: std::ptr::null_mut(),
        msg_namelen: 0,
        msg_iov: iov.as_mut_ptr(),
        msg_iovlen: iovcnt,
        msg_control: std::ptr::null_mut(),
        msg_controllen: 0,
        msg_flags: 0,
    };

    let n = unsafe { libc::sendmsg(fd, &msg, libc::MSG_NOSIGNAL) };
    if n < 0 || n as usize != total {
        return Err(UdsError::Send(errno()));
    }
    Ok(())
}

/// Send a contiguous buffer as one SEQPACKET message.
fn raw_send(fd: RawFd, data: &[u8]) -> Result<(), UdsError> {
    let n = unsafe {
        libc::send(
            fd,
            data.as_ptr() as *const libc::c_void,
            data.len(),
            libc::MSG_NOSIGNAL,
        )
    };
    if n < 0 || n as usize != data.len() {
        return Err(UdsError::Send(errno()));
    }
    Ok(())
}

/// Receive one SEQPACKET message. Returns bytes received.
fn raw_recv(fd: RawFd, buf: &mut [u8]) -> Result<usize, UdsError> {
    let n = unsafe { libc::recv(fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len(), 0) };
    if n <= 0 {
        return Err(UdsError::Recv(if n == 0 { 0 } else { errno() }));
    }
    Ok(n as usize)
}

// ---------------------------------------------------------------------------
//  Socket helpers
// ---------------------------------------------------------------------------

/// Bind a Unix socket to the given path.
fn bind_unix(fd: RawFd, path: &str) -> Result<(), UdsError> {
    let c_path = CString::new(path).map_err(|_| UdsError::PathTooLong)?;

    let mut addr: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    addr.sun_family = libc::AF_UNIX as libc::sa_family_t;

    let path_bytes = c_path.as_bytes_with_nul();
    let sun_path_ptr = addr.sun_path.as_mut_ptr() as *mut u8;
    unsafe {
        std::ptr::copy_nonoverlapping(path_bytes.as_ptr(), sun_path_ptr, path_bytes.len());
    }

    let rc = unsafe {
        libc::bind(
            fd,
            &addr as *const libc::sockaddr_un as *const libc::sockaddr,
            std::mem::size_of::<libc::sockaddr_un>() as libc::socklen_t,
        )
    };
    if rc < 0 {
        return Err(UdsError::Socket(errno()));
    }
    Ok(())
}

/// Connect to a Unix socket at the given path.
fn connect_unix(fd: RawFd, path: &str) -> Result<(), UdsError> {
    let c_path = CString::new(path).map_err(|_| UdsError::PathTooLong)?;

    let mut addr: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    addr.sun_family = libc::AF_UNIX as libc::sa_family_t;

    let path_bytes = c_path.as_bytes_with_nul();
    let sun_path_ptr = addr.sun_path.as_mut_ptr() as *mut u8;
    unsafe {
        std::ptr::copy_nonoverlapping(path_bytes.as_ptr(), sun_path_ptr, path_bytes.len());
    }

    let rc = unsafe {
        libc::connect(
            fd,
            &addr as *const libc::sockaddr_un as *const libc::sockaddr,
            std::mem::size_of::<libc::sockaddr_un>() as libc::socklen_t,
        )
    };
    if rc < 0 {
        return Err(UdsError::Connect(errno()));
    }
    Ok(())
}

// ---------------------------------------------------------------------------
//  Stale endpoint recovery
// ---------------------------------------------------------------------------

enum StaleResult {
    NotExist,
    Stale,
    LiveServer,
}

fn check_and_recover_stale(path: &str) -> StaleResult {
    if !Path::new(path).exists() {
        return StaleResult::NotExist;
    }

    // Try connecting to see if a live server is there
    let probe = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET, 0) };
    if probe < 0 {
        return StaleResult::NotExist;
    }

    let result = match connect_unix(probe, path) {
        Ok(()) => {
            // Connected => live server
            StaleResult::LiveServer
        }
        Err(_) => {
            // Connection refused => stale, unlink
            let _ = std::fs::remove_file(path);
            StaleResult::Stale
        }
    };

    unsafe {
        libc::close(probe);
    }
    result
}

// ---------------------------------------------------------------------------
//  Handshake: client side
// ---------------------------------------------------------------------------

fn connect_and_handshake(
    fd: RawFd,
    path: &str,
    config: &ClientConfig,
) -> Result<UdsSession, UdsError> {
    connect_unix(fd, path)?;

    let pkt_size = if config.packet_size == 0 {
        detect_packet_size(fd)
    } else {
        config.packet_size
    };

    let supported = if config.supported_profiles == 0 {
        PROFILE_BASELINE
    } else {
        config.supported_profiles
    };

    // Build HELLO
    let hello = Hello {
        layout_version: 1,
        flags: 0,
        supported_profiles: supported,
        preferred_profiles: config.preferred_profiles,
        max_request_payload_bytes: apply_default(
            config.max_request_payload_bytes,
            MAX_PAYLOAD_DEFAULT,
        ),
        max_request_batch_items: apply_default(config.max_request_batch_items, DEFAULT_BATCH_ITEMS),
        max_response_payload_bytes: apply_default(
            config.max_response_payload_bytes,
            MAX_PAYLOAD_DEFAULT,
        ),
        max_response_batch_items: apply_default(
            config.max_response_batch_items,
            DEFAULT_BATCH_ITEMS,
        ),
        auth_token: config.auth_token,
        packet_size: pkt_size,
    };

    let mut hello_buf = [0u8; HELLO_PAYLOAD_SIZE];
    hello.encode(&mut hello_buf);

    // Build outer CONTROL header
    let hdr = Header {
        magic: MAGIC_MSG,
        version: VERSION,
        header_len: protocol::HEADER_LEN,
        kind: protocol::KIND_CONTROL,
        flags: 0,
        code: protocol::CODE_HELLO,
        transport_status: protocol::STATUS_OK,
        payload_len: HELLO_PAYLOAD_SIZE as u32,
        item_count: 1,
        message_id: 0,
    };

    let mut pkt = [0u8; HEADER_SIZE + HELLO_PAYLOAD_SIZE];
    hdr.encode(&mut pkt[..HEADER_SIZE]);
    pkt[HEADER_SIZE..].copy_from_slice(&hello_buf);

    // Send HELLO
    raw_send(fd, &pkt)?;

    // Receive HELLO_ACK
    let mut buf = [0u8; 128];
    let n = raw_recv(fd, &mut buf)?;

    // Decode outer header
    let ack_hdr =
        Header::decode(&buf[..n]).map_err(|e| UdsError::Protocol(format!("ack header: {e}")))?;

    if ack_hdr.kind != protocol::KIND_CONTROL || ack_hdr.code != protocol::CODE_HELLO_ACK {
        return Err(UdsError::Protocol("expected HELLO_ACK".into()));
    }

    // Check transport_status for rejection
    if ack_hdr.transport_status == protocol::STATUS_AUTH_FAILED {
        return Err(UdsError::AuthFailed);
    }
    if ack_hdr.transport_status == protocol::STATUS_UNSUPPORTED {
        return Err(UdsError::NoProfile);
    }
    if ack_hdr.transport_status != protocol::STATUS_OK {
        return Err(UdsError::Handshake(format!(
            "transport_status={}",
            ack_hdr.transport_status
        )));
    }

    // Decode hello-ack payload
    if n < HEADER_SIZE + HELLO_ACK_PAYLOAD_SIZE {
        return Err(UdsError::Protocol("ack payload truncated".into()));
    }
    let ack = HelloAck::decode(&buf[HEADER_SIZE..n])
        .map_err(|e| UdsError::Protocol(format!("ack payload: {e}")))?;

    Ok(UdsSession {
        fd,
        role: Role::Client,
        max_request_payload_bytes: ack.agreed_max_request_payload_bytes,
        max_request_batch_items: ack.agreed_max_request_batch_items,
        max_response_payload_bytes: ack.agreed_max_response_payload_bytes,
        max_response_batch_items: ack.agreed_max_response_batch_items,
        packet_size: ack.agreed_packet_size,
        selected_profile: ack.selected_profile,
        session_id: ack.session_id,
        recv_buf: Vec::new(),
        pkt_buf: Vec::new(),
        inflight_ids: HashSet::new(),
    })
}

// ---------------------------------------------------------------------------
//  Handshake: server side
// ---------------------------------------------------------------------------

fn server_handshake(
    fd: RawFd,
    config: &ServerConfig,
    session_id: u64,
) -> Result<UdsSession, UdsError> {
    let server_pkt_size = if config.packet_size == 0 {
        detect_packet_size(fd)
    } else {
        config.packet_size
    };

    let s_req_pay = apply_default(config.max_request_payload_bytes, MAX_PAYLOAD_DEFAULT);
    let s_req_bat = apply_default(config.max_request_batch_items, DEFAULT_BATCH_ITEMS);
    let s_resp_pay = apply_default(config.max_response_payload_bytes, MAX_PAYLOAD_DEFAULT);
    let s_resp_bat = apply_default(config.max_response_batch_items, DEFAULT_BATCH_ITEMS);
    let s_profiles = if config.supported_profiles == 0 {
        PROFILE_BASELINE
    } else {
        config.supported_profiles
    };
    let s_preferred = config.preferred_profiles;

    // Receive HELLO
    let mut buf = [0u8; 128];
    let n = raw_recv(fd, &mut buf)?;

    let hdr =
        Header::decode(&buf[..n]).map_err(|e| UdsError::Protocol(format!("hello header: {e}")))?;

    if hdr.kind != protocol::KIND_CONTROL || hdr.code != protocol::CODE_HELLO {
        return Err(UdsError::Protocol("expected HELLO".into()));
    }

    let hello = Hello::decode(&buf[HEADER_SIZE..n])
        .map_err(|e| UdsError::Protocol(format!("hello payload: {e}")))?;

    // Compute intersection
    let intersection = hello.supported_profiles & s_profiles;

    // Helper: send rejection ACK
    let send_rejection = |status: u16| -> Result<(), UdsError> {
        let ack = HelloAck {
            layout_version: 1,
            ..HelloAck::default()
        };
        let mut ack_buf = [0u8; HELLO_ACK_PAYLOAD_SIZE];
        ack.encode(&mut ack_buf);

        let ack_hdr = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: protocol::HEADER_LEN,
            kind: protocol::KIND_CONTROL,
            flags: 0,
            code: protocol::CODE_HELLO_ACK,
            transport_status: status,
            payload_len: HELLO_ACK_PAYLOAD_SIZE as u32,
            item_count: 1,
            message_id: 0,
        };

        let mut pkt = [0u8; HEADER_SIZE + HELLO_ACK_PAYLOAD_SIZE];
        ack_hdr.encode(&mut pkt[..HEADER_SIZE]);
        pkt[HEADER_SIZE..].copy_from_slice(&ack_buf);
        let _ = raw_send(fd, &pkt);
        Ok(())
    };

    // Check intersection
    if intersection == 0 {
        send_rejection(protocol::STATUS_UNSUPPORTED)?;
        return Err(UdsError::NoProfile);
    }

    // Check auth
    if hello.auth_token != config.auth_token {
        send_rejection(protocol::STATUS_AUTH_FAILED)?;
        return Err(UdsError::AuthFailed);
    }

    // Select profile
    let preferred_intersection = intersection & hello.preferred_profiles & s_preferred;
    let selected = if preferred_intersection != 0 {
        highest_bit(preferred_intersection)
    } else {
        highest_bit(intersection)
    };

    // Negotiate limits: min(client, server) per direction
    let agreed_req_pay = hello.max_request_payload_bytes.min(s_req_pay);
    let agreed_req_bat = hello.max_request_batch_items.min(s_req_bat);
    let agreed_resp_pay = hello.max_response_payload_bytes.min(s_resp_pay);
    let agreed_resp_bat = hello.max_response_batch_items.min(s_resp_bat);
    let agreed_pkt = hello.packet_size.min(server_pkt_size);

    // Send HELLO_ACK (success)
    let ack = HelloAck {
        layout_version: 1,
        flags: 0,
        server_supported_profiles: s_profiles,
        intersection_profiles: intersection,
        selected_profile: selected,
        agreed_max_request_payload_bytes: agreed_req_pay,
        agreed_max_request_batch_items: agreed_req_bat,
        agreed_max_response_payload_bytes: agreed_resp_pay,
        agreed_max_response_batch_items: agreed_resp_bat,
        agreed_packet_size: agreed_pkt,
        session_id,
    };

    let mut ack_buf = [0u8; HELLO_ACK_PAYLOAD_SIZE];
    ack.encode(&mut ack_buf);

    let ack_hdr = Header {
        magic: MAGIC_MSG,
        version: VERSION,
        header_len: protocol::HEADER_LEN,
        kind: protocol::KIND_CONTROL,
        flags: 0,
        code: protocol::CODE_HELLO_ACK,
        transport_status: protocol::STATUS_OK,
        payload_len: HELLO_ACK_PAYLOAD_SIZE as u32,
        item_count: 1,
        message_id: 0,
    };

    let mut pkt = [0u8; HEADER_SIZE + HELLO_ACK_PAYLOAD_SIZE];
    ack_hdr.encode(&mut pkt[..HEADER_SIZE]);
    pkt[HEADER_SIZE..].copy_from_slice(&ack_buf);
    raw_send(fd, &pkt)?;

    Ok(UdsSession {
        fd,
        role: Role::Server,
        max_request_payload_bytes: agreed_req_pay,
        max_request_batch_items: agreed_req_bat,
        max_response_payload_bytes: agreed_resp_pay,
        max_response_batch_items: agreed_resp_bat,
        packet_size: agreed_pkt,
        selected_profile: selected,
        session_id,
        recv_buf: Vec::new(),
        pkt_buf: Vec::new(),
        inflight_ids: HashSet::new(),
    })
}

// ---------------------------------------------------------------------------
//  Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol;
    use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
    use std::sync::Arc;
    use std::thread;
    use std::time::Duration;

    const TEST_RUN_DIR: &str = "/tmp/nipc_rust_test";
    const AUTH_TOKEN: u64 = 0xDEADBEEFCAFEBABE;

    fn ensure_run_dir() {
        let _ = std::fs::create_dir_all(TEST_RUN_DIR);
    }

    fn cleanup_socket(service: &str) {
        let path = format!("{TEST_RUN_DIR}/{service}.sock");
        let _ = std::fs::remove_file(&path);
    }

    fn default_server_config() -> ServerConfig {
        ServerConfig {
            supported_profiles: PROFILE_BASELINE,
            preferred_profiles: 0,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 16,
            max_response_payload_bytes: 4096,
            max_response_batch_items: 16,
            auth_token: AUTH_TOKEN,
            packet_size: 0,
            backlog: 4,
        }
    }

    fn default_client_config() -> ClientConfig {
        ClientConfig {
            supported_profiles: PROFILE_BASELINE,
            preferred_profiles: 0,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 16,
            max_response_payload_bytes: 4096,
            max_response_batch_items: 16,
            auth_token: AUTH_TOKEN,
            packet_size: 0,
        }
    }

    /// Unique service name to avoid parallel test collisions.
    static TEST_COUNTER: AtomicU32 = AtomicU32::new(0);
    fn unique_service(prefix: &str) -> String {
        let n = TEST_COUNTER.fetch_add(1, Ordering::Relaxed);
        format!("{prefix}_{n}_{}", std::process::id())
    }

    fn socketpair_seqpacket() -> (RawFd, RawFd) {
        let mut fds = [-1; 2];
        let rc =
            unsafe { libc::socketpair(libc::AF_UNIX, libc::SOCK_SEQPACKET, 0, fds.as_mut_ptr()) };
        assert_eq!(rc, 0, "socketpair failed: {}", errno());
        (fds[0], fds[1])
    }

    fn test_session(fd: RawFd, role: Role, packet_size: u32) -> UdsSession {
        UdsSession {
            fd,
            role,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 16,
            max_response_payload_bytes: 4096,
            max_response_batch_items: 16,
            packet_size,
            selected_profile: PROFILE_BASELINE,
            session_id: 1,
            recv_buf: Vec::new(),
            pkt_buf: Vec::new(),
            inflight_ids: HashSet::new(),
        }
    }

    fn raw_listener_fd(service: &str) -> RawFd {
        let path = build_socket_path(TEST_RUN_DIR, service).expect("socket path");
        let fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET, 0) };
        assert!(fd >= 0, "socket failed: {}", errno());
        bind_unix(fd, &path).expect("bind raw listener");
        let rc = unsafe { libc::listen(fd, DEFAULT_BACKLOG) };
        assert_eq!(rc, 0, "listen failed: {}", errno());
        fd
    }

    // -----------------------------------------------------------------------
    //  Test 1: Single client ping-pong
    // -----------------------------------------------------------------------

    #[test]
    fn test_ping_pong() {
        ensure_run_dir();
        let svc = unique_service("rs_ping");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, default_server_config())
                .expect("listen");
            ready_clone.store(true, Ordering::Release);

            let mut session = listener.accept().expect("accept");

            let mut buf = [0u8; 8192];
            let (hdr, payload) = session.receive(&mut buf).expect("recv");
            let payload = payload.to_vec();

            // Echo back as response
            let mut resp = hdr;
            resp.kind = protocol::KIND_RESPONSE;
            resp.transport_status = protocol::STATUS_OK;
            session.send(&mut resp, &payload).expect("send");
        });

        // Wait for server
        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let mut session =
            UdsSession::connect(TEST_RUN_DIR, &svc, &default_client_config()).expect("connect");

        assert_eq!(session.selected_profile, PROFILE_BASELINE);

        let payload = [0x01u8, 0x02, 0x03, 0x04];
        let mut hdr = Header {
            kind: protocol::KIND_REQUEST,
            code: protocol::METHOD_INCREMENT,
            flags: 0,
            item_count: 1,
            message_id: 42,
            ..Header::default()
        };

        session.send(&mut hdr, &payload).expect("send");

        let mut rbuf = [0u8; 4096];
        let (rhdr, rpayload) = session.receive(&mut rbuf).expect("recv");
        assert_eq!(rhdr.kind, protocol::KIND_RESPONSE);
        assert_eq!(rhdr.message_id, 42);
        assert_eq!(rpayload, payload);

        drop(session);
        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Test 2: Multi-client (2 clients, 1 listener)
    // -----------------------------------------------------------------------

    #[test]
    fn test_multi_client() {
        ensure_run_dir();
        let svc = unique_service("rs_multi");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, default_server_config())
                .expect("listen");
            ready_clone.store(true, Ordering::Release);

            for _ in 0..2 {
                let mut session = listener.accept().expect("accept");
                let mut buf = [0u8; 8192];
                let (hdr, payload) = session.receive(&mut buf).expect("recv");
                let payload = payload.to_vec();
                let mut resp = hdr;
                resp.kind = protocol::KIND_RESPONSE;
                resp.transport_status = protocol::STATUS_OK;
                session.send(&mut resp, &payload).expect("send");
            }
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let results: Vec<_> = (0..2)
            .map(|i| {
                let svc = svc.clone();
                thread::spawn(move || {
                    let mut session =
                        UdsSession::connect(TEST_RUN_DIR, &svc, &default_client_config())
                            .expect("connect");

                    let payload = [0xAA + i as u8];
                    let msg_id = 100 + i as u64;
                    let mut hdr = Header {
                        kind: protocol::KIND_REQUEST,
                        code: 1,
                        item_count: 1,
                        message_id: msg_id,
                        ..Header::default()
                    };
                    session.send(&mut hdr, &payload).expect("send");

                    let mut rbuf = [0u8; 4096];
                    let (rhdr, rpayload) = session.receive(&mut rbuf).expect("recv");
                    assert_eq!(rhdr.message_id, msg_id);
                    assert_eq!(rpayload, payload);
                })
            })
            .collect();

        for t in results {
            t.join().expect("client join");
        }

        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Test 3: Pipelining (send N, receive N)
    // -----------------------------------------------------------------------

    #[test]
    fn test_pipelining() {
        ensure_run_dir();
        let svc = unique_service("rs_pipe");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, default_server_config())
                .expect("listen");
            ready_clone.store(true, Ordering::Release);

            let mut session = listener.accept().expect("accept");

            for _ in 0..3 {
                let mut buf = [0u8; 8192];
                let (hdr, payload) = session.receive(&mut buf).expect("recv");
                let payload = payload.to_vec();
                let mut resp = hdr;
                resp.kind = protocol::KIND_RESPONSE;
                resp.transport_status = protocol::STATUS_OK;
                session.send(&mut resp, &payload).expect("send");
            }
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let mut session =
            UdsSession::connect(TEST_RUN_DIR, &svc, &default_client_config()).expect("connect");

        // Send 3 requests
        for i in 1u64..=3 {
            let payload = [i as u8];
            let mut hdr = Header {
                kind: protocol::KIND_REQUEST,
                code: 1,
                item_count: 1,
                message_id: i,
                ..Header::default()
            };
            session.send(&mut hdr, &payload).expect("send");
        }

        // Receive 3 responses
        for i in 1u64..=3 {
            let mut rbuf = [0u8; 4096];
            let (rhdr, rpayload) = session.receive(&mut rbuf).expect("recv");
            assert_eq!(rhdr.message_id, i);
            assert_eq!(rpayload, [i as u8]);
        }

        drop(session);
        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Test 4: Chunking (large message with small packet_size)
    // -----------------------------------------------------------------------

    #[test]
    fn test_chunking() {
        ensure_run_dir();
        let svc = unique_service("rs_chunk");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let scfg = ServerConfig {
                packet_size: 128,
                max_request_payload_bytes: 65536,
                max_response_payload_bytes: 65536,
                ..default_server_config()
            };
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, scfg).expect("listen");
            ready_clone.store(true, Ordering::Release);

            let mut session = listener.accept().expect("accept");

            let mut buf = [0u8; 256]; // small buf, forces recv_buf usage
            let (hdr, payload) = session.receive(&mut buf).expect("recv");
            let payload = payload.to_vec();

            let mut resp = hdr;
            resp.kind = protocol::KIND_RESPONSE;
            session.send(&mut resp, &payload).expect("send");
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let ccfg = ClientConfig {
            packet_size: 128,
            max_request_payload_bytes: 65536,
            max_response_payload_bytes: 65536,
            ..default_client_config()
        };
        let mut session = UdsSession::connect(TEST_RUN_DIR, &svc, &ccfg).expect("connect");

        assert_eq!(session.packet_size, 128);

        // Build a payload larger than 128 - 32 = 96 bytes
        let big_len = 500;
        let big: Vec<u8> = (0..big_len).map(|i| (i & 0xFF) as u8).collect();

        let mut hdr = Header {
            kind: protocol::KIND_REQUEST,
            code: 1,
            item_count: 1,
            message_id: 7,
            ..Header::default()
        };

        session.send(&mut hdr, &big).expect("send chunked");

        let mut rbuf = [0u8; 256];
        let (rhdr, rpayload) = session.receive(&mut rbuf).expect("recv chunked");
        assert_eq!(rhdr.message_id, 7);
        assert_eq!(rpayload.len(), big_len);
        assert_eq!(rpayload, big);

        drop(session);
        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Test 5: Handshake failure - bad auth
    // -----------------------------------------------------------------------

    #[test]
    fn test_bad_auth() {
        ensure_run_dir();
        let svc = unique_service("rs_badauth");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, default_server_config())
                .expect("listen");
            ready_clone.store(true, Ordering::Release);
            // accept will fail due to auth mismatch
            let _ = listener.accept();
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let ccfg = ClientConfig {
            auth_token: 0xBAD,
            ..default_client_config()
        };

        let result = UdsSession::connect(TEST_RUN_DIR, &svc, &ccfg);
        assert!(matches!(result, Err(UdsError::AuthFailed)));

        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Test 6: Handshake failure - profile mismatch
    // -----------------------------------------------------------------------

    #[test]
    fn test_profile_mismatch() {
        ensure_run_dir();
        let svc = unique_service("rs_badprof");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let scfg = ServerConfig {
                supported_profiles: protocol::PROFILE_SHM_FUTEX,
                ..default_server_config()
            };
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, scfg).expect("listen");
            ready_clone.store(true, Ordering::Release);
            let _ = listener.accept();
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let ccfg = ClientConfig {
            supported_profiles: PROFILE_BASELINE,
            ..default_client_config()
        };

        let result = UdsSession::connect(TEST_RUN_DIR, &svc, &ccfg);
        assert!(matches!(result, Err(UdsError::NoProfile)));

        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Test 7: Stale socket recovery
    // -----------------------------------------------------------------------

    #[test]
    fn test_stale_recovery() {
        ensure_run_dir();
        let svc = unique_service("rs_stale");
        cleanup_socket(&svc);

        let path = format!("{TEST_RUN_DIR}/{svc}.sock");

        // Create a stale socket file (bound but not listening)
        unsafe {
            let sock = libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET, 0);
            assert!(sock >= 0);

            let c_path = CString::new(path.as_str()).unwrap();
            let mut addr: libc::sockaddr_un = std::mem::zeroed();
            addr.sun_family = libc::AF_UNIX as libc::sa_family_t;
            let path_bytes = c_path.as_bytes_with_nul();
            let sun_path_ptr = addr.sun_path.as_mut_ptr() as *mut u8;
            std::ptr::copy_nonoverlapping(path_bytes.as_ptr(), sun_path_ptr, path_bytes.len());

            libc::bind(
                sock,
                &addr as *const libc::sockaddr_un as *const libc::sockaddr,
                std::mem::size_of::<libc::sockaddr_un>() as libc::socklen_t,
            );
            // Close without unlink => stale
            libc::close(sock);
        }

        assert!(Path::new(&path).exists(), "stale socket should exist");

        // listen should recover it
        let listener = UdsListener::bind(TEST_RUN_DIR, &svc, default_server_config())
            .expect("listen should recover stale socket");
        drop(listener);
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Test 8: Disconnect detection
    // -----------------------------------------------------------------------

    #[test]
    fn test_disconnect_detection() {
        ensure_run_dir();
        let svc = unique_service("rs_disc");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, default_server_config())
                .expect("listen");
            ready_clone.store(true, Ordering::Release);

            let mut session = listener.accept().expect("accept");
            // Read request then close without responding
            let mut buf = [0u8; 4096];
            let _ = session.receive(&mut buf);
            drop(session); // close socket
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let mut session =
            UdsSession::connect(TEST_RUN_DIR, &svc, &default_client_config()).expect("connect");

        let mut hdr = Header {
            kind: protocol::KIND_REQUEST,
            code: 1,
            item_count: 1,
            message_id: 99,
            ..Header::default()
        };
        session.send(&mut hdr, &[0xFF]).expect("send");

        // Receive should fail because server disconnected
        let mut rbuf = [0u8; 4096];
        let result = session.receive(&mut rbuf);
        assert!(result.is_err());

        drop(session);
        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Test 9: Batch send/receive
    // -----------------------------------------------------------------------

    #[test]
    fn test_batch() {
        ensure_run_dir();
        let svc = unique_service("rs_batch");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, default_server_config())
                .expect("listen");
            ready_clone.store(true, Ordering::Release);

            let mut session = listener.accept().expect("accept");
            let mut buf = [0u8; 8192];
            let (hdr, payload) = session.receive(&mut buf).expect("recv");
            let payload = payload.to_vec();
            let mut resp = hdr;
            resp.kind = protocol::KIND_RESPONSE;
            resp.transport_status = protocol::STATUS_OK;
            session.send(&mut resp, &payload).expect("send");
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let mut session =
            UdsSession::connect(TEST_RUN_DIR, &svc, &default_client_config()).expect("connect");

        // Build batch using protocol layer
        let mut batch_buf = [0u8; 2048];
        let mut builder = protocol::BatchBuilder::new(&mut batch_buf, 3);

        let item0 = [0x10u8, 0x20];
        let item1 = [0x30u8, 0x40, 0x50];
        let item2 = [0x60u8];

        builder.add(&item0).expect("add item0");
        builder.add(&item1).expect("add item1");
        builder.add(&item2).expect("add item2");

        let (batch_len, batch_count) = builder.finish();
        assert_eq!(batch_count, 3);

        let mut hdr = Header {
            kind: protocol::KIND_REQUEST,
            code: protocol::METHOD_INCREMENT,
            flags: protocol::FLAG_BATCH,
            item_count: batch_count,
            message_id: 55,
            ..Header::default()
        };

        session
            .send(&mut hdr, &batch_buf[..batch_len])
            .expect("send batch");

        let mut rbuf = [0u8; 4096];
        let (rhdr, rpayload) = session.receive(&mut rbuf).expect("recv batch");
        assert_eq!(rhdr.message_id, 55);
        assert!(rhdr.flags & protocol::FLAG_BATCH != 0);
        assert_eq!(rhdr.item_count, 3);

        // Verify items
        let (ip0, len0) = protocol::batch_item_get(&rpayload, 3, 0).expect("item0");
        assert_eq!(len0, 2);
        assert_eq!(ip0, &item0);

        let (ip1, len1) = protocol::batch_item_get(&rpayload, 3, 1).expect("item1");
        assert_eq!(len1, 3);
        assert_eq!(ip1, &item1);

        let (ip2, len2) = protocol::batch_item_get(&rpayload, 3, 2).expect("item2");
        assert_eq!(len2, 1);
        assert_eq!(ip2, &item2);

        drop(session);
        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Test 10: Large chunked pipelining
    // -----------------------------------------------------------------------

    #[test]
    fn test_chunked_pipelining() {
        ensure_run_dir();
        let svc = unique_service("rs_chkpipe");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let scfg = ServerConfig {
                packet_size: 128,
                max_request_payload_bytes: 65536,
                max_response_payload_bytes: 65536,
                ..default_server_config()
            };
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, scfg).expect("listen");
            ready_clone.store(true, Ordering::Release);

            let mut session = listener.accept().expect("accept");

            // Echo 3 messages
            for _ in 0..3 {
                let mut buf = [0u8; 256];
                let (hdr, payload) = session.receive(&mut buf).expect("recv");
                let payload = payload.to_vec();
                let mut resp = hdr;
                resp.kind = protocol::KIND_RESPONSE;
                session.send(&mut resp, &payload).expect("send");
            }
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let ccfg = ClientConfig {
            packet_size: 128,
            max_request_payload_bytes: 65536,
            max_response_payload_bytes: 65536,
            ..default_client_config()
        };
        let mut session = UdsSession::connect(TEST_RUN_DIR, &svc, &ccfg).expect("connect");

        // Send 3 chunked messages in pipeline
        let sizes = [200usize, 500, 300];
        for (i, &sz) in sizes.iter().enumerate() {
            let payload: Vec<u8> = (0..sz).map(|j| ((i + j) & 0xFF) as u8).collect();
            let mut hdr = Header {
                kind: protocol::KIND_REQUEST,
                code: 1,
                item_count: 1,
                message_id: (i + 1) as u64,
                ..Header::default()
            };
            session.send(&mut hdr, &payload).expect("send");
        }

        // Receive 3 responses
        for (i, &sz) in sizes.iter().enumerate() {
            let mut rbuf = [0u8; 256];
            let (rhdr, rpayload) = session.receive(&mut rbuf).expect("recv");
            assert_eq!(rhdr.message_id, (i + 1) as u64);
            let expected: Vec<u8> = (0..sz).map(|j| ((i + j) & 0xFF) as u8).collect();
            assert_eq!(rpayload, expected);
        }

        drop(session);
        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Test: Pipeline 10 requests, verify all matched by message_id
    // -----------------------------------------------------------------------

    #[test]
    fn test_pipeline_10() {
        ensure_run_dir();
        let svc = unique_service("rs_pipe10");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, default_server_config())
                .expect("listen");
            ready_clone.store(true, Ordering::Release);

            let mut session = listener.accept().expect("accept");

            for _ in 0..10 {
                let mut buf = [0u8; 8192];
                let (hdr, payload) = session.receive(&mut buf).expect("recv");
                let payload = payload.to_vec();
                let mut resp = hdr;
                resp.kind = protocol::KIND_RESPONSE;
                resp.transport_status = protocol::STATUS_OK;
                session.send(&mut resp, &payload).expect("send");
            }
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let mut session =
            UdsSession::connect(TEST_RUN_DIR, &svc, &default_client_config()).expect("connect");

        // Send 10 requests before reading any response
        for i in 1u64..=10 {
            let payload = i.to_ne_bytes();
            let mut hdr = Header {
                kind: protocol::KIND_REQUEST,
                code: 1,
                item_count: 1,
                message_id: i,
                ..Header::default()
            };
            session.send(&mut hdr, &payload).expect("send");
        }

        // Receive 10 responses, verify message_id and payload
        for i in 1u64..=10 {
            let mut rbuf = [0u8; 4096];
            let (rhdr, rpayload) = session.receive(&mut rbuf).expect("recv");
            assert_eq!(rhdr.message_id, i, "message_id mismatch at {i}");
            assert_eq!(rpayload.len(), 8, "payload len at {i}");
            let val = u64::from_ne_bytes(rpayload.try_into().unwrap());
            assert_eq!(val, i, "payload value at {i}");
        }

        drop(session);
        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Test: Pipeline 100 requests (stress pipelining)
    // -----------------------------------------------------------------------

    #[test]
    fn test_pipeline_100() {
        ensure_run_dir();
        let svc = unique_service("rs_pipe100");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let scfg = ServerConfig {
                max_request_payload_bytes: 65536,
                max_response_payload_bytes: 65536,
                ..default_server_config()
            };
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, scfg).expect("listen");
            ready_clone.store(true, Ordering::Release);

            let mut session = listener.accept().expect("accept");

            for _ in 0..100 {
                let mut buf = [0u8; 8192];
                let (hdr, payload) = session.receive(&mut buf).expect("recv");
                let payload = payload.to_vec();
                let mut resp = hdr;
                resp.kind = protocol::KIND_RESPONSE;
                resp.transport_status = protocol::STATUS_OK;
                session.send(&mut resp, &payload).expect("send");
            }
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let ccfg = ClientConfig {
            max_request_payload_bytes: 65536,
            max_response_payload_bytes: 65536,
            ..default_client_config()
        };
        let mut session = UdsSession::connect(TEST_RUN_DIR, &svc, &ccfg).expect("connect");

        // Send 100 requests
        for i in 1u64..=100 {
            let payload = i.to_ne_bytes();
            let mut hdr = Header {
                kind: protocol::KIND_REQUEST,
                code: 1,
                item_count: 1,
                message_id: i,
                ..Header::default()
            };
            session.send(&mut hdr, &payload).expect("send");
        }

        // Receive 100 responses
        for i in 1u64..=100 {
            let mut rbuf = [0u8; 4096];
            let (rhdr, rpayload) = session.receive(&mut rbuf).expect("recv");
            assert_eq!(rhdr.message_id, i);
            let val = u64::from_ne_bytes(rpayload.try_into().unwrap());
            assert_eq!(val, i);
        }

        drop(session);
        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Test: Pipeline with mixed message sizes
    // -----------------------------------------------------------------------

    #[test]
    fn test_pipeline_mixed_sizes() {
        ensure_run_dir();
        let svc = unique_service("rs_pipemix");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let sizes = [8usize, 256, 1024, 8, 256, 1024, 8, 256, 1024];
        let count = sizes.len();

        let server_thread = thread::spawn(move || {
            let scfg = ServerConfig {
                max_request_payload_bytes: 65536,
                max_response_payload_bytes: 65536,
                ..default_server_config()
            };
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, scfg).expect("listen");
            ready_clone.store(true, Ordering::Release);

            let mut session = listener.accept().expect("accept");

            for _ in 0..count {
                let mut buf = [0u8; 8192];
                let (hdr, payload) = session.receive(&mut buf).expect("recv");
                let payload = payload.to_vec();
                let mut resp = hdr;
                resp.kind = protocol::KIND_RESPONSE;
                resp.transport_status = protocol::STATUS_OK;
                session.send(&mut resp, &payload).expect("send");
            }
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let ccfg = ClientConfig {
            max_request_payload_bytes: 65536,
            max_response_payload_bytes: 65536,
            ..default_client_config()
        };
        let mut session = UdsSession::connect(TEST_RUN_DIR, &svc, &ccfg).expect("connect");

        // Send all messages with varying sizes
        for (i, &sz) in sizes.iter().enumerate() {
            let payload: Vec<u8> = (0..sz).map(|j| ((i * 37 + j) & 0xFF) as u8).collect();
            let mut hdr = Header {
                kind: protocol::KIND_REQUEST,
                code: 1,
                item_count: 1,
                message_id: (i + 1) as u64,
                ..Header::default()
            };
            session.send(&mut hdr, &payload).expect("send");
        }

        // Receive all responses
        for (i, &sz) in sizes.iter().enumerate() {
            let mut rbuf = [0u8; 4096];
            let (rhdr, rpayload) = session.receive(&mut rbuf).expect("recv");
            assert_eq!(rhdr.message_id, (i + 1) as u64, "message_id at {i}");
            assert_eq!(rpayload.len(), sz, "payload len at {i}");
            let expected: Vec<u8> = (0..sz).map(|j| ((i * 37 + j) & 0xFF) as u8).collect();
            assert_eq!(rpayload, expected, "payload data at {i}");
        }

        drop(session);
        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Test: Pipeline with chunked messages (> packet_size)
    // -----------------------------------------------------------------------

    #[test]
    fn test_pipeline_chunked_multi() {
        ensure_run_dir();
        let svc = unique_service("rs_pipechk2");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let sizes = [200usize, 500, 300, 800, 150];
        let count = sizes.len();

        let server_thread = thread::spawn(move || {
            let scfg = ServerConfig {
                packet_size: 128,
                max_request_payload_bytes: 65536,
                max_response_payload_bytes: 65536,
                ..default_server_config()
            };
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, scfg).expect("listen");
            ready_clone.store(true, Ordering::Release);

            let mut session = listener.accept().expect("accept");

            for _ in 0..count {
                let mut buf = [0u8; 256];
                let (hdr, payload) = session.receive(&mut buf).expect("recv");
                let payload = payload.to_vec();
                let mut resp = hdr;
                resp.kind = protocol::KIND_RESPONSE;
                session.send(&mut resp, &payload).expect("send");
            }
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let ccfg = ClientConfig {
            packet_size: 128,
            max_request_payload_bytes: 65536,
            max_response_payload_bytes: 65536,
            ..default_client_config()
        };
        let mut session = UdsSession::connect(TEST_RUN_DIR, &svc, &ccfg).expect("connect");

        // Send all chunked messages
        for (i, &sz) in sizes.iter().enumerate() {
            let payload: Vec<u8> = (0..sz).map(|j| ((i + j) & 0xFF) as u8).collect();
            let mut hdr = Header {
                kind: protocol::KIND_REQUEST,
                code: 1,
                item_count: 1,
                message_id: (i + 1) as u64,
                ..Header::default()
            };
            session.send(&mut hdr, &payload).expect("send");
        }

        // Receive all responses
        for (i, &sz) in sizes.iter().enumerate() {
            let mut rbuf = [0u8; 256];
            let (rhdr, rpayload) = session.receive(&mut rbuf).expect("recv");
            assert_eq!(rhdr.message_id, (i + 1) as u64);
            let expected: Vec<u8> = (0..sz).map(|j| ((i + j) & 0xFF) as u8).collect();
            assert_eq!(rpayload, expected);
        }

        drop(session);
        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    #[test]
    fn test_invalid_service_name() {
        let bad_names = &["", ".", "..", "foo/bar", "../etc", "name space", "a@b"];
        for name in bad_names {
            let result = validate_service_name(name);
            assert!(result.is_err(), "should reject {:?}", name);
        }

        let good_names = &[
            "valid-name",
            "valid_name",
            "valid.name",
            "ValidName123",
            "a",
        ];
        for name in good_names {
            validate_service_name(name)
                .unwrap_or_else(|e| panic!("{:?} should be valid: {e}", name));
        }
    }

    #[test]
    fn test_hello_decode_nonzero_padding() {
        let h = Hello {
            layout_version: 1,
            supported_profiles: PROFILE_BASELINE,
            max_request_payload_bytes: 1024,
            max_request_batch_items: 1,
            max_response_payload_bytes: 1024,
            max_response_batch_items: 1,
            packet_size: 65536,
            ..Default::default()
        };

        let mut buf = [0u8; 44];
        h.encode(&mut buf);
        Hello::decode(&buf).expect("valid hello should decode");

        // Corrupt padding bytes 28..32
        buf[28] = 0xFF;
        assert_eq!(Hello::decode(&buf), Err(protocol::NipcError::BadLayout));
    }

    // -----------------------------------------------------------------------
    //  UdsError Display coverage (lines 73-91)
    // -----------------------------------------------------------------------

    #[test]
    fn uds_error_display_all_variants() {
        let cases: Vec<(UdsError, &str)> = vec![
            (UdsError::PathTooLong, "socket path exceeds sun_path limit"),
            (UdsError::Socket(22), "socket syscall failed: errno 22"),
            (UdsError::Connect(111), "connect failed: errno 111"),
            (UdsError::Accept(24), "accept failed: errno 24"),
            (UdsError::Send(32), "send failed: errno 32"),
            (UdsError::Recv(0), "recv failed: errno 0"),
            (UdsError::Handshake("test".into()), "handshake error: test"),
            (UdsError::AuthFailed, "authentication token rejected"),
            (UdsError::NoProfile, "no common transport profile"),
            (UdsError::Protocol("bad".into()), "protocol violation: bad"),
            (UdsError::AddrInUse, "address already in use by live server"),
            (UdsError::Chunk("mismatch".into()), "chunk error: mismatch"),
            (UdsError::Alloc, "memory allocation failed"),
            (UdsError::LimitExceeded, "negotiated limit exceeded"),
            (UdsError::BadParam("foo".into()), "bad parameter: foo"),
            (UdsError::DuplicateMsgId(42), "duplicate message_id: 42"),
            (
                UdsError::UnknownMsgId(99),
                "unknown response message_id: 99",
            ),
        ];
        for (err, expected) in cases {
            assert_eq!(format!("{}", err), expected);
        }
        // Verify Error trait
        let e: &dyn std::error::Error = &UdsError::PathTooLong;
        let _ = format!("{e}");
    }

    // -----------------------------------------------------------------------
    //  UdsSession::role() coverage (lines 205-206)
    // -----------------------------------------------------------------------

    #[test]
    fn test_session_role() {
        ensure_run_dir();
        let svc = unique_service("rs_role");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, default_server_config())
                .expect("listen");
            ready_clone.store(true, Ordering::Release);
            let session = listener.accept().expect("accept");
            assert_eq!(session.role(), Role::Server);
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let session =
            UdsSession::connect(TEST_RUN_DIR, &svc, &default_client_config()).expect("connect");
        assert_eq!(session.role(), Role::Client);

        drop(session);
        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Send on closed session (line 238)
    // -----------------------------------------------------------------------

    #[test]
    fn test_send_on_closed_session() {
        ensure_run_dir();
        let svc = unique_service("rs_sendclosed");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, default_server_config())
                .expect("listen");
            ready_clone.store(true, Ordering::Release);
            let _session = listener.accept().expect("accept");
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let mut session =
            UdsSession::connect(TEST_RUN_DIR, &svc, &default_client_config()).expect("connect");

        // Simulate closed session by closing the fd via Drop-like behavior
        // We can't call close() (no such method), so we close fd directly
        unsafe {
            libc::close(session.fd);
        }
        session.fd = -1;

        let mut hdr = Header {
            kind: protocol::KIND_REQUEST,
            code: 1,
            item_count: 1,
            message_id: 1,
            ..Header::default()
        };
        let result = session.send(&mut hdr, &[1, 2, 3]);
        assert!(matches!(result, Err(UdsError::BadParam(_))));

        // Receive on closed session (line 333)
        let mut rbuf = [0u8; 4096];
        let result = session.receive(&mut rbuf);
        assert!(matches!(result, Err(UdsError::BadParam(_))));

        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Duplicate message_id (line 244)
    // -----------------------------------------------------------------------

    #[test]
    fn test_duplicate_message_id() {
        ensure_run_dir();
        let svc = unique_service("rs_dupmsg");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, default_server_config())
                .expect("listen");
            ready_clone.store(true, Ordering::Release);
            let _session = listener.accept().expect("accept");
            // Hold connection open while client tests
            thread::sleep(Duration::from_millis(500));
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let mut session =
            UdsSession::connect(TEST_RUN_DIR, &svc, &default_client_config()).expect("connect");

        // First send with message_id=42 should succeed
        let mut hdr1 = Header {
            kind: protocol::KIND_REQUEST,
            code: 1,
            item_count: 1,
            message_id: 42,
            ..Header::default()
        };
        session.send(&mut hdr1, &[1]).expect("first send ok");

        // Second send with same message_id=42 should fail
        let mut hdr2 = Header {
            kind: protocol::KIND_REQUEST,
            code: 1,
            item_count: 1,
            message_id: 42,
            ..Header::default()
        };
        let result = session.send(&mut hdr2, &[2]);
        assert!(matches!(result, Err(UdsError::DuplicateMsgId(42))));

        drop(session);
        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Receive: payload exceeds limit (line 354)
    // -----------------------------------------------------------------------

    #[test]
    fn test_receive_payload_exceeds_limit() {
        let (fd0, fd1) = socketpair_seqpacket();
        let mut session = test_session(fd0, Role::Client, 4096);
        session.max_response_payload_bytes = 16;
        session.inflight_ids.insert(99);

        let payload = [0xAB; 32];
        let hdr = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: protocol::HEADER_LEN,
            kind: KIND_RESPONSE,
            code: 1,
            flags: 0,
            transport_status: protocol::STATUS_OK,
            payload_len: payload.len() as u32,
            item_count: 1,
            message_id: 99,
        };
        let mut pkt = [0u8; HEADER_SIZE + 32];
        hdr.encode(&mut pkt[..HEADER_SIZE]);
        pkt[HEADER_SIZE..].copy_from_slice(&payload);
        raw_send(fd1, &pkt).expect("send oversized payload");

        let mut buf = [0u8; 64];
        let err = session
            .receive(&mut buf)
            .expect_err("payload exceeds limit");
        assert!(matches!(err, UdsError::LimitExceeded));

        unsafe { libc::close(fd1) };
    }

    // -----------------------------------------------------------------------
    //  Receive: batch item_count exceeds limit (line 364)
    // -----------------------------------------------------------------------

    #[test]
    fn test_receive_batch_count_exceeds_limit() {
        ensure_run_dir();
        let svc = unique_service("rs_batchmax");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let scfg = ServerConfig {
                max_request_batch_items: 100,
                max_response_batch_items: 100,
                ..default_server_config()
            };
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, scfg).expect("listen");
            ready_clone.store(true, Ordering::Release);

            let mut session = listener.accept().expect("accept");
            let mut buf = [0u8; 8192];
            let (hdr, _payload) = session.receive(&mut buf).expect("recv");

            // Send response with item_count = 100 (exceeds client's limit of 16)
            let mut resp = Header {
                kind: protocol::KIND_RESPONSE,
                code: hdr.code,
                message_id: hdr.message_id,
                item_count: 100,
                transport_status: protocol::STATUS_OK,
                ..Header::default()
            };
            session.send(&mut resp, &[]).expect("send");
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let ccfg = ClientConfig {
            max_response_batch_items: 16,
            ..default_client_config()
        };
        let mut session = UdsSession::connect(TEST_RUN_DIR, &svc, &ccfg).expect("connect");

        let mut hdr = Header {
            kind: protocol::KIND_REQUEST,
            code: 1,
            item_count: 1,
            message_id: 1,
            ..Header::default()
        };
        session.send(&mut hdr, &[1]).expect("send");

        let mut rbuf = [0u8; 8192];
        let result = session.receive(&mut rbuf);
        assert!(matches!(result, Err(UdsError::LimitExceeded)));

        drop(session);
        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Connect to nonexistent socket (line 220)
    // -----------------------------------------------------------------------

    #[test]
    fn test_connect_nonexistent() {
        ensure_run_dir();
        let svc = unique_service("rs_noexist");
        cleanup_socket(&svc);

        let result = UdsSession::connect(TEST_RUN_DIR, &svc, &default_client_config());
        assert!(matches!(result, Err(UdsError::Connect(_))));
    }

    #[test]
    fn test_send_packet_size_too_small_rolls_back_inflight() {
        let (fd0, fd1) = socketpair_seqpacket();
        let mut session = test_session(fd0, Role::Client, HEADER_SIZE as u32);

        let mut hdr = Header {
            kind: KIND_REQUEST,
            code: 1,
            item_count: 1,
            message_id: 77,
            ..Header::default()
        };

        let err = session
            .send(&mut hdr, &[0xAA])
            .expect_err("packet_size too small");
        assert!(matches!(err, UdsError::BadParam(_)));
        assert!(
            !session.inflight_ids.contains(&77),
            "failed send should roll back the tracked message_id"
        );

        unsafe { libc::close(fd1) };
    }

    #[test]
    fn test_receive_packet_too_short_for_header() {
        let (fd0, fd1) = socketpair_seqpacket();
        let mut session = test_session(fd0, Role::Server, 4096);

        raw_send(fd1, &[0x01]).expect("send short packet");

        let mut buf = [0u8; 64];
        let err = session.receive(&mut buf).expect_err("packet too short");
        assert!(matches!(err, UdsError::Protocol(_)));

        unsafe { libc::close(fd1) };
    }

    #[test]
    fn test_receive_batch_directory_too_short_nonchunked() {
        let (fd0, fd1) = socketpair_seqpacket();
        let mut session = test_session(fd0, Role::Server, 4096);

        let payload = [0u8; 8];
        let mut pkt = [0u8; HEADER_SIZE + 8];
        let hdr = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: protocol::HEADER_LEN,
            kind: KIND_REQUEST,
            code: 1,
            flags: FLAG_BATCH,
            transport_status: protocol::STATUS_OK,
            payload_len: payload.len() as u32,
            item_count: 2,
            message_id: 1,
        };
        hdr.encode(&mut pkt[..HEADER_SIZE]);
        pkt[HEADER_SIZE..].copy_from_slice(&payload);

        raw_send(fd1, &pkt).expect("send malformed batch packet");

        let mut buf = [0u8; 128];
        let err = session
            .receive(&mut buf)
            .expect_err("batch directory exceeds payload");
        assert!(matches!(err, UdsError::Protocol(_)));

        unsafe { libc::close(fd1) };
    }

    #[test]
    fn test_receive_batch_directory_invalid_nonchunked() {
        let (fd0, fd1) = socketpair_seqpacket();
        let mut session = test_session(fd0, Role::Server, 4096);

        let mut payload = [0u8; 16];
        payload[0..4].copy_from_slice(&0u32.to_ne_bytes());
        payload[4..8].copy_from_slice(&32u32.to_ne_bytes());
        payload[8..12].copy_from_slice(&0u32.to_ne_bytes());
        payload[12..16].copy_from_slice(&32u32.to_ne_bytes());

        let mut pkt = [0u8; HEADER_SIZE + 16];
        let hdr = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: protocol::HEADER_LEN,
            kind: KIND_REQUEST,
            code: 1,
            flags: FLAG_BATCH,
            transport_status: protocol::STATUS_OK,
            payload_len: payload.len() as u32,
            item_count: 2,
            message_id: 2,
        };
        hdr.encode(&mut pkt[..HEADER_SIZE]);
        pkt[HEADER_SIZE..].copy_from_slice(&payload);

        raw_send(fd1, &pkt).expect("send invalid batch packet");

        let mut buf = [0u8; 128];
        let err = session
            .receive(&mut buf)
            .expect_err("batch directory validation should fail");
        assert!(matches!(err, UdsError::Protocol(_)));

        unsafe { libc::close(fd1) };
    }

    #[test]
    fn test_receive_chunk_message_id_mismatch() {
        let (fd0, fd1) = socketpair_seqpacket();
        let mut session = test_session(fd0, Role::Server, (HEADER_SIZE + 10) as u32);

        let first_payload = [1u8; 10];
        let hdr = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: protocol::HEADER_LEN,
            kind: KIND_REQUEST,
            code: 1,
            flags: 0,
            transport_status: protocol::STATUS_OK,
            payload_len: 20,
            item_count: 1,
            message_id: 11,
        };
        let mut first_pkt = [0u8; HEADER_SIZE + 10];
        hdr.encode(&mut first_pkt[..HEADER_SIZE]);
        first_pkt[HEADER_SIZE..].copy_from_slice(&first_payload);
        raw_send(fd1, &first_pkt).expect("send first chunk");

        let second_payload = [2u8; 10];
        let chk = ChunkHeader {
            magic: MAGIC_CHUNK,
            version: VERSION,
            flags: 0,
            message_id: 12,
            total_message_len: (HEADER_SIZE + 20) as u32,
            chunk_index: 1,
            chunk_count: 2,
            chunk_payload_len: second_payload.len() as u32,
        };
        let mut second_pkt = [0u8; HEADER_SIZE + 10];
        chk.encode(&mut second_pkt[..HEADER_SIZE]);
        second_pkt[HEADER_SIZE..].copy_from_slice(&second_payload);
        raw_send(fd1, &second_pkt).expect("send mismatched continuation");

        let mut buf = [0u8; 64];
        let err = session.receive(&mut buf).expect_err("message_id mismatch");
        assert!(matches!(err, UdsError::Chunk(_)));

        unsafe { libc::close(fd1) };
    }

    #[test]
    fn test_receive_chunk_index_mismatch() {
        let (fd0, fd1) = socketpair_seqpacket();
        let mut session = test_session(fd0, Role::Server, (HEADER_SIZE + 10) as u32);

        let first_payload = [1u8; 10];
        let hdr = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: protocol::HEADER_LEN,
            kind: KIND_REQUEST,
            code: 1,
            flags: 0,
            transport_status: protocol::STATUS_OK,
            payload_len: 20,
            item_count: 1,
            message_id: 11,
        };
        let mut first_pkt = [0u8; HEADER_SIZE + 10];
        hdr.encode(&mut first_pkt[..HEADER_SIZE]);
        first_pkt[HEADER_SIZE..].copy_from_slice(&first_payload);
        raw_send(fd1, &first_pkt).expect("send first chunk");

        let second_payload = [2u8; 10];
        let chk = ChunkHeader {
            magic: MAGIC_CHUNK,
            version: VERSION,
            flags: 0,
            message_id: 11,
            total_message_len: (HEADER_SIZE + 20) as u32,
            chunk_index: 2,
            chunk_count: 2,
            chunk_payload_len: second_payload.len() as u32,
        };
        let mut second_pkt = [0u8; HEADER_SIZE + 10];
        chk.encode(&mut second_pkt[..HEADER_SIZE]);
        second_pkt[HEADER_SIZE..].copy_from_slice(&second_payload);
        raw_send(fd1, &second_pkt).expect("send mismatched continuation");

        let mut buf = [0u8; 64];
        let err = session.receive(&mut buf).expect_err("chunk index mismatch");
        assert!(matches!(err, UdsError::Chunk(_)));

        unsafe { libc::close(fd1) };
    }

    #[test]
    fn test_receive_chunked_batch_directory_too_short() {
        let (fd0, fd1) = socketpair_seqpacket();
        let mut session = test_session(fd0, Role::Server, (HEADER_SIZE + 8) as u32);

        let first_payload = [0u8; 8];
        let hdr = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: protocol::HEADER_LEN,
            kind: KIND_REQUEST,
            code: 1,
            flags: FLAG_BATCH,
            transport_status: protocol::STATUS_OK,
            payload_len: 12,
            item_count: 2,
            message_id: 21,
        };
        let mut first_pkt = [0u8; HEADER_SIZE + 8];
        hdr.encode(&mut first_pkt[..HEADER_SIZE]);
        first_pkt[HEADER_SIZE..].copy_from_slice(&first_payload);
        raw_send(fd1, &first_pkt).expect("send first chunk");

        let second_payload = [0u8; 4];
        let chk = ChunkHeader {
            magic: MAGIC_CHUNK,
            version: VERSION,
            flags: 0,
            message_id: 21,
            total_message_len: (HEADER_SIZE + 12) as u32,
            chunk_index: 1,
            chunk_count: 2,
            chunk_payload_len: second_payload.len() as u32,
        };
        let mut second_pkt = [0u8; HEADER_SIZE + 4];
        chk.encode(&mut second_pkt[..HEADER_SIZE]);
        second_pkt[HEADER_SIZE..].copy_from_slice(&second_payload);
        raw_send(fd1, &second_pkt).expect("send continuation");

        let mut buf = [0u8; 64];
        let err = session
            .receive(&mut buf)
            .expect_err("chunked batch directory too short");
        assert!(matches!(err, UdsError::Protocol(_)));

        unsafe { libc::close(fd1) };
    }

    #[test]
    fn test_receive_chunk_total_message_len_mismatch() {
        let (fd0, fd1) = socketpair_seqpacket();
        let mut session = test_session(fd0, Role::Server, (HEADER_SIZE + 10) as u32);

        let first_payload = [1u8; 10];
        let hdr = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: protocol::HEADER_LEN,
            kind: KIND_REQUEST,
            code: 1,
            flags: 0,
            transport_status: protocol::STATUS_OK,
            payload_len: 20,
            item_count: 1,
            message_id: 41,
        };
        let mut first_pkt = [0u8; HEADER_SIZE + 10];
        hdr.encode(&mut first_pkt[..HEADER_SIZE]);
        first_pkt[HEADER_SIZE..].copy_from_slice(&first_payload);
        raw_send(fd1, &first_pkt).expect("send first chunk");

        let second_payload = [2u8; 10];
        let chk = ChunkHeader {
            magic: MAGIC_CHUNK,
            version: VERSION,
            flags: 0,
            message_id: 41,
            total_message_len: (HEADER_SIZE + 21) as u32,
            chunk_index: 1,
            chunk_count: 2,
            chunk_payload_len: second_payload.len() as u32,
        };
        let mut second_pkt = [0u8; HEADER_SIZE + 10];
        chk.encode(&mut second_pkt[..HEADER_SIZE]);
        second_pkt[HEADER_SIZE..].copy_from_slice(&second_payload);
        raw_send(fd1, &second_pkt).expect("send bad total_message_len");

        let mut buf = [0u8; 64];
        let err = session
            .receive(&mut buf)
            .expect_err("total_message_len mismatch");
        assert!(matches!(err, UdsError::Chunk(_)));

        unsafe { libc::close(fd1) };
    }

    #[test]
    fn test_receive_chunk_payload_len_mismatch() {
        let (fd0, fd1) = socketpair_seqpacket();
        let mut session = test_session(fd0, Role::Server, (HEADER_SIZE + 10) as u32);

        let first_payload = [1u8; 10];
        let hdr = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: protocol::HEADER_LEN,
            kind: KIND_REQUEST,
            code: 1,
            flags: 0,
            transport_status: protocol::STATUS_OK,
            payload_len: 20,
            item_count: 1,
            message_id: 42,
        };
        let mut first_pkt = [0u8; HEADER_SIZE + 10];
        hdr.encode(&mut first_pkt[..HEADER_SIZE]);
        first_pkt[HEADER_SIZE..].copy_from_slice(&first_payload);
        raw_send(fd1, &first_pkt).expect("send first chunk");

        let second_payload = [2u8; 10];
        let chk = ChunkHeader {
            magic: MAGIC_CHUNK,
            version: VERSION,
            flags: 0,
            message_id: 42,
            total_message_len: (HEADER_SIZE + 20) as u32,
            chunk_index: 1,
            chunk_count: 2,
            chunk_payload_len: 9,
        };
        let mut second_pkt = [0u8; HEADER_SIZE + 10];
        chk.encode(&mut second_pkt[..HEADER_SIZE]);
        second_pkt[HEADER_SIZE..].copy_from_slice(&second_payload);
        raw_send(fd1, &second_pkt).expect("send bad chunk length");

        let mut buf = [0u8; 64];
        let err = session
            .receive(&mut buf)
            .expect_err("chunk_payload_len mismatch");
        assert!(matches!(err, UdsError::Chunk(_)));

        unsafe { libc::close(fd1) };
    }

    #[test]
    fn test_receive_continuation_too_short() {
        let (fd0, fd1) = socketpair_seqpacket();
        let mut session = test_session(fd0, Role::Server, (HEADER_SIZE + 10) as u32);

        let first_payload = [1u8; 10];
        let hdr = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: protocol::HEADER_LEN,
            kind: KIND_REQUEST,
            code: 1,
            flags: 0,
            transport_status: protocol::STATUS_OK,
            payload_len: 20,
            item_count: 1,
            message_id: 43,
        };
        let mut first_pkt = [0u8; HEADER_SIZE + 10];
        hdr.encode(&mut first_pkt[..HEADER_SIZE]);
        first_pkt[HEADER_SIZE..].copy_from_slice(&first_payload);
        raw_send(fd1, &first_pkt).expect("send first chunk");
        raw_send(fd1, &[0x01]).expect("send truncated continuation");

        let mut buf = [0u8; 64];
        let err = session
            .receive(&mut buf)
            .expect_err("continuation too short");
        assert!(matches!(err, UdsError::Chunk(_)));

        unsafe { libc::close(fd1) };
    }

    #[test]
    fn test_receive_chunk_count_mismatch() {
        let (fd0, fd1) = socketpair_seqpacket();
        let mut session = test_session(fd0, Role::Server, (HEADER_SIZE + 10) as u32);

        let first_payload = [1u8; 10];
        let hdr = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: protocol::HEADER_LEN,
            kind: KIND_REQUEST,
            code: 1,
            flags: 0,
            transport_status: protocol::STATUS_OK,
            payload_len: 20,
            item_count: 1,
            message_id: 44,
        };
        let mut first_pkt = [0u8; HEADER_SIZE + 10];
        hdr.encode(&mut first_pkt[..HEADER_SIZE]);
        first_pkt[HEADER_SIZE..].copy_from_slice(&first_payload);
        raw_send(fd1, &first_pkt).expect("send first chunk");

        let second_payload = [2u8; 10];
        let chk = ChunkHeader {
            magic: MAGIC_CHUNK,
            version: VERSION,
            flags: 0,
            message_id: 44,
            total_message_len: (HEADER_SIZE + 20) as u32,
            chunk_index: 1,
            chunk_count: 3,
            chunk_payload_len: second_payload.len() as u32,
        };
        let mut second_pkt = [0u8; HEADER_SIZE + 10];
        chk.encode(&mut second_pkt[..HEADER_SIZE]);
        second_pkt[HEADER_SIZE..].copy_from_slice(&second_payload);
        raw_send(fd1, &second_pkt).expect("send bad chunk count");

        let mut buf = [0u8; 64];
        let err = session.receive(&mut buf).expect_err("chunk_count mismatch");
        assert!(matches!(err, UdsError::Chunk(_)));

        unsafe { libc::close(fd1) };
    }

    #[test]
    fn test_receive_chunk_exceeds_payload_len() {
        let (fd0, fd1) = socketpair_seqpacket();
        let mut session = test_session(fd0, Role::Server, (HEADER_SIZE + 10) as u32);

        let first_payload = [1u8; 10];
        let hdr = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: protocol::HEADER_LEN,
            kind: KIND_REQUEST,
            code: 1,
            flags: 0,
            transport_status: protocol::STATUS_OK,
            payload_len: 15,
            item_count: 1,
            message_id: 45,
        };
        let mut first_pkt = [0u8; HEADER_SIZE + 10];
        hdr.encode(&mut first_pkt[..HEADER_SIZE]);
        first_pkt[HEADER_SIZE..].copy_from_slice(&first_payload);
        raw_send(fd1, &first_pkt).expect("send first chunk");

        let second_payload = [2u8; 10];
        let chk = ChunkHeader {
            magic: MAGIC_CHUNK,
            version: VERSION,
            flags: 0,
            message_id: 45,
            total_message_len: (HEADER_SIZE + 15) as u32,
            chunk_index: 1,
            chunk_count: 2,
            chunk_payload_len: second_payload.len() as u32,
        };
        let mut second_pkt = [0u8; HEADER_SIZE + 10];
        chk.encode(&mut second_pkt[..HEADER_SIZE]);
        second_pkt[HEADER_SIZE..].copy_from_slice(&second_payload);
        raw_send(fd1, &second_pkt).expect("send oversized continuation");

        let mut buf = [0u8; 64];
        let err = session
            .receive(&mut buf)
            .expect_err("chunk exceeds payload_len");
        assert!(matches!(err, UdsError::Chunk(_)));

        unsafe { libc::close(fd1) };
    }

    #[test]
    fn test_bind_rejects_live_server_addr_in_use() {
        ensure_run_dir();
        let svc = unique_service("rs_live_bind");
        cleanup_socket(&svc);

        let listener =
            UdsListener::bind(TEST_RUN_DIR, &svc, default_server_config()).expect("first bind");
        let result = UdsListener::bind(TEST_RUN_DIR, &svc, default_server_config());
        assert!(matches!(result, Err(UdsError::AddrInUse)));

        drop(listener);
        cleanup_socket(&svc);
    }

    #[test]
    fn test_bind_missing_run_dir_fails() {
        let svc = unique_service("rs_missing_bind");
        let bad_run_dir = "/tmp/nipc_rust_missing_parent/does/not/exist";
        let err = match UdsListener::bind(bad_run_dir, &svc, default_server_config()) {
            Ok(_) => panic!("missing parent bind should fail"),
            Err(err) => err,
        };
        assert!(matches!(err, UdsError::Socket(_)));
    }

    #[test]
    fn test_profile_preference_selects_shm_futex() {
        ensure_run_dir();
        let svc = unique_service("rs_pref_prof");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let scfg = ServerConfig {
                supported_profiles: PROFILE_BASELINE | protocol::PROFILE_SHM_FUTEX,
                preferred_profiles: protocol::PROFILE_SHM_FUTEX,
                ..default_server_config()
            };
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, scfg).expect("listen");
            ready_clone.store(true, Ordering::Release);
            let session = listener.accept().expect("accept");
            assert_eq!(session.selected_profile, protocol::PROFILE_SHM_FUTEX);
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let ccfg = ClientConfig {
            supported_profiles: PROFILE_BASELINE | protocol::PROFILE_SHM_FUTEX,
            preferred_profiles: protocol::PROFILE_SHM_FUTEX,
            ..default_client_config()
        };
        let session = UdsSession::connect(TEST_RUN_DIR, &svc, &ccfg).expect("connect");
        assert_eq!(session.selected_profile, protocol::PROFILE_SHM_FUTEX);

        drop(session);
        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    #[test]
    fn test_zero_supported_profiles_defaults_to_baseline() {
        ensure_run_dir();
        let svc = unique_service("rs_default_profiles");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let scfg = ServerConfig {
                supported_profiles: 0,
                preferred_profiles: 0,
                ..default_server_config()
            };
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, scfg).expect("listen");
            ready_clone.store(true, Ordering::Release);
            let session = listener.accept().expect("accept");
            assert_eq!(session.selected_profile, PROFILE_BASELINE);
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let ccfg = ClientConfig {
            supported_profiles: 0,
            preferred_profiles: 0,
            ..default_client_config()
        };
        let session = UdsSession::connect(TEST_RUN_DIR, &svc, &ccfg).expect("connect");
        assert_eq!(session.selected_profile, PROFILE_BASELINE);

        drop(session);
        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    #[test]
    fn test_bind_zero_backlog_uses_default() {
        ensure_run_dir();
        let svc = unique_service("rs_default_backlog");
        cleanup_socket(&svc);

        let listener = UdsListener::bind(
            TEST_RUN_DIR,
            &svc,
            ServerConfig {
                backlog: 0,
                ..default_server_config()
            },
        )
        .expect("bind with default backlog");

        drop(listener);
        cleanup_socket(&svc);
    }

    #[test]
    fn test_connect_rejects_bad_hello_ack_kind() {
        ensure_run_dir();
        let svc = unique_service("rs_bad_ack_kind");
        cleanup_socket(&svc);

        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();
        let svc_clone = svc.clone();
        let server = thread::spawn(move || {
            let fd = raw_listener_fd(&svc_clone);
            ready_clone.store(true, Ordering::Release);
            let client_fd = unsafe { libc::accept(fd, std::ptr::null_mut(), std::ptr::null_mut()) };
            assert!(client_fd >= 0, "accept failed: {}", errno());

            let mut hello = [0u8; 128];
            let n = raw_recv(client_fd, &mut hello).expect("recv hello");
            assert!(n >= HEADER_SIZE + HELLO_PAYLOAD_SIZE);

            let ack = HelloAck {
                layout_version: 1,
                ..HelloAck::default()
            };
            let mut ack_buf = [0u8; HELLO_ACK_PAYLOAD_SIZE];
            ack.encode(&mut ack_buf);
            let hdr = Header {
                magic: MAGIC_MSG,
                version: VERSION,
                header_len: protocol::HEADER_LEN,
                kind: KIND_RESPONSE,
                flags: 0,
                code: protocol::CODE_HELLO_ACK,
                transport_status: protocol::STATUS_OK,
                payload_len: HELLO_ACK_PAYLOAD_SIZE as u32,
                item_count: 1,
                message_id: 0,
            };
            let mut pkt = [0u8; HEADER_SIZE + HELLO_ACK_PAYLOAD_SIZE];
            hdr.encode(&mut pkt[..HEADER_SIZE]);
            pkt[HEADER_SIZE..].copy_from_slice(&ack_buf);
            raw_send(client_fd, &pkt).expect("send malformed ack");

            unsafe {
                libc::close(client_fd);
                libc::close(fd);
            }
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let err = match UdsSession::connect(TEST_RUN_DIR, &svc, &default_client_config()) {
            Ok(_) => panic!("bad hello ack kind should fail"),
            Err(err) => err,
        };
        assert!(matches!(err, UdsError::Protocol(_)));

        server.join().expect("server join");
        cleanup_socket(&svc);
    }

    #[test]
    fn test_connect_rejects_bad_hello_ack_status() {
        ensure_run_dir();
        let svc = unique_service("rs_bad_ack_status");
        cleanup_socket(&svc);

        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();
        let svc_clone = svc.clone();
        let server = thread::spawn(move || {
            let fd = raw_listener_fd(&svc_clone);
            ready_clone.store(true, Ordering::Release);
            let client_fd = unsafe { libc::accept(fd, std::ptr::null_mut(), std::ptr::null_mut()) };
            assert!(client_fd >= 0, "accept failed: {}", errno());

            let mut hello = [0u8; 128];
            let _ = raw_recv(client_fd, &mut hello).expect("recv hello");

            let ack = HelloAck {
                layout_version: 1,
                ..HelloAck::default()
            };
            let mut ack_buf = [0u8; HELLO_ACK_PAYLOAD_SIZE];
            ack.encode(&mut ack_buf);
            let hdr = Header {
                magic: MAGIC_MSG,
                version: VERSION,
                header_len: protocol::HEADER_LEN,
                kind: protocol::KIND_CONTROL,
                flags: 0,
                code: protocol::CODE_HELLO_ACK,
                transport_status: 0x7777,
                payload_len: HELLO_ACK_PAYLOAD_SIZE as u32,
                item_count: 1,
                message_id: 0,
            };
            let mut pkt = [0u8; HEADER_SIZE + HELLO_ACK_PAYLOAD_SIZE];
            hdr.encode(&mut pkt[..HEADER_SIZE]);
            pkt[HEADER_SIZE..].copy_from_slice(&ack_buf);
            raw_send(client_fd, &pkt).expect("send malformed ack");

            unsafe {
                libc::close(client_fd);
                libc::close(fd);
            }
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let err = match UdsSession::connect(TEST_RUN_DIR, &svc, &default_client_config()) {
            Ok(_) => panic!("bad hello ack status should fail"),
            Err(err) => err,
        };
        assert!(matches!(err, UdsError::Handshake(_)));

        server.join().expect("server join");
        cleanup_socket(&svc);
    }

    #[test]
    fn test_connect_rejects_truncated_hello_ack() {
        ensure_run_dir();
        let svc = unique_service("rs_bad_ack_trunc");
        cleanup_socket(&svc);

        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();
        let svc_clone = svc.clone();
        let server = thread::spawn(move || {
            let fd = raw_listener_fd(&svc_clone);
            ready_clone.store(true, Ordering::Release);
            let client_fd = unsafe { libc::accept(fd, std::ptr::null_mut(), std::ptr::null_mut()) };
            assert!(client_fd >= 0, "accept failed: {}", errno());

            let mut hello = [0u8; 128];
            let _ = raw_recv(client_fd, &mut hello).expect("recv hello");

            let hdr = Header {
                magic: MAGIC_MSG,
                version: VERSION,
                header_len: protocol::HEADER_LEN,
                kind: protocol::KIND_CONTROL,
                flags: 0,
                code: protocol::CODE_HELLO_ACK,
                transport_status: protocol::STATUS_OK,
                payload_len: HELLO_ACK_PAYLOAD_SIZE as u32,
                item_count: 1,
                message_id: 0,
            };
            let mut pkt = [0u8; HEADER_SIZE];
            hdr.encode(&mut pkt);
            raw_send(client_fd, &pkt).expect("send truncated ack");

            unsafe {
                libc::close(client_fd);
                libc::close(fd);
            }
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let err = match UdsSession::connect(TEST_RUN_DIR, &svc, &default_client_config()) {
            Ok(_) => panic!("truncated hello ack should fail"),
            Err(err) => err,
        };
        assert!(matches!(err, UdsError::Protocol(_)));

        server.join().expect("server join");
        cleanup_socket(&svc);
    }

    #[test]
    fn test_accept_rejects_bad_hello_kind() {
        ensure_run_dir();
        let svc = unique_service("rs_bad_hello_kind");
        cleanup_socket(&svc);

        let listener =
            UdsListener::bind(TEST_RUN_DIR, &svc, default_server_config()).expect("bind listener");

        let svc_clone = svc.clone();
        let client = thread::spawn(move || {
            let path = build_socket_path(TEST_RUN_DIR, &svc_clone).expect("socket path");
            let fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET, 0) };
            assert!(fd >= 0, "socket failed: {}", errno());
            connect_unix(fd, &path).expect("connect");

            let hdr = Header {
                magic: MAGIC_MSG,
                version: VERSION,
                header_len: protocol::HEADER_LEN,
                kind: KIND_REQUEST,
                flags: 0,
                code: protocol::CODE_HELLO,
                transport_status: protocol::STATUS_OK,
                payload_len: HELLO_PAYLOAD_SIZE as u32,
                item_count: 1,
                message_id: 0,
            };
            let mut payload = [0u8; HELLO_PAYLOAD_SIZE];
            let hello = Hello {
                layout_version: 1,
                supported_profiles: PROFILE_BASELINE,
                max_request_payload_bytes: 4096,
                max_request_batch_items: 16,
                max_response_payload_bytes: 4096,
                max_response_batch_items: 16,
                auth_token: AUTH_TOKEN,
                packet_size: DEFAULT_PACKET_SIZE_FALLBACK,
                ..Hello::default()
            };
            hello.encode(&mut payload);
            let mut pkt = [0u8; HEADER_SIZE + HELLO_PAYLOAD_SIZE];
            hdr.encode(&mut pkt[..HEADER_SIZE]);
            pkt[HEADER_SIZE..].copy_from_slice(&payload);
            raw_send(fd, &pkt).expect("send malformed hello");
            unsafe { libc::close(fd) };
        });

        let err = match listener.accept() {
            Ok(_) => panic!("bad hello kind should fail"),
            Err(err) => err,
        };
        assert!(matches!(err, UdsError::Protocol(_)));

        client.join().expect("client join");
        drop(listener);
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Receive: unknown response message_id (line 370)
    // -----------------------------------------------------------------------

    #[test]
    fn test_unknown_response_msg_id() {
        ensure_run_dir();
        let svc = unique_service("rs_unkmsg");
        cleanup_socket(&svc);

        let svc_clone = svc.clone();
        let ready = Arc::new(AtomicBool::new(false));
        let ready_clone = ready.clone();

        let server_thread = thread::spawn(move || {
            let listener = UdsListener::bind(TEST_RUN_DIR, &svc_clone, default_server_config())
                .expect("listen");
            ready_clone.store(true, Ordering::Release);

            let mut session = listener.accept().expect("accept");
            let mut buf = [0u8; 8192];
            let (hdr, payload) = session.receive(&mut buf).expect("recv");
            let payload = payload.to_vec();

            // Respond with a different message_id
            let mut resp = Header {
                kind: protocol::KIND_RESPONSE,
                code: hdr.code,
                message_id: hdr.message_id + 999, // wrong message_id
                item_count: 1,
                transport_status: protocol::STATUS_OK,
                ..Header::default()
            };
            session.send(&mut resp, &payload).expect("send");
        });

        while !ready.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(1));
        }

        let mut session =
            UdsSession::connect(TEST_RUN_DIR, &svc, &default_client_config()).expect("connect");

        let mut hdr = Header {
            kind: protocol::KIND_REQUEST,
            code: 1,
            item_count: 1,
            message_id: 50,
            ..Header::default()
        };
        session.send(&mut hdr, &[1]).expect("send");

        let mut rbuf = [0u8; 8192];
        let result = session.receive(&mut rbuf);
        assert!(matches!(result, Err(UdsError::UnknownMsgId(_))));

        drop(session);
        server_thread.join().expect("server join");
        cleanup_socket(&svc);
    }

    // -----------------------------------------------------------------------
    //  Path length validation
    // -----------------------------------------------------------------------

    #[test]
    fn test_path_too_long() {
        let long_name = "a".repeat(200);
        let result = build_socket_path("/tmp", &long_name);
        assert!(matches!(result, Err(UdsError::PathTooLong)));
    }

    // -----------------------------------------------------------------------
    //  Helpers: highest_bit, apply_default
    // -----------------------------------------------------------------------

    #[test]
    fn test_highest_bit() {
        assert_eq!(highest_bit(0), 0);
        assert_eq!(highest_bit(1), 1);
        assert_eq!(highest_bit(0b0101), 4);
        assert_eq!(highest_bit(0b1000), 8);
        assert_eq!(highest_bit(0xFF), 128);
    }

    #[test]
    fn test_apply_default() {
        assert_eq!(apply_default(0, 42), 42);
        assert_eq!(apply_default(10, 42), 10);
    }
}
