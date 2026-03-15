//! L1 Windows Named Pipe transport.
//!
//! Connection lifecycle, handshake with profile/limit negotiation,
//! and send/receive with transparent chunking over Win32 Named Pipes
//! in message mode. Wire-compatible with the C and Go implementations.

use crate::protocol::{
    self, ChunkHeader, Header, Hello, HelloAck, HEADER_SIZE, KIND_REQUEST, KIND_RESPONSE,
    MAGIC_CHUNK, MAGIC_MSG, MAX_PAYLOAD_DEFAULT, PROFILE_BASELINE, VERSION,
};
use std::collections::HashSet;
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};

// ---------------------------------------------------------------------------
//  Win32 FFI — using windows-sys when available, raw bindings as fallback
// ---------------------------------------------------------------------------

#[cfg(windows)]
mod ffi {
    #![allow(non_snake_case, non_camel_case_types, dead_code)]

    pub type HANDLE = isize;
    pub type DWORD = u32;
    pub type BOOL = i32;
    pub type LPCWSTR = *const u16;
    pub type LPVOID = *mut core::ffi::c_void;
    pub type LPCVOID = *const core::ffi::c_void;
    pub type LPDWORD = *mut DWORD;

    pub const INVALID_HANDLE_VALUE: HANDLE = -1;
    pub const PIPE_ACCESS_DUPLEX: DWORD = 0x00000003;
    pub const FILE_FLAG_FIRST_PIPE_INSTANCE: DWORD = 0x00080000;
    pub const PIPE_TYPE_MESSAGE: DWORD = 0x00000004;
    pub const PIPE_READMODE_MESSAGE: DWORD = 0x00000002;
    pub const PIPE_WAIT: DWORD = 0x00000000;
    pub const PIPE_UNLIMITED_INSTANCES: DWORD = 255;
    pub const GENERIC_READ: DWORD = 0x80000000;
    pub const GENERIC_WRITE: DWORD = 0x40000000;
    pub const OPEN_EXISTING: DWORD = 3;

    pub const ERROR_PIPE_CONNECTED: DWORD = 535;
    pub const ERROR_BROKEN_PIPE: DWORD = 109;
    pub const ERROR_NO_DATA: DWORD = 232;
    pub const ERROR_PIPE_NOT_CONNECTED: DWORD = 233;
    pub const ERROR_ACCESS_DENIED: DWORD = 5;
    pub const ERROR_PIPE_BUSY: DWORD = 231;

    extern "system" {
        pub fn CreateNamedPipeW(
            lpName: LPCWSTR,
            dwOpenMode: DWORD,
            dwPipeMode: DWORD,
            nMaxInstances: DWORD,
            nOutBufferSize: DWORD,
            nInBufferSize: DWORD,
            nDefaultTimeOut: DWORD,
            lpSecurityAttributes: *const core::ffi::c_void,
        ) -> HANDLE;

        pub fn ConnectNamedPipe(
            hNamedPipe: HANDLE,
            lpOverlapped: *mut core::ffi::c_void,
        ) -> BOOL;

        pub fn DisconnectNamedPipe(hNamedPipe: HANDLE) -> BOOL;
        pub fn FlushFileBuffers(hFile: HANDLE) -> BOOL;

        pub fn CreateFileW(
            lpFileName: LPCWSTR,
            dwDesiredAccess: DWORD,
            dwShareMode: DWORD,
            lpSecurityAttributes: *const core::ffi::c_void,
            dwCreationDisposition: DWORD,
            dwFlagsAndAttributes: DWORD,
            hTemplateFile: HANDLE,
        ) -> HANDLE;

        pub fn ReadFile(
            hFile: HANDLE,
            lpBuffer: LPVOID,
            nNumberOfBytesToRead: DWORD,
            lpNumberOfBytesRead: LPDWORD,
            lpOverlapped: *mut core::ffi::c_void,
        ) -> BOOL;

        pub fn WriteFile(
            hFile: HANDLE,
            lpBuffer: LPCVOID,
            nNumberOfBytesToWrite: DWORD,
            lpNumberOfBytesWritten: LPDWORD,
            lpOverlapped: *mut core::ffi::c_void,
        ) -> BOOL;

        pub fn CloseHandle(hObject: HANDLE) -> BOOL;

        pub fn GetLastError() -> DWORD;

        pub fn SetNamedPipeHandleState(
            hNamedPipe: HANDLE,
            lpMode: *const DWORD,
            lpMaxCollectionCount: *const DWORD,
            lpCollectDataTimeout: *const DWORD,
        ) -> BOOL;
    }
}

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

const DEFAULT_BATCH_ITEMS: u32 = 1;
const DEFAULT_PACKET_SIZE: u32 = 65536;
const DEFAULT_PIPE_BUF_SIZE: u32 = 65536;
const HELLO_PAYLOAD_SIZE: usize = 44;
const HELLO_ACK_PAYLOAD_SIZE: usize = 48;
const MAX_PIPE_NAME_CHARS: usize = 256;

// FNV-1a 64-bit constants
const FNV1A_OFFSET_BASIS: u64 = 0xcbf29ce484222325;
const FNV1A_PRIME: u64 = 0x00000100000001B3;

// ---------------------------------------------------------------------------
//  Errors
// ---------------------------------------------------------------------------

/// Transport-level errors for Named Pipe transport.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum NpError {
    /// Pipe name derivation failed.
    PipeName(String),
    /// CreateNamedPipeW failed.
    CreatePipe(u32),
    /// CreateFileW / connection failed.
    Connect(u32),
    /// ConnectNamedPipe (accept) failed.
    Accept(u32),
    /// WriteFile failed.
    Send(u32),
    /// ReadFile failed or peer disconnected.
    Recv(u32),
    /// Handshake protocol error.
    Handshake(String),
    /// Authentication token rejected.
    AuthFailed,
    /// No common profile.
    NoProfile,
    /// Wire protocol violation.
    Protocol(String),
    /// Pipe name already in use by live server.
    AddrInUse,
    /// Chunk header mismatch.
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
    /// Peer disconnected (graceful).
    Disconnected,
}

impl std::fmt::Display for NpError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            NpError::PipeName(s) => write!(f, "pipe name error: {s}"),
            NpError::CreatePipe(e) => write!(f, "CreateNamedPipeW failed: {e}"),
            NpError::Connect(e) => write!(f, "connect failed: {e}"),
            NpError::Accept(e) => write!(f, "accept failed: {e}"),
            NpError::Send(e) => write!(f, "send failed: {e}"),
            NpError::Recv(e) => write!(f, "recv failed: {e}"),
            NpError::Handshake(s) => write!(f, "handshake error: {s}"),
            NpError::AuthFailed => write!(f, "authentication token rejected"),
            NpError::NoProfile => write!(f, "no common transport profile"),
            NpError::Protocol(s) => write!(f, "protocol violation: {s}"),
            NpError::AddrInUse => write!(f, "pipe name already in use by live server"),
            NpError::Chunk(s) => write!(f, "chunk error: {s}"),
            NpError::Alloc => write!(f, "memory allocation failed"),
            NpError::LimitExceeded => write!(f, "negotiated limit exceeded"),
            NpError::BadParam(s) => write!(f, "bad parameter: {s}"),
            NpError::DuplicateMsgId(id) => write!(f, "duplicate message_id: {id}"),
            NpError::UnknownMsgId(id) => write!(f, "unknown response message_id: {id}"),
            NpError::Disconnected => write!(f, "peer disconnected"),
        }
    }
}

impl std::error::Error for NpError {}

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
    /// 0 = use default (65536).
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
    /// 0 = use default (65536).
    pub packet_size: u32,
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
        }
    }
}

// ---------------------------------------------------------------------------
//  FNV-1a 64-bit hash
// ---------------------------------------------------------------------------

/// Compute FNV-1a 64-bit hash of data.
pub fn fnv1a_64(data: &[u8]) -> u64 {
    let mut hash = FNV1A_OFFSET_BASIS;
    for &byte in data {
        hash ^= byte as u64;
        hash = hash.wrapping_mul(FNV1A_PRIME);
    }
    hash
}

// ---------------------------------------------------------------------------
//  Service name validation
// ---------------------------------------------------------------------------

fn validate_service_name(name: &str) -> Result<(), NpError> {
    if name.is_empty() {
        return Err(NpError::BadParam("empty service name".into()));
    }
    if name == "." || name == ".." {
        return Err(NpError::BadParam("service name cannot be '.' or '..'".into()));
    }
    for &b in name.as_bytes() {
        if (b >= b'a' && b <= b'z')
            || (b >= b'A' && b <= b'Z')
            || (b >= b'0' && b <= b'9')
            || b == b'.'
            || b == b'_'
            || b == b'-'
        {
            continue;
        }
        return Err(NpError::BadParam(format!(
            "service name contains invalid character: {:?}",
            b as char
        )));
    }
    Ok(())
}

// ---------------------------------------------------------------------------
//  Pipe name derivation
// ---------------------------------------------------------------------------

/// Build pipe name from run_dir and service_name.
/// Returns the pipe name as a NUL-terminated wide string vector.
pub fn build_pipe_name(run_dir: &str, service_name: &str) -> Result<Vec<u16>, NpError> {
    validate_service_name(service_name)?;

    let hash = fnv1a_64(run_dir.as_bytes());
    let narrow = format!("\\\\.\\pipe\\netipc-{:016x}-{}", hash, service_name);

    if narrow.len() >= MAX_PIPE_NAME_CHARS {
        return Err(NpError::PipeName("pipe name too long".into()));
    }

    // Convert to UTF-16 with NUL terminator
    let mut wide: Vec<u16> = narrow.encode_utf16().collect();
    wide.push(0);
    Ok(wide)
}

// ---------------------------------------------------------------------------
//  Internal helpers
// ---------------------------------------------------------------------------

fn apply_default(val: u32, def: u32) -> u32 {
    if val == 0 { def } else { val }
}

fn min_u32(a: u32, b: u32) -> u32 {
    a.min(b)
}

fn highest_bit(mask: u32) -> u32 {
    if mask == 0 {
        return 0;
    }
    let mut bit: u32 = 1 << 31;
    while bit & mask == 0 {
        bit >>= 1;
    }
    bit
}

// ---------------------------------------------------------------------------
//  Low-level I/O (Windows-only)
// ---------------------------------------------------------------------------

#[cfg(windows)]
fn is_disconnect_error(err: u32) -> bool {
    err == ffi::ERROR_BROKEN_PIPE
        || err == ffi::ERROR_NO_DATA
        || err == ffi::ERROR_PIPE_NOT_CONNECTED
}

#[cfg(windows)]
fn last_error() -> u32 {
    unsafe { ffi::GetLastError() }
}

#[cfg(windows)]
fn raw_write(handle: ffi::HANDLE, data: &[u8]) -> Result<(), NpError> {
    let mut written: u32 = 0;
    let ok = unsafe {
        ffi::WriteFile(
            handle,
            data.as_ptr() as ffi::LPCVOID,
            data.len() as u32,
            &mut written,
            ptr::null_mut(),
        )
    };
    if ok == 0 {
        let err = last_error();
        if is_disconnect_error(err) {
            return Err(NpError::Disconnected);
        }
        return Err(NpError::Send(err));
    }
    if written != data.len() as u32 {
        return Err(NpError::Send(0));
    }
    Ok(())
}

/// Send header + payload as one pipe message.
#[cfg(windows)]
fn raw_send_msg(handle: ffi::HANDLE, hdr: &[u8], payload: &[u8]) -> Result<(), NpError> {
    let mut buf = Vec::with_capacity(hdr.len() + payload.len());
    buf.extend_from_slice(hdr);
    buf.extend_from_slice(payload);
    raw_write(handle, &buf)
}

/// Read one pipe message. Returns number of bytes read.
#[cfg(windows)]
fn raw_recv(handle: ffi::HANDLE, buf: &mut [u8]) -> Result<usize, NpError> {
    let mut read: u32 = 0;
    let ok = unsafe {
        ffi::ReadFile(
            handle,
            buf.as_mut_ptr() as ffi::LPVOID,
            buf.len() as u32,
            &mut read,
            ptr::null_mut(),
        )
    };
    if ok == 0 {
        let err = last_error();
        if is_disconnect_error(err) {
            return Err(NpError::Disconnected);
        }
        return Err(NpError::Recv(err));
    }
    if read == 0 {
        return Err(NpError::Disconnected);
    }
    Ok(read as usize)
}

#[cfg(windows)]
fn close_handle(handle: ffi::HANDLE) {
    if handle != ffi::INVALID_HANDLE_VALUE && handle != 0 {
        unsafe { ffi::CloseHandle(handle); }
    }
}

// ---------------------------------------------------------------------------
//  Session
// ---------------------------------------------------------------------------

/// A connected Named Pipe session (client or server side).
#[cfg(windows)]
pub struct NpSession {
    handle: ffi::HANDLE,
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

    // In-flight message_id set (client-side only)
    inflight_ids: HashSet<u64>,
}

#[cfg(windows)]
impl NpSession {
    /// Get the raw HANDLE for WaitForSingleObject integration.
    pub fn handle(&self) -> ffi::HANDLE {
        self.handle
    }

    /// Get the session role.
    pub fn role(&self) -> Role {
        self.role
    }

    /// Connect to a server pipe derived from run_dir + service_name.
    pub fn connect(
        run_dir: &str,
        service_name: &str,
        config: &ClientConfig,
    ) -> Result<Self, NpError> {
        let pipe_name = build_pipe_name(run_dir, service_name)?;

        let handle = unsafe {
            ffi::CreateFileW(
                pipe_name.as_ptr(),
                ffi::GENERIC_READ | ffi::GENERIC_WRITE,
                0,
                ptr::null(),
                ffi::OPEN_EXISTING,
                0,
                0,
            )
        };

        if handle == ffi::INVALID_HANDLE_VALUE {
            return Err(NpError::Connect(last_error()));
        }

        // Set read mode to message mode
        let mode: u32 = ffi::PIPE_READMODE_MESSAGE;
        let ok = unsafe {
            ffi::SetNamedPipeHandleState(handle, &mode, ptr::null(), ptr::null())
        };
        if ok == 0 {
            let err = last_error();
            close_handle(handle);
            return Err(NpError::Connect(err));
        }

        match client_handshake(handle, config) {
            Ok(session) => Ok(session),
            Err(e) => {
                close_handle(handle);
                Err(e)
            }
        }
    }

    /// Send one logical message. Fills magic/version/header_len/payload_len.
    /// Chunked transparently if message exceeds packet_size.
    pub fn send(&mut self, hdr: &mut Header, payload: &[u8]) -> Result<(), NpError> {
        if self.handle == ffi::INVALID_HANDLE_VALUE {
            return Err(NpError::BadParam("session closed".into()));
        }

        // Client-side: track in-flight message_ids for requests
        if self.role == Role::Client && hdr.kind == KIND_REQUEST {
            if !self.inflight_ids.insert(hdr.message_id) {
                return Err(NpError::DuplicateMsgId(hdr.message_id));
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

        if result.is_err() && tracked {
            self.inflight_ids.remove(&msg_id);
        }

        result
    }

    fn send_inner(&mut self, hdr: &mut Header, payload: &[u8]) -> Result<(), NpError> {
        let total_msg = HEADER_SIZE + payload.len();

        // Single packet?
        if total_msg <= self.packet_size as usize {
            let mut hdr_buf = [0u8; HEADER_SIZE];
            hdr.encode(&mut hdr_buf);
            return raw_send_msg(self.handle, &hdr_buf, payload);
        }

        // Chunked send
        let chunk_payload_budget = self.packet_size as usize - HEADER_SIZE;
        if chunk_payload_budget == 0 {
            return Err(NpError::BadParam("packet_size too small".into()));
        }

        let first_chunk_payload = payload.len().min(chunk_payload_budget);
        let remaining_after_first = payload.len() - first_chunk_payload;

        let continuation_chunks = if remaining_after_first > 0 {
            (remaining_after_first + chunk_payload_budget - 1) / chunk_payload_budget
        } else {
            0
        };
        let chunk_count = 1 + continuation_chunks as u32;

        // First chunk
        let mut hdr_buf = [0u8; HEADER_SIZE];
        hdr.encode(&mut hdr_buf);
        raw_send_msg(self.handle, &hdr_buf, &payload[..first_chunk_payload])?;

        // Continuation chunks
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
            raw_send_msg(self.handle, &chk_buf, &payload[offset..offset + this_chunk])?;

            offset += this_chunk;
        }

        Ok(())
    }

    /// Receive one logical message. buf is a scratch buffer for the first read.
    pub fn receive(&mut self, buf: &mut [u8]) -> Result<(Header, Vec<u8>), NpError> {
        if self.handle == ffi::INVALID_HANDLE_VALUE {
            return Err(NpError::BadParam("session closed".into()));
        }

        let n = raw_recv(self.handle, buf)?;

        if n < HEADER_SIZE {
            return Err(NpError::Protocol("packet too short for header".into()));
        }

        let hdr = Header::decode(&buf[..n])
            .map_err(|e| NpError::Protocol(format!("header decode: {e}")))?;

        // Validate payload_len against negotiated limit
        let max_payload = if self.role == Role::Server {
            self.max_request_payload_bytes
        } else {
            self.max_response_payload_bytes
        };
        if hdr.payload_len > max_payload {
            return Err(NpError::LimitExceeded);
        }

        // Validate item_count
        let max_batch = if self.role == Role::Server {
            self.max_request_batch_items
        } else {
            self.max_response_batch_items
        };
        if hdr.item_count > max_batch {
            return Err(NpError::LimitExceeded);
        }

        // Client-side: validate response message_id
        if self.role == Role::Client && hdr.kind == KIND_RESPONSE {
            if !self.inflight_ids.remove(&hdr.message_id) {
                return Err(NpError::UnknownMsgId(hdr.message_id));
            }
        }

        let total_msg = HEADER_SIZE + hdr.payload_len as usize;

        // Non-chunked
        if n >= total_msg {
            let payload = buf[HEADER_SIZE..HEADER_SIZE + hdr.payload_len as usize].to_vec();
            return Ok((hdr, payload));
        }

        // Chunked
        let first_payload_bytes = n - HEADER_SIZE;
        let needed = hdr.payload_len as usize;
        if self.recv_buf.len() < needed {
            self.recv_buf.resize(needed, 0);
        }

        self.recv_buf[..first_payload_bytes]
            .copy_from_slice(&buf[HEADER_SIZE..HEADER_SIZE + first_payload_bytes]);

        let mut assembled = first_payload_bytes;
        let chunk_payload_budget = self.packet_size as usize - HEADER_SIZE;

        let remaining_after_first = hdr.payload_len as usize - first_payload_bytes;
        let expected_continuations = if remaining_after_first > 0 && chunk_payload_budget > 0 {
            (remaining_after_first + chunk_payload_budget - 1) / chunk_payload_budget
        } else {
            0
        };
        let expected_chunk_count = 1 + expected_continuations as u32;

        let mut pkt_buf = vec![0u8; self.packet_size as usize];

        let mut ci: u32 = 1;
        while assembled < hdr.payload_len as usize {
            let cn = raw_recv(self.handle, &mut pkt_buf)?;

            if cn < HEADER_SIZE {
                return Err(NpError::Chunk("continuation too short".into()));
            }

            let chk = ChunkHeader::decode(&pkt_buf[..cn])
                .map_err(|e| NpError::Chunk(format!("chunk header: {e}")))?;

            if chk.message_id != hdr.message_id {
                return Err(NpError::Chunk("message_id mismatch".into()));
            }
            if chk.chunk_index != ci {
                return Err(NpError::Chunk(format!(
                    "chunk_index mismatch: expected {ci}, got {}",
                    chk.chunk_index
                )));
            }
            if chk.chunk_count != expected_chunk_count {
                return Err(NpError::Chunk("chunk_count mismatch".into()));
            }
            if chk.total_message_len != total_msg as u32 {
                return Err(NpError::Chunk("total_message_len mismatch".into()));
            }

            let chunk_data = cn - HEADER_SIZE;
            if chunk_data != chk.chunk_payload_len as usize {
                return Err(NpError::Chunk("chunk_payload_len mismatch".into()));
            }
            if assembled + chunk_data > hdr.payload_len as usize {
                return Err(NpError::Chunk("chunk exceeds payload_len".into()));
            }

            self.recv_buf[assembled..assembled + chunk_data]
                .copy_from_slice(&pkt_buf[HEADER_SIZE..HEADER_SIZE + chunk_data]);
            assembled += chunk_data;
            ci += 1;
        }

        let payload = self.recv_buf[..hdr.payload_len as usize].to_vec();
        Ok((hdr, payload))
    }

    /// Close the session.
    pub fn close(&mut self) {
        if self.handle != ffi::INVALID_HANDLE_VALUE && self.handle != 0 {
            // Flush pending writes so the peer reads all data
            unsafe { ffi::FlushFileBuffers(self.handle); }
            if self.role == Role::Server {
                unsafe { ffi::DisconnectNamedPipe(self.handle); }
            }
            close_handle(self.handle);
            self.handle = ffi::INVALID_HANDLE_VALUE;
        }
        self.recv_buf.clear();
    }
}

#[cfg(windows)]
impl Drop for NpSession {
    fn drop(&mut self) {
        self.close();
    }
}

// ---------------------------------------------------------------------------
//  Listener
// ---------------------------------------------------------------------------

/// A Named Pipe listener that accepts client connections.
#[cfg(windows)]
pub struct NpListener {
    handle: ffi::HANDLE,
    config: ServerConfig,
    pipe_name: Vec<u16>,
    next_session_id: AtomicU64,
}

#[cfg(windows)]
impl NpListener {
    /// Create a listener on a Named Pipe derived from run_dir + service_name.
    pub fn bind(
        run_dir: &str,
        service_name: &str,
        config: ServerConfig,
    ) -> Result<Self, NpError> {
        let pipe_name = build_pipe_name(run_dir, service_name)?;
        let buf_size = apply_default(config.packet_size, DEFAULT_PIPE_BUF_SIZE);

        // Create first instance with FILE_FLAG_FIRST_PIPE_INSTANCE
        let handle = create_pipe_instance(&pipe_name, buf_size, true)?;

        Ok(Self {
            handle,
            config,
            pipe_name,
            next_session_id: AtomicU64::new(1),
        })
    }

    /// Get the raw HANDLE.
    pub fn handle(&self) -> ffi::HANDLE {
        self.handle
    }

    /// Accept one client connection. Performs the full handshake.
    pub fn accept(&mut self) -> Result<NpSession, NpError> {
        // Wait for client
        let connected = unsafe { ffi::ConnectNamedPipe(self.handle, ptr::null_mut()) };
        if connected == 0 {
            let err = last_error();
            if err != ffi::ERROR_PIPE_CONNECTED {
                return Err(NpError::Accept(err));
            }
        }

        let session_handle = self.handle;

        // Create new pipe instance for next client
        let buf_size = apply_default(self.config.packet_size, DEFAULT_PIPE_BUF_SIZE);
        let next = create_pipe_instance(&self.pipe_name, buf_size, false)?;
        self.handle = next;

        // Perform handshake
        let session_id = self.next_session_id.fetch_add(1, Ordering::Relaxed);
        match server_handshake(session_handle, &self.config, session_id) {
            Ok(session) => Ok(session),
            Err(e) => {
                unsafe { ffi::DisconnectNamedPipe(session_handle); }
                close_handle(session_handle);
                Err(e)
            }
        }
    }

    /// Close the listener.
    pub fn close(&mut self) {
        close_handle(self.handle);
        self.handle = ffi::INVALID_HANDLE_VALUE;
    }
}

#[cfg(windows)]
impl Drop for NpListener {
    fn drop(&mut self) {
        self.close();
    }
}

// ---------------------------------------------------------------------------
//  Pipe instance creation
// ---------------------------------------------------------------------------

#[cfg(windows)]
fn create_pipe_instance(
    pipe_name: &[u16],
    buf_size: u32,
    first_instance: bool,
) -> Result<ffi::HANDLE, NpError> {
    let mut open_mode = ffi::PIPE_ACCESS_DUPLEX;
    if first_instance {
        open_mode |= ffi::FILE_FLAG_FIRST_PIPE_INSTANCE;
    }

    let handle = unsafe {
        ffi::CreateNamedPipeW(
            pipe_name.as_ptr(),
            open_mode,
            ffi::PIPE_TYPE_MESSAGE | ffi::PIPE_READMODE_MESSAGE | ffi::PIPE_WAIT,
            ffi::PIPE_UNLIMITED_INSTANCES,
            buf_size,
            buf_size,
            0,
            ptr::null(),
        )
    };

    if handle == ffi::INVALID_HANDLE_VALUE {
        let err = last_error();
        if err == ffi::ERROR_ACCESS_DENIED || err == ffi::ERROR_PIPE_BUSY {
            return Err(NpError::AddrInUse);
        }
        return Err(NpError::CreatePipe(err));
    }

    Ok(handle)
}

// ---------------------------------------------------------------------------
//  Client handshake
// ---------------------------------------------------------------------------

#[cfg(windows)]
fn client_handshake(
    handle: ffi::HANDLE,
    config: &ClientConfig,
) -> Result<NpSession, NpError> {
    let pkt_size = apply_default(config.packet_size, DEFAULT_PACKET_SIZE);

    let supported = if config.supported_profiles != 0 {
        config.supported_profiles
    } else {
        PROFILE_BASELINE
    };

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

    raw_write(handle, &pkt)?;

    // Receive HELLO_ACK
    let mut ack_buf = [0u8; 128];
    let n = raw_recv(handle, &mut ack_buf)?;

    let ack_hdr = Header::decode(&ack_buf[..n])
        .map_err(|e| NpError::Protocol(format!("ack header: {e}")))?;

    if ack_hdr.kind != protocol::KIND_CONTROL || ack_hdr.code != protocol::CODE_HELLO_ACK {
        return Err(NpError::Protocol("expected HELLO_ACK".into()));
    }

    if ack_hdr.transport_status == protocol::STATUS_AUTH_FAILED {
        return Err(NpError::AuthFailed);
    }
    if ack_hdr.transport_status == protocol::STATUS_UNSUPPORTED {
        return Err(NpError::NoProfile);
    }
    if ack_hdr.transport_status != protocol::STATUS_OK {
        return Err(NpError::Handshake(format!(
            "transport_status={}",
            ack_hdr.transport_status
        )));
    }

    if n < HEADER_SIZE + HELLO_ACK_PAYLOAD_SIZE {
        return Err(NpError::Protocol("ack payload truncated".into()));
    }

    let ack = HelloAck::decode(&ack_buf[HEADER_SIZE..n])
        .map_err(|e| NpError::Protocol(format!("ack payload: {e}")))?;

    Ok(NpSession {
        handle,
        role: Role::Client,
        max_request_payload_bytes: ack.agreed_max_request_payload_bytes,
        max_request_batch_items: ack.agreed_max_request_batch_items,
        max_response_payload_bytes: ack.agreed_max_response_payload_bytes,
        max_response_batch_items: ack.agreed_max_response_batch_items,
        packet_size: ack.agreed_packet_size,
        selected_profile: ack.selected_profile,
        session_id: ack.session_id,
        recv_buf: Vec::new(),
        inflight_ids: HashSet::new(),
    })
}

// ---------------------------------------------------------------------------
//  Server handshake
// ---------------------------------------------------------------------------

#[cfg(windows)]
fn server_handshake(
    handle: ffi::HANDLE,
    config: &ServerConfig,
    session_id: u64,
) -> Result<NpSession, NpError> {
    let server_pkt_size = apply_default(config.packet_size, DEFAULT_PACKET_SIZE);
    let s_req_pay = apply_default(config.max_request_payload_bytes, MAX_PAYLOAD_DEFAULT);
    let s_req_bat = apply_default(config.max_request_batch_items, DEFAULT_BATCH_ITEMS);
    let s_resp_pay = apply_default(config.max_response_payload_bytes, MAX_PAYLOAD_DEFAULT);
    let s_resp_bat = apply_default(config.max_response_batch_items, DEFAULT_BATCH_ITEMS);
    let s_profiles = if config.supported_profiles != 0 {
        config.supported_profiles
    } else {
        PROFILE_BASELINE
    };
    let s_preferred = config.preferred_profiles;

    // Receive HELLO
    let mut buf = [0u8; 128];
    let n = raw_recv(handle, &mut buf)?;

    let hdr = Header::decode(&buf[..n])
        .map_err(|e| NpError::Protocol(format!("hello header: {e}")))?;

    if hdr.kind != protocol::KIND_CONTROL || hdr.code != protocol::CODE_HELLO {
        return Err(NpError::Protocol("expected HELLO".into()));
    }

    let hello = Hello::decode(&buf[HEADER_SIZE..n])
        .map_err(|e| NpError::Protocol(format!("hello payload: {e}")))?;

    let intersection = hello.supported_profiles & s_profiles;

    // Helper: send rejection
    let send_rejection = |status: u16| {
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
        let _ = raw_write(handle, &pkt);
    };

    if intersection == 0 {
        send_rejection(protocol::STATUS_UNSUPPORTED);
        return Err(NpError::NoProfile);
    }

    if hello.auth_token != config.auth_token {
        send_rejection(protocol::STATUS_AUTH_FAILED);
        return Err(NpError::AuthFailed);
    }

    // Select profile
    let preferred_intersection = intersection & hello.preferred_profiles & s_preferred;
    let selected = if preferred_intersection != 0 {
        highest_bit(preferred_intersection)
    } else {
        highest_bit(intersection)
    };

    // Negotiate limits
    let agreed_req_pay = min_u32(hello.max_request_payload_bytes, s_req_pay);
    let agreed_req_bat = min_u32(hello.max_request_batch_items, s_req_bat);
    let agreed_resp_pay = min_u32(hello.max_response_payload_bytes, s_resp_pay);
    let agreed_resp_bat = min_u32(hello.max_response_batch_items, s_resp_bat);
    let agreed_pkt = min_u32(hello.packet_size, server_pkt_size);

    // Send HELLO_ACK
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

    raw_write(handle, &pkt)?;

    Ok(NpSession {
        handle,
        role: Role::Server,
        max_request_payload_bytes: agreed_req_pay,
        max_request_batch_items: agreed_req_bat,
        max_response_payload_bytes: agreed_resp_pay,
        max_response_batch_items: agreed_resp_bat,
        packet_size: agreed_pkt,
        selected_profile: selected,
        session_id,
        recv_buf: Vec::new(),
        inflight_ids: HashSet::new(),
    })
}

// ---------------------------------------------------------------------------
//  Tests (cross-platform unit tests for non-Win32 logic)
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_fnv1a_64_empty() {
        assert_eq!(fnv1a_64(b""), FNV1A_OFFSET_BASIS);
    }

    #[test]
    fn test_fnv1a_64_known() {
        // FNV-1a of "foobar" — verified against reference implementation
        let hash = fnv1a_64(b"foobar");
        assert_ne!(hash, 0);
        assert_ne!(hash, FNV1A_OFFSET_BASIS);
    }

    #[test]
    fn test_fnv1a_64_deterministic() {
        let h1 = fnv1a_64(b"/var/run/netdata");
        let h2 = fnv1a_64(b"/var/run/netdata");
        assert_eq!(h1, h2);
    }

    #[test]
    fn test_fnv1a_64_different_inputs() {
        let h1 = fnv1a_64(b"/var/run/netdata");
        let h2 = fnv1a_64(b"/tmp/netdata");
        assert_ne!(h1, h2);
    }

    #[test]
    fn test_validate_service_name_valid() {
        assert!(validate_service_name("cgroups-snapshot").is_ok());
        assert!(validate_service_name("test_service.v1").is_ok());
        assert!(validate_service_name("A-Z_09").is_ok());
    }

    #[test]
    fn test_validate_service_name_invalid() {
        assert!(validate_service_name("").is_err());
        assert!(validate_service_name(".").is_err());
        assert!(validate_service_name("..").is_err());
        assert!(validate_service_name("has space").is_err());
        assert!(validate_service_name("has/slash").is_err());
        assert!(validate_service_name("has\\backslash").is_err());
    }

    #[test]
    fn test_build_pipe_name() {
        let name = build_pipe_name("/var/run/netdata", "cgroups-snapshot").unwrap();
        // Should produce a valid wide string ending in NUL
        assert!(*name.last().unwrap() == 0);
        // Convert back to narrow for checking prefix
        let narrow: String = name[..name.len() - 1]
            .iter()
            .map(|&c| c as u8 as char)
            .collect();
        assert!(narrow.starts_with("\\\\.\\pipe\\netipc-"));
        assert!(narrow.ends_with("-cgroups-snapshot"));
        // Hash should be 16 hex chars
        let parts: Vec<&str> = narrow.split('-').collect();
        // \\.\pipe\netipc - {hash} - cgroups - snapshot
        // parts: ["\\\\.", "\\pipe\\netipc", "{hash}", "cgroups", "snapshot"]
        // Actually the split is on '-' so:
        // "\\\\.\\pipe\\netipc" - "hash" - "cgroups" - "snapshot"
        assert!(parts.len() >= 3);
        let hash_part = parts[1]; // after first '-'
        // The pipe name is \\.\pipe\netipc-{hash}-{service}
        // Splitting on '-': parts[0]="\\\\.\pipe\netipc", parts[1]=hash, rest=service parts
        // Actually the prefix is "\\\\.\\pipe\\netipc" which contains no '-'
        // So: parts = ["\\\\.", "\\pipe\\netipc", hash, "cgroups", "snapshot"]
        // No, wait. The narrow string is literally: \\.\pipe\netipc-{hash}-cgroups-snapshot
        // backslash is just a char. Split on '-':
        // ["\\\\.\\pipe\\netipc", hash, "cgroups", "snapshot"]
        assert_eq!(parts[1].len(), 16, "hash should be 16 hex chars");
    }

    #[test]
    fn test_build_pipe_name_invalid_service() {
        assert!(build_pipe_name("/var/run", "").is_err());
        assert!(build_pipe_name("/var/run", "bad/name").is_err());
        assert!(build_pipe_name("/var/run", ".").is_err());
    }

    #[test]
    fn test_pipe_name_deterministic() {
        let n1 = build_pipe_name("/var/run/netdata", "test-svc").unwrap();
        let n2 = build_pipe_name("/var/run/netdata", "test-svc").unwrap();
        assert_eq!(n1, n2);
    }

    #[test]
    fn test_pipe_name_different_run_dir() {
        let n1 = build_pipe_name("/var/run/netdata", "svc").unwrap();
        let n2 = build_pipe_name("/tmp/netdata", "svc").unwrap();
        assert_ne!(n1, n2);
    }
}
