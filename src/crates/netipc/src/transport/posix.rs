//! L1 POSIX UDS SEQPACKET transport.
//!
//! Connection lifecycle, handshake with profile/limit negotiation,
//! and send/receive with transparent chunking over AF_UNIX SEQPACKET sockets.
//! Wire-compatible with the C implementation in netipc_uds.c.

use super::framing::{self, Framing, FramingError, ServerOffer, apply_default};
use crate::protocol::{
    self, HEADER_SIZE, Header, Hello, HelloAck, MAX_PAYLOAD_DEFAULT, PROFILE_BASELINE,
};
use std::collections::HashSet;
use std::ffi::CString;
use std::io;
use std::os::unix::io::RawFd;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::{Duration, Instant};

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

const DEFAULT_BACKLOG: i32 = 16;
const DEFAULT_BATCH_ITEMS: u32 = 1;
const DEFAULT_PACKET_SIZE_FALLBACK: u32 = 65536;
const PACKET_SIZE_MAX: u32 = 262_144;
/// Bytes the kernel keeps from SO_SNDBUF for a SEQPACKET datagram: Linux
/// accepts SO_SNDBUF - 32, FreeBSD the full SO_SNDBUF.
#[cfg(target_os = "freebsd")]
const SNDBUF_OVERHEAD: u32 = 0;
#[cfg(not(target_os = "freebsd"))]
const SNDBUF_OVERHEAD: u32 = 32;

/// Profiles this platform can provide. POSIX SHM needs Linux futexes, so
/// other POSIX systems offer and accept the baseline profile only.
#[cfg(target_os = "linux")]
const PLATFORM_PROFILES: u32 = u32::MAX;
#[cfg(not(target_os = "linux"))]
const PLATFORM_PROFILES: u32 = PROFILE_BASELINE;

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
    /// Receive timed out before a complete message arrived.
    Timeout,
    /// Receive was aborted by the caller.
    Aborted,
    /// Handshake protocol error.
    Handshake(String),
    /// Authentication token rejected.
    AuthFailed,
    /// No common profile between client and server.
    NoProfile,
    /// Protocol or layout version mismatch.
    Incompatible(String),
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
            UdsError::Timeout => write!(f, "receive timed out"),
            UdsError::Aborted => write!(f, "receive aborted"),
            UdsError::Handshake(s) => write!(f, "handshake error: {s}"),
            UdsError::AuthFailed => write!(f, "authentication token rejected"),
            UdsError::NoProfile => write!(f, "no common transport profile"),
            UdsError::Incompatible(s) => write!(f, "incompatible protocol: {s}"),
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

impl FramingError for UdsError {
    fn protocol(msg: String) -> Self {
        UdsError::Protocol(msg)
    }
    fn chunk(msg: String) -> Self {
        UdsError::Chunk(msg)
    }
    fn incompatible(msg: String) -> Self {
        UdsError::Incompatible(msg)
    }
    fn handshake(msg: String) -> Self {
        UdsError::Handshake(msg)
    }
    fn bad_param(msg: String) -> Self {
        UdsError::BadParam(msg)
    }
    fn limit_exceeded() -> Self {
        UdsError::LimitExceeded
    }
    fn auth_failed() -> Self {
        UdsError::AuthFailed
    }
    fn no_profile() -> Self {
        UdsError::NoProfile
    }
    fn duplicate_msg_id(id: u64) -> Self {
        UdsError::DuplicateMsgId(id)
    }
    fn unknown_msg_id(id: u64) -> Self {
        UdsError::UnknownMsgId(id)
    }
    fn fails_session(&self) -> bool {
        matches!(self, UdsError::Send(_))
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
    /// 0 = kernel limit; always capped at SO_SNDBUF - 32 and 256 KiB.
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
    /// 0 = kernel limit; always capped at SO_SNDBUF - 32 and 256 KiB.
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
    /// The framing of the messages this side sends (`sending`) or receives.
    /// Clients send requests and receive responses; servers the opposite.
    fn framing(&self, sending: bool) -> Framing {
        let client = self.role == Role::Client;
        let (max_payload, max_items) = if client == sending {
            (self.max_request_payload_bytes, self.max_request_batch_items)
        } else {
            (
                self.max_response_payload_bytes,
                self.max_response_batch_items,
            )
        };
        Framing {
            client,
            max_payload,
            max_items,
            packet_size: self.packet_size,
        }
    }

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
        Self::connect_with_timeout(run_dir, service_name, config, 0, None)
    }

    /// `connect()` bounded by `timeout_ms`: connect and the wait for
    /// HELLO_ACK together end with `UdsError::Timeout` after `timeout_ms`
    /// (0 = no deadline), and with `UdsError::Aborted` once `abort` is set.
    pub fn connect_with_timeout(
        run_dir: &str,
        service_name: &str,
        config: &ClientConfig,
        timeout_ms: u32,
        abort: Option<&AtomicBool>,
    ) -> Result<Self, UdsError> {
        let wait = ReceiveWait::new(timeout_ms, abort);
        let path = build_socket_path(run_dir, service_name)?;

        let fd = seqpacket_socket_cloexec();
        if fd < 0 {
            return Err(UdsError::Socket(errno()));
        }

        let result = connect_and_handshake(fd, &path, config, &wait);
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

        let framing = self.framing(true);
        let fd = self.fd;
        framing::send_message(framing, &mut self.inflight_ids, hdr, payload, |h, p| {
            raw_send_iov(fd, h, p)
        })
    }

    /// Receive one logical message. Blocks until a complete message arrives.
    ///
    /// On success, returns (header, payload_view). The payload view is valid
    /// until the next receive call on this session.
    pub fn receive<'a>(&'a mut self, buf: &'a mut [u8]) -> Result<(Header, &'a [u8]), UdsError> {
        self.receive_with_control(buf, 0, None)
    }

    /// Receive one logical message with an optional deadline and abort flag.
    ///
    /// `timeout_ms == 0` waits indefinitely. When `abort` is set before a
    /// complete message arrives, the call returns `UdsError::Aborted`.
    pub fn receive_with_control<'a>(
        &'a mut self,
        buf: &'a mut [u8],
        timeout_ms: u32,
        abort: Option<&AtomicBool>,
    ) -> Result<(Header, &'a [u8]), UdsError> {
        if self.fd < 0 {
            return Err(UdsError::BadParam("session closed".into()));
        }

        let controlled = timeout_ms != 0 || abort.is_some();
        let mut wait = ReceiveWait::new(timeout_ms, abort);

        let framing = self.framing(false);
        let fd = self.fd;
        framing::receive_message(
            framing,
            &mut self.inflight_ids,
            buf,
            &mut self.recv_buf,
            &mut self.pkt_buf,
            |b| raw_recv_control(fd, b, controlled, &mut wait),
        )
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
    /// `{service_name}.lock`, flock'ed for the listener's lifetime. Fields
    /// drop after `Drop::drop` removed the socket file, so the endpoint is
    /// released only once its socket file is gone.
    _lock: std::fs::File,
}

/// Test-only hook run inside `UdsListener::bind` between bind() and
/// listen() for the given socket path.
#[cfg(test)]
pub(crate) type ListenHook = (String, Box<dyn Fn() + Send>);
#[cfg(test)]
pub(crate) static LISTEN_HOOK: std::sync::Mutex<Option<ListenHook>> = std::sync::Mutex::new(None);

/// Take the endpoint lock `{run_dir}/{service_name}.lock`. A server's
/// socket file exists from bind() until listen(), and connects in that
/// window are refused just as for a dead server's leftover file, so a
/// connect probe alone cannot tell a starting server from a stale one. The
/// lock can: its holder owns the endpoint, and the kernel drops it when that
/// process dies. The file stays on disk; removing it would let a starter
/// that already opened it lock an orphaned inode.
fn acquire_endpoint_lock(run_dir: &str, service_name: &str) -> Result<std::fs::File, UdsError> {
    use std::os::unix::fs::OpenOptionsExt;
    use std::os::unix::io::AsRawFd;

    let lock_path = format!("{run_dir}/{service_name}.lock");
    let file = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .mode(0o600)
        .custom_flags(libc::O_CLOEXEC | libc::O_NOFOLLOW)
        .open(&lock_path)
        .map_err(|e| UdsError::Socket(e.raw_os_error().unwrap_or(0)))?;
    loop {
        if unsafe { libc::flock(file.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } == 0 {
            return Ok(file);
        }
        match errno() {
            libc::EINTR => continue,
            libc::EWOULDBLOCK => return Err(UdsError::AddrInUse),
            e => return Err(UdsError::Socket(e)),
        }
    }
}

impl UdsListener {
    /// Create a listener on `{run_dir}/{service_name}.sock`.
    /// First takes an exclusive flock on `{run_dir}/{service_name}.lock`,
    /// held until the listener drops; a held lock means another server owns
    /// the endpoint (`AddrInUse`). With the lock held, an existing socket
    /// file no live server accepts on is stale and is replaced.
    pub fn bind(run_dir: &str, service_name: &str, config: ServerConfig) -> Result<Self, UdsError> {
        let path = build_socket_path(run_dir, service_name)?;
        let lock = acquire_endpoint_lock(run_dir, service_name)?;

        // The probe still guards against a live server that takes no lock
        // (an older library version).
        match check_and_recover_stale(&path) {
            StaleResult::LiveServer => return Err(UdsError::AddrInUse),
            StaleResult::Stale | StaleResult::NotExist => { /* proceed */ }
        }

        let fd = seqpacket_socket_cloexec();
        if fd < 0 {
            return Err(UdsError::Socket(errno()));
        }

        // Owner-only (0600, narrowed by the umask). Linux and FreeBSD give
        // the socket file the mode of the unbound socket, so this needs no
        // post-bind window and no change to the process-global umask.
        if unsafe { libc::fchmod(fd, libc::S_IRUSR | libc::S_IWUSR) } < 0 {
            let e = errno();
            unsafe {
                libc::close(fd);
            }
            return Err(UdsError::Socket(e));
        }

        if let Err(e) = bind_unix(fd, &path) {
            unsafe {
                libc::close(fd);
            }
            return Err(e);
        }

        #[cfg(test)]
        {
            // Run the hook once, outside the lock: other binds go on.
            let hook = {
                let mut slot = LISTEN_HOOK.lock().unwrap();
                match slot.as_ref() {
                    Some((hook_path, _)) if *hook_path == path => slot.take(),
                    _ => None,
                }
            };
            if let Some((_, hook)) = hook {
                hook();
            }
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
            _lock: lock,
        })
    }

    /// Get the raw fd for poll/epoll integration.
    pub fn fd(&self) -> RawFd {
        self.fd
    }

    /// Update the payload limits used for future handshakes.
    pub fn set_payload_limits(
        &mut self,
        max_request_payload_bytes: u32,
        max_response_payload_bytes: u32,
    ) {
        self.config.max_request_payload_bytes = max_request_payload_bytes;
        self.config.max_response_payload_bytes = max_response_payload_bytes;
    }

    /// Accept one client. Performs the full handshake.
    /// Blocks until a client connects and the handshake completes.
    pub fn accept(&self) -> Result<UdsSession, UdsError> {
        let session_id = self.next_session_id.fetch_add(1, Ordering::Relaxed);
        self.accept_with_config(session_id, self.config.clone())
    }

    /// Accept one client using a caller-provided per-session server config
    /// and session ID. The handshake waits at most
    /// `SERVER_HANDSHAKE_TIMEOUT_MS` for HELLO.
    pub fn accept_with_config(
        &self,
        session_id: u64,
        config: ServerConfig,
    ) -> Result<UdsSession, UdsError> {
        self.accept_pending()?.handshake(
            session_id,
            &config,
            protocol::SERVER_HANDSHAKE_TIMEOUT_MS,
            None,
        )
    }

    /// Accept one connection without handshaking, so the caller can run the
    /// handshake on another thread. Blocks until a client connects.
    pub fn accept_pending(&self) -> Result<PendingConnection, UdsError> {
        let fd = accept_cloexec(self.fd);
        if fd < 0 {
            return Err(UdsError::Accept(errno()));
        }
        Ok(PendingConnection { fd })
    }
}

/// An accepted connection whose handshake has not run yet. Dropping it
/// closes the connection.
#[derive(Debug)]
pub struct PendingConnection {
    fd: RawFd,
}

impl PendingConnection {
    /// Complete the server side of the handshake. Waits at most `timeout_ms`
    /// for HELLO (`UdsError::Timeout`; 0 waits without a deadline) and returns
    /// `UdsError::Aborted` once `abort` is set.
    pub fn handshake(
        self,
        session_id: u64,
        config: &ServerConfig,
        timeout_ms: u32,
        abort: Option<&AtomicBool>,
    ) -> Result<UdsSession, UdsError> {
        self.handshake_inner(session_id, config, timeout_ms, abort, None)
    }

    /// [`handshake`](Self::handshake) with a prepare step. Once a profile is
    /// selected and the payload limits are agreed, and before HELLO_ACK is
    /// sent, `prepare(profile, max_request_payload_bytes,
    /// max_response_payload_bytes)` sets up that profile's resources (an SHM
    /// region). Returning false drops the profile and the handshake selects
    /// again; with no profile left the client is rejected as unsupported.
    pub fn handshake_prepared(
        self,
        session_id: u64,
        config: &ServerConfig,
        timeout_ms: u32,
        abort: Option<&AtomicBool>,
        prepare: &mut dyn FnMut(u32, u32, u32) -> bool,
    ) -> Result<UdsSession, UdsError> {
        self.handshake_inner(session_id, config, timeout_ms, abort, Some(prepare))
    }

    fn handshake_inner(
        self,
        session_id: u64,
        config: &ServerConfig,
        timeout_ms: u32,
        abort: Option<&AtomicBool>,
        prepare: Option<&mut dyn FnMut(u32, u32, u32) -> bool>,
    ) -> Result<UdsSession, UdsError> {
        wait_fd_readable(self.fd, &ReceiveWait::new(timeout_ms, abort))?;
        let session = server_handshake(self.fd, config, session_id, prepare)?;
        // The session owns the descriptor now.
        std::mem::forget(self);
        Ok(session)
    }
}

impl Drop for PendingConnection {
    fn drop(&mut self) {
        unsafe {
            libc::close(self.fd);
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

// Sockets must not leak into children a consumer forks and execs: an
// inherited session socket keeps the peer from ever seeing EOF, which is
// the dead-peer signal. Platforms without SOCK_CLOEXEC set the flag right
// after creation.
#[cfg(any(target_os = "linux", target_os = "android", target_os = "freebsd"))]
fn seqpacket_socket_cloexec() -> i32 {
    unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET | libc::SOCK_CLOEXEC, 0) }
}

#[cfg(not(any(target_os = "linux", target_os = "android", target_os = "freebsd")))]
fn seqpacket_socket_cloexec() -> i32 {
    let fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET, 0) };
    if fd >= 0 {
        unsafe { libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC) };
    }
    fd
}

#[cfg(any(target_os = "linux", target_os = "android", target_os = "freebsd"))]
fn accept_cloexec(listen_fd: i32) -> i32 {
    unsafe {
        libc::accept4(
            listen_fd,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            libc::SOCK_CLOEXEC,
        )
    }
}

#[cfg(not(any(target_os = "linux", target_os = "android", target_os = "freebsd")))]
fn accept_cloexec(listen_fd: i32) -> i32 {
    let fd = unsafe { libc::accept(listen_fd, std::ptr::null_mut(), std::ptr::null_mut()) };
    if fd >= 0 {
        unsafe { libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC) };
    }
    fd
}

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
                )));
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

/// Packet size this side may propose: `configured` (0 = no preference)
/// capped by what the kernel accepts as one datagram.
///
/// Linux rejects a datagram larger than SO_SNDBUF minus 32 bytes of
/// accounting overhead with EMSGSIZE; very large buffers also hit ENOBUFS
/// near 1 MiB, hence the fixed cap.
fn packet_size_limit(fd: RawFd, configured: u32) -> u32 {
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

    let limit = if rc < 0 || val <= SNDBUF_OVERHEAD as libc::c_int {
        DEFAULT_PACKET_SIZE_FALLBACK
    } else {
        val as u32 - SNDBUF_OVERHEAD
    }
    .min(PACKET_SIZE_MAX);

    if configured != 0 && configured < limit {
        configured
    } else {
        limit
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

    // Zero-initialize: musl's 64-bit msghdr has private padding fields, so a
    // struct literal does not compile there.
    let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
    msg.msg_iov = iov.as_mut_ptr();
    msg.msg_iovlen = iovcnt;

    let n = retry_eintr(
        #[cfg(test)]
        &eintr_inject::SENDMSG,
        || unsafe { libc::sendmsg(fd, &msg, libc::MSG_NOSIGNAL) },
    );
    match n {
        Ok(n) if n as usize == total => Ok(()),
        Ok(_) => Err(UdsError::Send(errno())),
        Err(e) => Err(UdsError::Send(e)),
    }
}

/// Run a socket call, retrying EINTR. SOCK_SEQPACKET sends and receives
/// whole messages or nothing, so an interrupted call moved no data.
/// Returns the call's result, or the errno of a failed call.
fn retry_eintr(
    #[cfg(test)] inject: &'static std::thread::LocalKey<std::cell::Cell<u32>>,
    mut call: impl FnMut() -> isize,
) -> Result<isize, i32> {
    loop {
        #[cfg(test)]
        let injected = eintr_inject::take(inject);
        #[cfg(not(test))]
        let injected = false;
        let n = if injected { -1 } else { call() };
        if n >= 0 {
            return Ok(n);
        }
        let e = if injected { libc::EINTR } else { errno() };
        if e != libc::EINTR {
            return Err(e);
        }
    }
}

/// Test-only EINTR injection: the next N socket calls of a kind made on the
/// current thread fail with EINTR before reaching the kernel.
#[cfg(test)]
pub(crate) mod eintr_inject {
    use std::cell::Cell;
    use std::thread::LocalKey;

    thread_local! {
        pub(crate) static SEND: Cell<u32> = const { Cell::new(0) };
        pub(crate) static SENDMSG: Cell<u32> = const { Cell::new(0) };
        pub(crate) static RECV: Cell<u32> = const { Cell::new(0) };
    }

    pub(crate) fn take(counter: &'static LocalKey<Cell<u32>>) -> bool {
        counter.with(|c| {
            let left = c.get();
            if left == 0 {
                return false;
            }
            c.set(left - 1);
            true
        })
    }

    /// Injections on this thread not consumed yet.
    pub(crate) fn pending() -> u32 {
        SEND.with(Cell::get) + SENDMSG.with(Cell::get) + RECV.with(Cell::get)
    }
}

/// Send a contiguous buffer as one SEQPACKET message.
fn raw_send(fd: RawFd, data: &[u8]) -> Result<(), UdsError> {
    let n = retry_eintr(
        #[cfg(test)]
        &eintr_inject::SEND,
        || unsafe {
            libc::send(
                fd,
                data.as_ptr() as *const libc::c_void,
                data.len(),
                libc::MSG_NOSIGNAL,
            )
        },
    );
    match n {
        Ok(n) if n as usize == data.len() => Ok(()),
        Ok(_) => Err(UdsError::Send(errno())),
        Err(e) => Err(UdsError::Send(e)),
    }
}

/// Receive one SEQPACKET message. Returns bytes received.
fn raw_recv(fd: RawFd, buf: &mut [u8]) -> Result<usize, UdsError> {
    let n = retry_eintr(
        #[cfg(test)]
        &eintr_inject::RECV,
        || unsafe { libc::recv(fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len(), 0) },
    );
    match n {
        Ok(0) => Err(UdsError::Recv(0)),
        Ok(n) => Ok(n as usize),
        Err(e) => Err(UdsError::Recv(e)),
    }
}

struct ReceiveWait<'a> {
    deadline: Option<Instant>,
    abort: Option<&'a AtomicBool>,
}

impl<'a> ReceiveWait<'a> {
    fn new(timeout_ms: u32, abort: Option<&'a AtomicBool>) -> Self {
        Self {
            deadline: (timeout_ms != 0)
                .then(|| Instant::now() + Duration::from_millis(timeout_ms as u64)),
            abort,
        }
    }

    fn aborted(&self) -> bool {
        self.abort
            .map(|flag| flag.load(Ordering::Acquire))
            .unwrap_or(false)
    }

    fn poll_timeout_ms(&self) -> Result<i32, UdsError> {
        if self.aborted() {
            return Err(UdsError::Aborted);
        }

        let mut wait_ms: Option<u64> = if let Some(deadline) = self.deadline {
            let now = Instant::now();
            if now >= deadline {
                return Err(UdsError::Timeout);
            }
            Some(
                deadline
                    .duration_since(now)
                    .as_millis()
                    .clamp(1, i32::MAX as u128) as u64,
            )
        } else {
            None
        };

        if self.abort.is_some() {
            wait_ms = Some(wait_ms.map_or(100, |ms| ms.min(100)));
        }

        Ok(wait_ms.map_or(-1, |ms| ms as i32))
    }
}

fn wait_fd_readable(fd: RawFd, wait: &ReceiveWait<'_>) -> Result<(), UdsError> {
    loop {
        let timeout = wait.poll_timeout_ms()?;
        let mut pfd = libc::pollfd {
            fd,
            events: libc::POLLIN,
            revents: 0,
        };
        let rc = unsafe { libc::poll(&mut pfd, 1, timeout) };
        if rc < 0 {
            let err = errno();
            if err == libc::EINTR {
                continue;
            }
            return Err(UdsError::Recv(err));
        }
        if rc == 0 {
            // An empty slice only means the abort flag is due for a check;
            // poll_timeout_ms() reports the real deadline.
            continue;
        }
        if wait.aborted() {
            return Err(UdsError::Aborted);
        }
        return Ok(());
    }
}

fn raw_recv_with_wait(
    fd: RawFd,
    buf: &mut [u8],
    wait: &mut ReceiveWait<'_>,
) -> Result<usize, UdsError> {
    wait_fd_readable(fd, wait)?;
    raw_recv(fd, buf)
}

fn raw_recv_control(
    fd: RawFd,
    buf: &mut [u8],
    controlled: bool,
    wait: &mut ReceiveWait<'_>,
) -> Result<usize, UdsError> {
    if controlled {
        raw_recv_with_wait(fd, buf, wait)
    } else {
        raw_recv(fd, buf)
    }
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
    let probe = seqpacket_socket_cloexec();
    if probe < 0 {
        // Cannot probe liveness (fd exhaustion) — keep the endpoint; the
        // subsequent bind() fails instead of risking a live socket.
        return StaleResult::LiveServer;
    }

    let result = match connect_unix(probe, path) {
        Ok(()) => {
            // Connected => live server
            StaleResult::LiveServer
        }
        Err(UdsError::Connect(e)) if e == libc::ENOENT => StaleResult::NotExist,
        Err(_) => {
            // Nothing accepted the connection: a dead server's socket, or a
            // foreign file squatting on the endpoint path. Reclaim it.
            match std::fs::remove_file(path) {
                Ok(()) => StaleResult::Stale,
                Err(err) if err.kind() == io::ErrorKind::NotFound => StaleResult::NotExist,
                Err(err) if err.raw_os_error() == Some(libc::EISDIR) => {
                    match std::fs::remove_dir(path) {
                        Ok(()) => StaleResult::Stale,
                        Err(_) => StaleResult::LiveServer,
                    }
                }
                Err(_) => StaleResult::LiveServer,
            }
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

/// Longest single connect wait, so an abort ends a connect promptly.
const CONNECT_SLICE_MS: u64 = 100;

fn set_send_timeout(fd: RawFd, ms: u64) {
    let tv = libc::timeval {
        tv_sec: (ms / 1000) as _,
        tv_usec: ((ms % 1000) * 1000) as _,
    };
    unsafe {
        libc::setsockopt(
            fd,
            libc::SOL_SOCKET,
            libc::SO_SNDTIMEO,
            &tv as *const libc::timeval as *const libc::c_void,
            std::mem::size_of::<libc::timeval>() as libc::socklen_t,
        );
    }
}

/// A blocking AF_UNIX connect waits while the listener's backlog is full.
/// On Linux SO_SNDTIMEO bounds that wait and the connect fails with EAGAIN
/// when it expires; waiting in slices lets an abort end it. Other systems
/// refuse a full backlog at once.
fn connect_unix_bounded(fd: RawFd, path: &str, wait: &ReceiveWait<'_>) -> Result<(), UdsError> {
    let result = loop {
        if wait.aborted() {
            break Err(UdsError::Aborted);
        }
        let mut slice = CONNECT_SLICE_MS;
        if let Some(deadline) = wait.deadline {
            let now = Instant::now();
            if now >= deadline {
                break Err(UdsError::Timeout);
            }
            let left = deadline.duration_since(now).as_millis().max(1) as u64;
            slice = slice.min(left);
        }
        set_send_timeout(fd, slice);
        match connect_unix(fd, path) {
            Ok(()) => break Ok(()),
            Err(UdsError::Connect(e)) if e == libc::EISCONN => break Ok(()),
            Err(UdsError::Connect(e))
                if e == libc::EAGAIN
                    || e == libc::EWOULDBLOCK
                    || e == libc::EINTR
                    || e == libc::EALREADY => {}
            Err(e) => break Err(e),
        }
    };
    // Session sends block without a timeout.
    set_send_timeout(fd, 0);
    result
}

fn connect_and_handshake(
    fd: RawFd,
    path: &str,
    config: &ClientConfig,
    wait: &ReceiveWait<'_>,
) -> Result<UdsSession, UdsError> {
    connect_unix_bounded(fd, path, wait)?;

    let pkt_size = packet_size_limit(fd, config.packet_size);

    let supported = apply_default(config.supported_profiles, PROFILE_BASELINE) & PLATFORM_PROFILES;

    // Build HELLO
    let hello = Hello {
        layout_version: 1,
        flags: 0,
        supported_profiles: supported,
        preferred_profiles: config.preferred_profiles & PLATFORM_PROFILES,
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

    // Send HELLO
    raw_send(fd, &framing::hello_packet(&hello))?;

    // Receive HELLO_ACK; a server that never answers must not hold the
    // client past its deadline.
    wait_fd_readable(fd, wait)?;
    let mut buf = [0u8; 128];
    let n = raw_recv(fd, &mut buf)?;
    let ack: HelloAck = framing::parse_hello_ack(&buf[..n], supported)?;

    // Sanity: reject a packet_size too small for chunking arithmetic
    if ack.agreed_packet_size <= HEADER_SIZE as u32 {
        return Err(UdsError::Protocol("agreed packet_size too small".into()));
    }
    // A server may only lower our proposal; a larger size could exceed what
    // this side's kernel accepts as one datagram.
    if ack.agreed_packet_size > pkt_size {
        return Err(UdsError::Protocol(
            "agreed packet_size exceeds proposal".into(),
        ));
    }

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
    prepare: Option<&mut dyn FnMut(u32, u32, u32) -> bool>,
) -> Result<UdsSession, UdsError> {
    let offer = ServerOffer {
        packet_size: packet_size_limit(fd, config.packet_size),
        supported_profiles: apply_default(config.supported_profiles, PROFILE_BASELINE)
            & PLATFORM_PROFILES,
        preferred_profiles: config.preferred_profiles & PLATFORM_PROFILES,
        max_request_payload_bytes: apply_default(
            config.max_request_payload_bytes,
            MAX_PAYLOAD_DEFAULT,
        ),
        max_response_payload_bytes: apply_default(
            config.max_response_payload_bytes,
            MAX_PAYLOAD_DEFAULT,
        ),
        auth_token: config.auth_token,
    };

    // Receive HELLO
    let mut buf = [0u8; 128];
    let n = raw_recv(fd, &mut buf)?;
    let ack = framing::server_negotiate(&buf[..n], &offer, session_id, prepare, |pkt| {
        raw_send(fd, pkt)
    })?;

    Ok(UdsSession {
        fd,
        role: Role::Server,
        max_request_payload_bytes: ack.agreed_max_request_payload_bytes,
        max_request_batch_items: ack.agreed_max_request_batch_items,
        max_response_payload_bytes: ack.agreed_max_response_payload_bytes,
        max_response_batch_items: ack.agreed_max_response_batch_items,
        packet_size: ack.agreed_packet_size,
        selected_profile: ack.selected_profile,
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
#[path = "posix_tests.rs"]
mod tests;
