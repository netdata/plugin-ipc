//! L1 Windows Named Pipe transport.
//!
//! Connection lifecycle, handshake with profile/limit negotiation,
//! and send/receive with transparent chunking over Win32 Named Pipes
//! in message mode. Wire-compatible with the C and Go implementations.

use super::framing::{self, Framing, FramingError, ServerOffer, apply_default};
use crate::protocol::{
    HEADER_SIZE, Header, Hello, HelloAck, MAX_PAYLOAD_DEFAULT, PROFILE_BASELINE,
};
#[cfg(windows)]
use crate::transport::win_security::{OwnerOnlyAttributes, owner_trusted};
use std::collections::HashSet;
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::{Duration, Instant};

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
    pub const PIPE_REJECT_REMOTE_CLIENTS: DWORD = 0x00000008;
    pub const SECURITY_SQOS_PRESENT: DWORD = 0x00100000;
    pub const SECURITY_IDENTIFICATION: DWORD = 0x00010000;
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
    pub const ERROR_SEM_TIMEOUT: DWORD = 121;

    unsafe extern "system" {
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

        pub fn ConnectNamedPipe(hNamedPipe: HANDLE, lpOverlapped: *mut core::ffi::c_void) -> BOOL;

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
        pub fn GetTickCount64() -> u64;
        pub fn PeekNamedPipe(
            hNamedPipe: HANDLE,
            lpBuffer: LPVOID,
            nBufferSize: DWORD,
            lpBytesRead: LPDWORD,
            lpTotalBytesAvail: LPDWORD,
            lpBytesLeftThisMessage: LPDWORD,
        ) -> BOOL;

        pub fn SwitchToThread() -> BOOL;

        pub fn WaitNamedPipeW(lpNamedPipeName: LPCWSTR, nTimeOut: DWORD) -> BOOL;

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
const MAX_PIPE_NAME_CHARS: usize = 256;

/// How long a connect waits for a free pipe instance when every instance is
/// busy (ERROR_PIPE_BUSY).
pub const CONNECT_BUSY_WAIT_MS: u32 = 1000;

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
    /// Receive timed out before a complete message arrived.
    Timeout,
    /// Receive was aborted by the caller.
    Aborted,
    /// Handshake protocol error.
    Handshake(String),
    /// Authentication token rejected.
    AuthFailed,
    /// No common profile.
    NoProfile,
    /// Protocol or layout version mismatch.
    Incompatible(String),
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
    /// The pipe owner is not this account, SYSTEM or Administrators.
    UntrustedServer,
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
            NpError::Timeout => write!(f, "receive timed out"),
            NpError::Aborted => write!(f, "receive aborted"),
            NpError::Handshake(s) => write!(f, "handshake error: {s}"),
            NpError::AuthFailed => write!(f, "authentication token rejected"),
            NpError::NoProfile => write!(f, "no common transport profile"),
            NpError::Incompatible(s) => write!(f, "incompatible protocol: {s}"),
            NpError::Protocol(s) => write!(f, "protocol violation: {s}"),
            NpError::AddrInUse => write!(f, "pipe name already in use by live server"),
            NpError::Chunk(s) => write!(f, "chunk error: {s}"),
            NpError::Alloc => write!(f, "memory allocation failed"),
            NpError::LimitExceeded => write!(f, "negotiated limit exceeded"),
            NpError::BadParam(s) => write!(f, "bad parameter: {s}"),
            NpError::DuplicateMsgId(id) => write!(f, "duplicate message_id: {id}"),
            NpError::UnknownMsgId(id) => write!(f, "unknown response message_id: {id}"),
            NpError::Disconnected => write!(f, "peer disconnected"),
            NpError::UntrustedServer => write!(f, "pipe owned by an untrusted account"),
        }
    }
}

impl FramingError for NpError {
    fn protocol(msg: String) -> Self {
        NpError::Protocol(msg)
    }
    fn chunk(msg: String) -> Self {
        NpError::Chunk(msg)
    }
    fn incompatible(msg: String) -> Self {
        NpError::Incompatible(msg)
    }
    fn handshake(msg: String) -> Self {
        NpError::Handshake(msg)
    }
    fn bad_param(msg: String) -> Self {
        NpError::BadParam(msg)
    }
    fn limit_exceeded() -> Self {
        NpError::LimitExceeded
    }
    fn auth_failed() -> Self {
        NpError::AuthFailed
    }
    fn no_profile() -> Self {
        NpError::NoProfile
    }
    fn duplicate_msg_id(id: u64) -> Self {
        NpError::DuplicateMsgId(id)
    }
    fn unknown_msg_id(id: u64) -> Self {
        NpError::UnknownMsgId(id)
    }
    fn fails_session(&self) -> bool {
        matches!(self, NpError::Send(_) | NpError::Disconnected)
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
        return Err(NpError::BadParam(
            "service name cannot be '.' or '..'".into(),
        ));
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

#[cfg(windows)]
fn pipe_buffer_size(packet_size: u32) -> u32 {
    // The protocol packet size controls logical framing and chunk size. The
    // underlying pipe quota must stay large enough for full-duplex pipelining
    // even when tests force a tiny protocol packet size.
    apply_default(packet_size, DEFAULT_PIPE_BUF_SIZE).max(DEFAULT_PIPE_BUF_SIZE)
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
fn raw_send_msg(
    handle: ffi::HANDLE,
    scratch: &mut Vec<u8>,
    hdr: &[u8],
    payload: &[u8],
) -> Result<(), NpError> {
    let total = hdr.len() + payload.len();
    if scratch.len() < total {
        scratch.resize(total, 0);
    }
    scratch[..hdr.len()].copy_from_slice(hdr);
    scratch[hdr.len()..total].copy_from_slice(payload);
    raw_write(handle, &scratch[..total])
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
struct ReceiveWait<'a> {
    deadline: Option<Instant>,
    abort: Option<&'a AtomicBool>,
}

#[cfg(windows)]
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

    fn wait_slice_ms(&self) -> Result<u32, NpError> {
        if self.aborted() {
            return Err(NpError::Aborted);
        }

        let mut wait_ms: Option<u128> = if let Some(deadline) = self.deadline {
            let now = Instant::now();
            if now >= deadline {
                return Err(NpError::Timeout);
            }
            Some(deadline.duration_since(now).as_millis().max(1))
        } else {
            None
        };

        if self.abort.is_some() {
            wait_ms = Some(wait_ms.map_or(100, |ms| ms.min(100)));
        }

        Ok(wait_ms.unwrap_or(u32::MAX as u128).min(u32::MAX as u128) as u32)
    }
}

#[cfg(windows)]
fn wait_pipe_readable(handle: ffi::HANDLE, timeout_ms: u32) -> Result<bool, NpError> {
    if handle == ffi::INVALID_HANDLE_VALUE || handle == 0 {
        return Err(NpError::BadParam("session closed".into()));
    }

    let deadline = unsafe { ffi::GetTickCount64() }.saturating_add(timeout_ms as u64);
    let mut yielded = false;
    loop {
        let mut available: u32 = 0;
        let ok = unsafe {
            ffi::PeekNamedPipe(
                handle,
                ptr::null_mut(),
                0,
                ptr::null_mut(),
                &mut available,
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
        if available > 0 {
            return Ok(true);
        }

        if unsafe { ffi::GetTickCount64() } >= deadline {
            return Ok(false);
        }

        if !yielded {
            yielded = true;
            for _ in 0..256 {
                unsafe {
                    ffi::SwitchToThread();
                }

                let mut yielded_available: u32 = 0;
                let yielded_ok = unsafe {
                    ffi::PeekNamedPipe(
                        handle,
                        ptr::null_mut(),
                        0,
                        ptr::null_mut(),
                        &mut yielded_available,
                        ptr::null_mut(),
                    )
                };
                if yielded_ok == 0 {
                    let err = last_error();
                    if is_disconnect_error(err) {
                        return Err(NpError::Disconnected);
                    }
                    return Err(NpError::Recv(err));
                }
                if yielded_available > 0 {
                    return Ok(true);
                }

                if unsafe { ffi::GetTickCount64() } >= deadline {
                    return Ok(false);
                }
            }
            continue;
        }

        std::thread::sleep(std::time::Duration::from_millis(1));
    }
}

#[cfg(windows)]
fn raw_recv_with_wait(
    handle: ffi::HANDLE,
    buf: &mut [u8],
    wait: &mut ReceiveWait<'_>,
) -> Result<usize, NpError> {
    loop {
        let wait_ms = wait.wait_slice_ms()?;
        if !wait_pipe_readable(handle, wait_ms)? {
            // An empty slice only means the abort flag is due for a check;
            // wait_slice_ms() reports the real deadline.
            continue;
        }
        if wait.aborted() {
            return Err(NpError::Aborted);
        }
        return raw_recv(handle, buf);
    }
}

#[cfg(windows)]
fn raw_recv_control(
    handle: ffi::HANDLE,
    buf: &mut [u8],
    controlled: bool,
    wait: &mut ReceiveWait<'_>,
) -> Result<usize, NpError> {
    if controlled {
        raw_recv_with_wait(handle, buf, wait)
    } else {
        raw_recv(handle, buf)
    }
}

#[cfg(windows)]
fn close_handle(handle: ffi::HANDLE) {
    if handle != ffi::INVALID_HANDLE_VALUE && handle != 0 {
        unsafe {
            ffi::CloseHandle(handle);
        }
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
    pkt_buf: Vec<u8>,
    send_buf: Vec<u8>,

    // In-flight message_id set (client-side only)
    inflight_ids: HashSet<u64>,
}

#[cfg(windows)]
impl NpSession {
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

    /// Get the raw HANDLE for WaitForSingleObject integration.
    pub fn handle(&self) -> ffi::HANDLE {
        self.handle
    }

    /// Get the session role.
    pub fn role(&self) -> Role {
        self.role
    }

    /// Wait until the pipe becomes readable or the timeout expires.
    pub fn wait_readable(&self, timeout_ms: u32) -> Result<bool, NpError> {
        wait_pipe_readable(self.handle, timeout_ms)
    }

    /// Return the most recently reassembled payload stored in the internal
    /// receive buffer.
    pub fn received_payload(&self, len: usize) -> &[u8] {
        &self.recv_buf[..len]
    }

    /// Connect to a server pipe derived from run_dir + service_name.
    pub fn connect(
        run_dir: &str,
        service_name: &str,
        config: &ClientConfig,
    ) -> Result<Self, NpError> {
        Self::connect_with_timeout(run_dir, service_name, config, 0, None)
    }

    /// `connect()` bounded by `timeout_ms`: the busy-pipe wait and the wait
    /// for HELLO_ACK together end with `NpError::Timeout` after `timeout_ms`
    /// (0 = no deadline), and with `NpError::Aborted` once `abort` is set.
    pub fn connect_with_timeout(
        run_dir: &str,
        service_name: &str,
        config: &ClientConfig,
        timeout_ms: u32,
        abort: Option<&AtomicBool>,
    ) -> Result<Self, NpError> {
        let wait = ReceiveWait::new(timeout_ms, abort);
        let pipe_name = build_pipe_name(run_dir, service_name)?;
        let handle = open_client_pipe(&pipe_name, &wait)?;

        // Whoever created the pipe name first owns it. Send HELLO and the auth
        // token only to a server running as this account, SYSTEM or
        // Administrators; other accounts cannot take those as owner.
        if !owner_trusted(handle) {
            close_handle(handle);
            return Err(NpError::UntrustedServer);
        }

        // Set read mode to message mode
        let mode: u32 = ffi::PIPE_READMODE_MESSAGE;
        let ok = unsafe { ffi::SetNamedPipeHandleState(handle, &mode, ptr::null(), ptr::null()) };
        if ok == 0 {
            let err = last_error();
            close_handle(handle);
            return Err(NpError::Connect(err));
        }

        match client_handshake(handle, config, &wait) {
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

        let framing = self.framing(true);
        let handle = self.handle;
        let send_buf = &mut self.send_buf;
        framing::send_message(framing, &mut self.inflight_ids, hdr, payload, |h, p| {
            raw_send_msg(handle, send_buf, h, p)
        })
    }

    /// Receive one logical message. buf is a scratch buffer for the first read.
    /// The payload view is valid until the next receive call on this session.
    pub fn receive<'a>(&'a mut self, buf: &'a mut [u8]) -> Result<(Header, &'a [u8]), NpError> {
        self.receive_with_control(buf, 0, None)
    }

    /// Receive one logical message with an optional deadline and abort flag.
    pub fn receive_with_control<'a>(
        &'a mut self,
        buf: &'a mut [u8],
        timeout_ms: u32,
        abort: Option<&AtomicBool>,
    ) -> Result<(Header, &'a [u8]), NpError> {
        if self.handle == ffi::INVALID_HANDLE_VALUE {
            return Err(NpError::BadParam("session closed".into()));
        }

        let controlled = timeout_ms != 0 || abort.is_some();
        let mut wait = ReceiveWait::new(timeout_ms, abort);

        let framing = self.framing(false);
        let handle = self.handle;
        framing::receive_message(
            framing,
            &mut self.inflight_ids,
            buf,
            &mut self.recv_buf,
            &mut self.pkt_buf,
            |b| raw_recv_control(handle, b, controlled, &mut wait),
        )
    }

    /// Close the session.
    pub fn close(&mut self) {
        if self.handle != ffi::INVALID_HANDLE_VALUE && self.handle != 0 {
            // Close only. The peer still reads every message written before
            // the close, then sees ERROR_BROKEN_PIPE. FlushFileBuffers would
            // block on a peer that stopped reading, and neither
            // CancelSynchronousIo nor DisconnectNamedPipe from another thread
            // releases it.
            close_handle(self.handle);
            self.handle = ffi::INVALID_HANDLE_VALUE;
        }
        if self.role == Role::Client {
            self.inflight_ids.clear();
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
    pub fn bind(run_dir: &str, service_name: &str, config: ServerConfig) -> Result<Self, NpError> {
        let pipe_name = build_pipe_name(run_dir, service_name)?;
        let buf_size = pipe_buffer_size(config.packet_size);

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

    /// Update the payload limits used for future handshakes.
    pub fn set_payload_limits(
        &mut self,
        max_request_payload_bytes: u32,
        max_response_payload_bytes: u32,
    ) {
        self.config.max_request_payload_bytes = max_request_payload_bytes;
        self.config.max_response_payload_bytes = max_response_payload_bytes;
    }

    /// Accept one client connection. Performs the full handshake.
    pub fn accept(&mut self) -> Result<NpSession, NpError> {
        let session_id = self.next_session_id.fetch_add(1, Ordering::Relaxed);
        self.accept_with_config(session_id, self.config.clone())
    }

    /// Accept one client using a caller-provided per-session server config
    /// and session ID. The handshake waits at most
    /// `SERVER_HANDSHAKE_TIMEOUT_MS` for HELLO.
    pub fn accept_with_config(
        &mut self,
        session_id: u64,
        config: ServerConfig,
    ) -> Result<NpSession, NpError> {
        self.accept_pending()?.handshake(
            session_id,
            &config,
            crate::protocol::SERVER_HANDSHAKE_TIMEOUT_MS,
            None,
        )
    }

    /// Accept one connection without handshaking, so the caller can run the
    /// handshake on another thread. Blocks until a client connects, then
    /// creates a new pipe instance for subsequent clients.
    pub fn accept_pending(&mut self) -> Result<PendingPipe, NpError> {
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
        let buf_size = pipe_buffer_size(self.config.packet_size);
        let next = match create_pipe_instance(&self.pipe_name, buf_size, false) {
            Ok(h) => h,
            Err(e) => {
                // Failed to create replacement instance; close the accepted
                // client to avoid an orphaned connection.
                close_handle(session_handle);
                self.handle = ffi::INVALID_HANDLE_VALUE;
                return Err(e);
            }
        };
        self.handle = next;

        Ok(PendingPipe {
            handle: session_handle,
        })
    }

    /// A waker that unblocks `accept_pending()` from another thread. It uses
    /// only the pipe name, so it never touches a handle the accept loop owns.
    pub fn waker(&self) -> NpListenerWaker {
        NpListenerWaker {
            pipe_name: self.pipe_name.clone(),
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

/// SECURITY_IDENTIFICATION lets the server identify the client but never act
/// as it.
#[cfg(windows)]
fn create_client_pipe(pipe_name: &[u16]) -> ffi::HANDLE {
    unsafe {
        ffi::CreateFileW(
            pipe_name.as_ptr(),
            ffi::GENERIC_READ | ffi::GENERIC_WRITE,
            0,
            ptr::null(),
            ffi::OPEN_EXISTING,
            ffi::SECURITY_SQOS_PRESENT | ffi::SECURITY_IDENTIFICATION,
            0,
        )
    }
}

/// Runs whenever a connect finds every pipe instance busy, before it waits.
#[cfg(all(windows, test))]
pub(crate) static CONNECT_BUSY_HOOK: std::sync::Mutex<Option<Box<dyn Fn() + Send>>> =
    std::sync::Mutex::new(None);

/// Longest single busy-pipe wait, so an abort ends it promptly.
#[cfg(windows)]
const CONNECT_BUSY_SLICE_MS: u64 = 100;

/// A server has no free pipe instance between accepting one client and
/// creating the next instance, and connects in that window fail with
/// ERROR_PIPE_BUSY. Wait, bounded by `CONNECT_BUSY_WAIT_MS` and the caller's
/// deadline, for an instance, then try once more.
#[cfg(windows)]
fn open_client_pipe(pipe_name: &[u16], wait: &ReceiveWait<'_>) -> Result<ffi::HANDLE, NpError> {
    let handle = create_client_pipe(pipe_name);
    if handle != ffi::INVALID_HANDLE_VALUE {
        return Ok(handle);
    }
    let err = last_error();
    if err != ffi::ERROR_PIPE_BUSY {
        return Err(NpError::Connect(err));
    }
    #[cfg(test)]
    if let Some(hook) = CONNECT_BUSY_HOOK.lock().unwrap().as_ref() {
        hook();
    }

    let mut busy_deadline = Instant::now() + Duration::from_millis(CONNECT_BUSY_WAIT_MS as u64);
    if let Some(deadline) = wait.deadline {
        busy_deadline = busy_deadline.min(deadline);
    }
    loop {
        if wait.aborted() {
            return Err(NpError::Aborted);
        }
        let now = Instant::now();
        if now >= busy_deadline {
            if wait.deadline.is_some_and(|deadline| now >= deadline) {
                return Err(NpError::Timeout);
            }
            return Err(NpError::Connect(ffi::ERROR_PIPE_BUSY));
        }
        let left = busy_deadline.duration_since(now).as_millis().max(1) as u64;
        let slice = left.min(CONNECT_BUSY_SLICE_MS) as u32;
        if unsafe { ffi::WaitNamedPipeW(pipe_name.as_ptr(), slice) } != 0 {
            let handle = create_client_pipe(pipe_name);
            if handle == ffi::INVALID_HANDLE_VALUE {
                return Err(NpError::Connect(last_error()));
            }
            return Ok(handle);
        }
        let err = last_error();
        if err != ffi::ERROR_SEM_TIMEOUT {
            return Err(NpError::Connect(err));
        }
    }
}

/// Wakes a listener blocked in `ConnectNamedPipe` by connecting to it.
#[cfg(windows)]
#[derive(Clone, Debug)]
pub struct NpListenerWaker {
    pipe_name: Vec<u16>,
}

#[cfg(windows)]
impl NpListenerWaker {
    pub fn wake(&self) {
        let h = unsafe {
            ffi::CreateFileW(
                self.pipe_name.as_ptr(),
                ffi::GENERIC_READ | ffi::GENERIC_WRITE,
                0,
                ptr::null(),
                ffi::OPEN_EXISTING,
                ffi::SECURITY_SQOS_PRESENT | ffi::SECURITY_IDENTIFICATION,
                0,
            )
        };
        close_handle(h);
    }
}

/// An accepted pipe whose handshake has not run yet. Dropping it closes the
/// pipe; DisconnectNamedPipe would discard a rejection reply the client has
/// not read yet.
#[cfg(windows)]
#[derive(Debug)]
pub struct PendingPipe {
    handle: ffi::HANDLE,
}

#[cfg(windows)]
impl PendingPipe {
    /// Complete the server side of the handshake. Waits at most `timeout_ms`
    /// for HELLO (`NpError::Timeout`; 0 waits without a deadline) and returns
    /// `NpError::Aborted` once `abort` is set.
    pub fn handshake(
        self,
        session_id: u64,
        config: &ServerConfig,
        timeout_ms: u32,
        abort: Option<&AtomicBool>,
    ) -> Result<NpSession, NpError> {
        self.handshake_inner(session_id, config, timeout_ms, abort, None)
    }

    /// [`handshake`](Self::handshake) with a prepare step. Once a profile is
    /// selected and the payload limits are agreed, and before HELLO_ACK is
    /// sent, `prepare(profile, max_request_payload_bytes,
    /// max_response_payload_bytes)` sets up that profile's resources (SHM
    /// objects). Returning false drops the profile and the handshake selects
    /// again; with no profile left the client is rejected as unsupported.
    pub fn handshake_prepared(
        self,
        session_id: u64,
        config: &ServerConfig,
        timeout_ms: u32,
        abort: Option<&AtomicBool>,
        prepare: &mut dyn FnMut(u32, u32, u32) -> bool,
    ) -> Result<NpSession, NpError> {
        self.handshake_inner(session_id, config, timeout_ms, abort, Some(prepare))
    }

    fn handshake_inner(
        self,
        session_id: u64,
        config: &ServerConfig,
        timeout_ms: u32,
        abort: Option<&AtomicBool>,
        prepare: Option<&mut dyn FnMut(u32, u32, u32) -> bool>,
    ) -> Result<NpSession, NpError> {
        let wait = ReceiveWait::new(timeout_ms, abort);
        loop {
            let wait_ms = wait.wait_slice_ms()?;
            if wait_pipe_readable(self.handle, wait_ms)? {
                break;
            }
        }
        if wait.aborted() {
            return Err(NpError::Aborted);
        }
        let session = server_handshake(self.handle, config, session_id, prepare)?;
        // The session owns the handle now.
        std::mem::forget(self);
        Ok(session)
    }
}

#[cfg(windows)]
impl Drop for PendingPipe {
    fn drop(&mut self) {
        close_handle(self.handle);
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

    let attrs = OwnerOnlyAttributes::new().map_err(NpError::CreatePipe)?;
    let handle = unsafe {
        ffi::CreateNamedPipeW(
            pipe_name.as_ptr(),
            open_mode,
            ffi::PIPE_TYPE_MESSAGE
                | ffi::PIPE_READMODE_MESSAGE
                | ffi::PIPE_WAIT
                | ffi::PIPE_REJECT_REMOTE_CLIENTS,
            ffi::PIPE_UNLIMITED_INSTANCES,
            buf_size,
            buf_size,
            0,
            attrs.as_ptr(),
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
    wait: &ReceiveWait<'_>,
) -> Result<NpSession, NpError> {
    let pkt_size = apply_default(config.packet_size, DEFAULT_PACKET_SIZE);

    let supported = apply_default(config.supported_profiles, PROFILE_BASELINE);

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

    raw_write(handle, &framing::hello_packet(&hello))?;

    // Receive HELLO_ACK; a server that never answers must not hold the
    // client past its deadline. A server that rejects the client writes its
    // reply and closes, so a disconnected pipe may still hold that reply.
    loop {
        let wait_ms = wait.wait_slice_ms()?;
        match wait_pipe_readable(handle, wait_ms) {
            Ok(true) | Err(NpError::Disconnected) => break,
            Ok(false) => {}
            Err(e) => return Err(e),
        }
    }
    if wait.aborted() {
        return Err(NpError::Aborted);
    }
    let mut ack_buf = [0u8; 128];
    let n = raw_recv(handle, &mut ack_buf)?;

    let ack: HelloAck = framing::parse_hello_ack(&ack_buf[..n], supported)?;

    // Reject packet sizes too small for a header
    if ack.agreed_packet_size <= HEADER_SIZE as u32 {
        return Err(NpError::Handshake(
            "agreed packet_size <= HEADER_SIZE".into(),
        ));
    }

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
        pkt_buf: Vec::new(),
        send_buf: Vec::new(),
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
    prepare: Option<&mut dyn FnMut(u32, u32, u32) -> bool>,
) -> Result<NpSession, NpError> {
    let offer = ServerOffer {
        packet_size: apply_default(config.packet_size, DEFAULT_PACKET_SIZE),
        supported_profiles: apply_default(config.supported_profiles, PROFILE_BASELINE),
        preferred_profiles: config.preferred_profiles,
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
    let n = raw_recv(handle, &mut buf)?;
    let ack = framing::server_negotiate(&buf[..n], &offer, session_id, prepare, |pkt| {
        raw_write(handle, pkt)
    })?;

    Ok(NpSession {
        handle,
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
        send_buf: Vec::new(),
        inflight_ids: HashSet::new(),
    })
}

// ---------------------------------------------------------------------------
//  Tests (cross-platform unit tests for non-Win32 logic)
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol;
    #[cfg(windows)]
    use crate::protocol::{KIND_REQUEST, KIND_RESPONSE, MAGIC_MSG, VERSION};
    #[cfg(windows)]
    use crate::transport::framing::HELLO_ACK_PAYLOAD_SIZE;
    #[cfg(windows)]
    use std::sync::atomic::{AtomicU64, Ordering};
    #[cfg(windows)]
    use std::thread;

    #[cfg(windows)]
    const TEST_RUN_DIR: &str = r"C:\Temp\nipc_transport_rust_test";
    #[cfg(windows)]
    static TEST_COUNTER: AtomicU64 = AtomicU64::new(0);

    #[cfg(windows)]
    fn ensure_run_dir() {
        let _ = std::fs::create_dir_all(TEST_RUN_DIR);
    }

    #[cfg(windows)]
    fn unique_service(prefix: &str) -> String {
        format!(
            "{}_{}_{}",
            prefix,
            std::process::id(),
            TEST_COUNTER.fetch_add(1, Ordering::Relaxed) + 1
        )
    }

    #[cfg(windows)]
    fn default_client_config() -> ClientConfig {
        ClientConfig {
            supported_profiles: PROFILE_BASELINE,
            preferred_profiles: 0,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 16,
            max_response_payload_bytes: 4096,
            max_response_batch_items: 16,
            auth_token: 0xDEADBEEFCAFEBABE,
            packet_size: 0,
        }
    }

    #[cfg(windows)]
    fn default_server_config() -> ServerConfig {
        ServerConfig {
            supported_profiles: PROFILE_BASELINE,
            preferred_profiles: 0,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 16,
            max_response_payload_bytes: 4096,
            max_response_batch_items: 16,
            auth_token: 0xDEADBEEFCAFEBABE,
            packet_size: 0,
        }
    }

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
        // The pipe name is \\.\pipe\netipc-{hash}-{service}, so parts[1]
        // is the 16-character hash component.
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

    #[test]
    fn test_np_error_display() {
        let cases = [
            (NpError::PipeName("bad".into()), "pipe name error: bad"),
            (NpError::CreatePipe(5), "CreateNamedPipeW failed: 5"),
            (NpError::Connect(231), "connect failed: 231"),
            (NpError::Accept(24), "accept failed: 24"),
            (NpError::Send(32), "send failed: 32"),
            (NpError::Recv(0), "recv failed: 0"),
            (NpError::Handshake("test".into()), "handshake error: test"),
            (NpError::AuthFailed, "authentication token rejected"),
            (NpError::NoProfile, "no common transport profile"),
            (
                NpError::Incompatible("version mismatch".into()),
                "incompatible protocol: version mismatch",
            ),
            (NpError::Protocol("bad".into()), "protocol violation: bad"),
            (
                NpError::AddrInUse,
                "pipe name already in use by live server",
            ),
            (NpError::Chunk("mismatch".into()), "chunk error: mismatch"),
            (NpError::Alloc, "memory allocation failed"),
            (NpError::LimitExceeded, "negotiated limit exceeded"),
            (NpError::BadParam("foo".into()), "bad parameter: foo"),
            (NpError::DuplicateMsgId(42), "duplicate message_id: 42"),
            (NpError::UnknownMsgId(99), "unknown response message_id: 99"),
            (NpError::Disconnected, "peer disconnected"),
        ];

        for (err, expected) in cases {
            assert_eq!(format!("{err}"), expected);
        }

        let e: &dyn std::error::Error = &NpError::Disconnected;
        let _ = format!("{e}");
    }

    #[test]
    fn test_build_pipe_name_too_long() {
        let long_name = "a".repeat(300);
        let result = build_pipe_name("/var/run", &long_name);
        assert!(matches!(result, Err(NpError::PipeName(_))));
    }

    #[cfg(windows)]
    #[test]
    fn test_connect_rejects_selected_profile_not_offered() {
        ensure_run_dir();
        // The client offers baseline only.
        for selected in [
            protocol::PROFILE_SHM_HYBRID,
            0,
            PROFILE_BASELINE | protocol::PROFILE_SHM_HYBRID,
        ] {
            let svc = unique_service("rs_ack_selected");
            let pipe_name = build_pipe_name(TEST_RUN_DIR, &svc).expect("pipe name");
            let (ready_tx, ready_rx) = std::sync::mpsc::channel();
            let server = thread::spawn(move || {
                let handle =
                    create_pipe_instance(&pipe_name, pipe_buffer_size(DEFAULT_PACKET_SIZE), true)
                        .expect("create pipe");
                ready_tx.send(()).expect("signal ready");
                unsafe { ffi::ConnectNamedPipe(handle, std::ptr::null_mut()) };

                let mut buf = [0u8; 128];
                let n = raw_recv(handle, &mut buf).expect("recv hello");
                let hello = Hello::decode(&buf[HEADER_SIZE..n]).expect("decode hello");
                let ack = HelloAck {
                    layout_version: 1,
                    server_supported_profiles: PROFILE_BASELINE | protocol::PROFILE_SHM_HYBRID,
                    intersection_profiles: PROFILE_BASELINE,
                    selected_profile: selected,
                    agreed_max_request_payload_bytes: hello.max_request_payload_bytes,
                    agreed_max_request_batch_items: hello.max_request_batch_items,
                    agreed_max_response_payload_bytes: hello.max_response_payload_bytes,
                    agreed_max_response_batch_items: hello.max_response_batch_items,
                    agreed_packet_size: hello.packet_size,
                    session_id: 1,
                    ..HelloAck::default()
                };
                let mut pkt = [0u8; HEADER_SIZE + HELLO_ACK_PAYLOAD_SIZE];
                Header {
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
                }
                .encode(&mut pkt[..HEADER_SIZE]);
                ack.encode(&mut pkt[HEADER_SIZE..]);
                raw_write(handle, &pkt).expect("send ack");
                close_handle(handle);
            });

            ready_rx.recv().expect("server ready");
            let err = match NpSession::connect(TEST_RUN_DIR, &svc, &default_client_config()) {
                Ok(_) => panic!("HELLO_ACK selecting 0x{selected:x} should fail"),
                Err(err) => err,
            };
            assert!(
                matches!(&err, NpError::Protocol(msg) if msg.contains("not offered")),
                "selected 0x{selected:x}: {err:?}"
            );
            server.join().expect("server join");
        }
    }

    #[cfg(windows)]
    #[test]
    fn test_connect_nonexistent() {
        ensure_run_dir();
        let svc = unique_service("rs_noexist");
        let result = NpSession::connect(TEST_RUN_DIR, &svc, &default_client_config());
        assert!(matches!(result, Err(NpError::Connect(_))));
    }

    #[cfg(windows)]
    #[test]
    fn test_send_receive_on_closed_session() {
        let mut session = NpSession {
            handle: ffi::INVALID_HANDLE_VALUE,
            role: Role::Client,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 16,
            max_response_payload_bytes: 4096,
            max_response_batch_items: 16,
            packet_size: DEFAULT_PACKET_SIZE,
            selected_profile: PROFILE_BASELINE,
            session_id: 1,
            recv_buf: Vec::new(),
            pkt_buf: Vec::new(),
            send_buf: Vec::new(),
            inflight_ids: HashSet::new(),
        };

        let mut hdr = Header {
            kind: protocol::KIND_REQUEST,
            code: protocol::METHOD_INCREMENT,
            item_count: 1,
            message_id: 1,
            ..Header::default()
        };
        assert!(matches!(
            session.send(&mut hdr, &[1, 2, 3]),
            Err(NpError::BadParam(_))
        ));

        let mut buf = [0u8; 64];
        assert!(matches!(
            session.receive(&mut buf),
            Err(NpError::BadParam(_))
        ));
    }

    #[cfg(windows)]
    #[test]
    fn test_roundtrip_and_accessors() {
        ensure_run_dir();
        let svc = unique_service("rs_roundtrip");

        let mut listener =
            NpListener::bind(TEST_RUN_DIR, &svc, default_server_config()).expect("bind");
        assert_ne!(listener.handle(), ffi::INVALID_HANDLE_VALUE);
        assert_ne!(listener.handle(), 0);

        let server = thread::spawn(move || {
            let mut session = listener.accept().expect("accept");
            assert_eq!(session.role(), Role::Server);
            assert_ne!(session.handle(), ffi::INVALID_HANDLE_VALUE);
            let mut buf = [0u8; 256];
            let (hdr, payload) = session.receive(&mut buf).expect("recv");
            let payload = payload.to_vec();
            let mut resp = hdr;
            resp.kind = KIND_RESPONSE;
            resp.transport_status = protocol::STATUS_OK;
            session.send(&mut resp, &payload).expect("send");
            session.close();
        });

        let mut session =
            NpSession::connect(TEST_RUN_DIR, &svc, &default_client_config()).expect("connect");
        assert_eq!(session.role(), Role::Client);
        assert_ne!(session.handle(), ffi::INVALID_HANDLE_VALUE);
        assert_eq!(session.selected_profile, PROFILE_BASELINE);

        let payload = [1u8, 2, 3, 4];
        let mut hdr = Header {
            kind: KIND_REQUEST,
            code: protocol::METHOD_INCREMENT,
            flags: 0,
            item_count: 1,
            message_id: 42,
            ..Header::default()
        };
        session.send(&mut hdr, &payload).expect("send");

        let mut rbuf = [0u8; 256];
        let (rhdr, rpayload) = session.receive(&mut rbuf).expect("recv");
        assert_eq!(rhdr.kind, KIND_RESPONSE);
        assert_eq!(rhdr.message_id, 42);
        assert_eq!(rpayload, payload);

        session.close();
        server.join().expect("server join");
    }

    #[cfg(windows)]
    #[test]
    fn test_receive_packet_longer_than_declared_payload() {
        ensure_run_dir();
        let svc = unique_service("rs_trailing");

        let mut listener =
            NpListener::bind(TEST_RUN_DIR, &svc, default_server_config()).expect("bind");
        let server = thread::spawn(move || {
            let mut session = listener.accept().expect("accept");
            let mut buf = [0u8; 256];
            let (req_hdr, _) = session.receive(&mut buf).expect("recv request");

            let mut pkt = [0u8; HEADER_SIZE + 2];
            let resp_hdr = Header {
                magic: MAGIC_MSG,
                version: VERSION,
                header_len: protocol::HEADER_LEN,
                kind: KIND_RESPONSE,
                code: req_hdr.code,
                flags: 0,
                transport_status: protocol::STATUS_OK,
                payload_len: 1,
                item_count: 1,
                message_id: req_hdr.message_id,
            };
            resp_hdr.encode(&mut pkt[..HEADER_SIZE]);
            pkt[HEADER_SIZE..].copy_from_slice(&[0xBE, 0xEF]);
            raw_write(session.handle, &pkt).expect("raw trailing packet write");
            session.close();
        });

        let mut session =
            NpSession::connect(TEST_RUN_DIR, &svc, &default_client_config()).expect("connect");
        let mut hdr = Header {
            kind: KIND_REQUEST,
            code: protocol::METHOD_INCREMENT,
            item_count: 1,
            message_id: 43,
            ..Header::default()
        };
        session.send(&mut hdr, &[0xAA]).expect("send request");

        let mut rbuf = [0u8; 256];
        let err = session
            .receive(&mut rbuf)
            .expect_err("trailing bytes should be rejected");
        assert!(matches!(err, NpError::Protocol(ref msg)
            if msg.contains("exceeds declared payload_len")));

        session.close();
        server.join().expect("server join");
    }

    #[cfg(windows)]
    #[test]
    fn test_chunking_and_received_payload() {
        ensure_run_dir();
        let svc = unique_service("rs_chunk");

        let scfg = ServerConfig {
            packet_size: 128,
            max_request_payload_bytes: 65536,
            max_response_payload_bytes: 65536,
            ..default_server_config()
        };
        let mut listener = NpListener::bind(TEST_RUN_DIR, &svc, scfg).expect("bind");

        let server = thread::spawn(move || {
            let mut session = listener.accept().expect("accept");
            let mut buf = [0u8; 256];
            let (hdr, payload) = session.receive(&mut buf).expect("recv");
            let payload = payload.to_vec();
            let mut resp = hdr;
            resp.kind = KIND_RESPONSE;
            resp.transport_status = protocol::STATUS_OK;
            session.send(&mut resp, &payload).expect("send");
            session.close();
        });

        let ccfg = ClientConfig {
            packet_size: 128,
            max_request_payload_bytes: 65536,
            max_response_payload_bytes: 65536,
            ..default_client_config()
        };
        let mut session = NpSession::connect(TEST_RUN_DIR, &svc, &ccfg).expect("connect");
        assert_eq!(session.packet_size, 128);

        let big: Vec<u8> = (0..500).map(|i| (i & 0xff) as u8).collect();
        let mut hdr = Header {
            kind: KIND_REQUEST,
            code: protocol::METHOD_INCREMENT,
            item_count: 1,
            message_id: 7,
            ..Header::default()
        };
        session.send(&mut hdr, &big).expect("send chunked");

        let mut rbuf = [0u8; 256];
        let (rhdr, received_len) = {
            let (rhdr, rpayload) = session.receive(&mut rbuf).expect("recv chunked");
            assert_eq!(rpayload, big);
            (rhdr, rpayload.len())
        };
        assert_eq!(rhdr.message_id, 7);
        assert_eq!(session.received_payload(received_len), big.as_slice());

        session.close();
        server.join().expect("server join");
    }

    #[cfg(windows)]
    #[test]
    fn test_pipeline_chunked() {
        ensure_run_dir();
        let svc = unique_service("rs_pipe_chunked");
        let forced_packet_size = 128u32;

        let scfg = ServerConfig {
            packet_size: forced_packet_size,
            max_request_payload_bytes: 65536,
            max_response_payload_bytes: 65536,
            ..default_server_config()
        };
        let mut listener = NpListener::bind(TEST_RUN_DIR, &svc, scfg).expect("bind");

        let server = thread::spawn(move || {
            let mut session = listener.accept().expect("accept");
            let mut buf = vec![0u8; forced_packet_size as usize];
            for _ in 0..5 {
                let (hdr, payload) = session.receive(&mut buf).expect("recv");
                let payload = payload.to_vec();
                let mut resp = hdr;
                resp.kind = KIND_RESPONSE;
                resp.transport_status = protocol::STATUS_OK;
                session.send(&mut resp, &payload).expect("send");
            }
            session.close();
        });

        let ccfg = ClientConfig {
            packet_size: forced_packet_size,
            max_request_payload_bytes: 65536,
            max_response_payload_bytes: 65536,
            ..default_client_config()
        };
        let mut session = NpSession::connect(TEST_RUN_DIR, &svc, &ccfg).expect("connect");

        let sizes = [200usize, 500, 300, 800, 150];
        for (i, &sz) in sizes.iter().enumerate() {
            let payload: Vec<u8> = (0..sz).map(|j| ((i + j) & 0xFF) as u8).collect();
            let mut hdr = Header {
                kind: KIND_REQUEST,
                code: protocol::METHOD_INCREMENT,
                item_count: 1,
                message_id: (i + 1) as u64,
                ..Header::default()
            };
            session.send(&mut hdr, &payload).expect("send");
        }

        let mut rbuf = vec![0u8; forced_packet_size as usize];
        for (i, &sz) in sizes.iter().enumerate() {
            let (rhdr, rpayload) = session.receive(&mut rbuf).expect("recv");
            assert_eq!(rhdr.message_id, (i + 1) as u64, "message_id at {i}");
            assert_eq!(rpayload.len(), sz, "payload len at {i}");
            let expected: Vec<u8> = (0..sz).map(|j| ((i + j) & 0xFF) as u8).collect();
            assert_eq!(rpayload, expected, "payload data at {i}");
        }

        session.close();
        server.join().expect("server join");
    }

    #[cfg(windows)]
    #[test]
    fn test_duplicate_message_id() {
        ensure_run_dir();
        let svc = unique_service("rs_dupmsg");
        let mut listener =
            NpListener::bind(TEST_RUN_DIR, &svc, default_server_config()).expect("bind");

        let server = thread::spawn(move || {
            let mut session = listener.accept().expect("accept");
            let mut buf = [0u8; 256];
            let _ = session.receive(&mut buf).expect("recv");
            session.close();
        });

        let mut session =
            NpSession::connect(TEST_RUN_DIR, &svc, &default_client_config()).expect("connect");

        let mut hdr1 = Header {
            kind: KIND_REQUEST,
            code: protocol::METHOD_INCREMENT,
            item_count: 1,
            message_id: 42,
            ..Header::default()
        };
        session.send(&mut hdr1, &[1]).expect("first send");

        let mut hdr2 = Header {
            kind: KIND_REQUEST,
            code: protocol::METHOD_INCREMENT,
            item_count: 1,
            message_id: 42,
            ..Header::default()
        };
        assert!(matches!(
            session.send(&mut hdr2, &[2]),
            Err(NpError::DuplicateMsgId(42))
        ));

        session.close();
        server.join().expect("server join");
    }

    #[cfg(windows)]
    #[test]
    fn test_directional_limit_negotiation() {
        ensure_run_dir();
        let svc = unique_service("rs_dir_limits");

        let scfg = ServerConfig {
            max_request_payload_bytes: 4096,
            max_request_batch_items: 8,
            max_response_payload_bytes: 8192,
            max_response_batch_items: 32,
            ..default_server_config()
        };
        let mut listener = NpListener::bind(TEST_RUN_DIR, &svc, scfg).expect("bind");

        let server = thread::spawn(move || {
            let mut session = listener.accept().expect("accept");
            assert_eq!(session.max_request_payload_bytes, 4096);
            assert_eq!(session.max_request_batch_items, 16);
            assert_eq!(session.max_response_payload_bytes, 8192);
            assert_eq!(session.max_response_batch_items, 16);
            assert_ne!(session.session_id, 0);
            session.close();
        });

        let ccfg = ClientConfig {
            max_request_payload_bytes: 4096,
            max_request_batch_items: 16,
            max_response_payload_bytes: 4096,
            max_response_batch_items: 16,
            ..default_client_config()
        };
        let mut session = NpSession::connect(TEST_RUN_DIR, &svc, &ccfg).expect("connect");
        assert_eq!(session.max_request_payload_bytes, 4096);
        assert_eq!(session.max_request_batch_items, 16);
        assert_eq!(session.max_response_payload_bytes, 8192);
        assert_eq!(session.max_response_batch_items, 16);
        assert_ne!(session.session_id, 0);

        session.close();
        server.join().expect("server join");
    }

    #[cfg(windows)]
    #[test]
    fn test_request_payload_over_cap() {
        ensure_run_dir();
        let svc = unique_service("rs_reqcap");

        let mut listener =
            NpListener::bind(TEST_RUN_DIR, &svc, default_server_config()).expect("bind");

        let server = thread::spawn(move || {
            let result = listener.accept();
            assert!(matches!(result, Err(NpError::LimitExceeded)));
        });

        let ccfg = ClientConfig {
            max_request_payload_bytes: protocol::MAX_PAYLOAD_CAP + 1,
            ..default_client_config()
        };
        let result = NpSession::connect(TEST_RUN_DIR, &svc, &ccfg);
        assert!(matches!(result, Err(NpError::LimitExceeded)));

        server.join().expect("server join");
    }

    #[cfg(windows)]
    #[test]
    fn test_disconnect_clears_all_inflight() {
        ensure_run_dir();
        let svc = unique_service("rs_disc");
        let mut listener =
            NpListener::bind(TEST_RUN_DIR, &svc, default_server_config()).expect("bind");

        let server = thread::spawn(move || {
            let mut session = listener.accept().expect("accept");
            let mut buf = [0u8; 256];
            let _ = session.receive(&mut buf).expect("recv request");
            session.close();
        });

        let mut session =
            NpSession::connect(TEST_RUN_DIR, &svc, &default_client_config()).expect("connect");

        let mut hdr = Header {
            kind: KIND_REQUEST,
            code: protocol::METHOD_INCREMENT,
            item_count: 1,
            message_id: 99,
            ..Header::default()
        };
        session.send(&mut hdr, &[0xFF]).expect("send");
        session.inflight_ids.insert(100);

        let mut rbuf = [0u8; 256];
        assert!(matches!(
            session.receive(&mut rbuf),
            Err(NpError::Disconnected)
        ));
        assert!(
            session.inflight_ids.is_empty(),
            "disconnect must fail every in-flight request on the session"
        );

        session.close();
        server.join().expect("server join");
    }

    #[cfg(windows)]
    #[test]
    fn test_unknown_response_msg_id() {
        ensure_run_dir();
        let svc = unique_service("rs_unkmsg");
        let mut listener =
            NpListener::bind(TEST_RUN_DIR, &svc, default_server_config()).expect("bind");

        let server = thread::spawn(move || {
            let mut session = listener.accept().expect("accept");
            let mut buf = [0u8; 256];
            let (hdr, payload) = session.receive(&mut buf).expect("recv");
            let payload = payload.to_vec();

            let mut resp = Header {
                kind: KIND_RESPONSE,
                code: hdr.code,
                message_id: hdr.message_id + 999,
                item_count: 1,
                transport_status: protocol::STATUS_OK,
                ..Header::default()
            };
            session.send(&mut resp, &payload).expect("send");
            session.close();
        });

        let mut session =
            NpSession::connect(TEST_RUN_DIR, &svc, &default_client_config()).expect("connect");

        let mut hdr = Header {
            kind: KIND_REQUEST,
            code: protocol::METHOD_INCREMENT,
            item_count: 1,
            message_id: 50,
            ..Header::default()
        };
        session.send(&mut hdr, &[1]).expect("send");

        let mut rbuf = [0u8; 256];
        assert!(matches!(
            session.receive(&mut rbuf),
            Err(NpError::UnknownMsgId(_))
        ));

        session.close();
        server.join().expect("server join");
    }

    #[cfg(windows)]
    #[test]
    fn test_connect_waits_for_busy_pipe() {
        ensure_run_dir();
        let svc = unique_service("rs_busy");
        let mut listener =
            NpListener::bind(TEST_RUN_DIR, &svc, default_server_config()).expect("bind");

        // The first client takes the only instance; the server has not
        // accepted it, so no next instance exists yet.
        let pipe_name = build_pipe_name(TEST_RUN_DIR, &svc).expect("pipe name");
        let holder = create_client_pipe(&pipe_name);
        assert_ne!(
            holder,
            ffi::INVALID_HANDLE_VALUE,
            "first client holds the only instance"
        );

        let (busy_tx, busy_rx) = std::sync::mpsc::channel::<()>();
        *CONNECT_BUSY_HOOK.lock().unwrap() = Some(Box::new(move || {
            let _ = busy_tx.send(());
        }));

        // Accept the holder once the second client found the pipe busy, then
        // serve the second client's handshake.
        let server = thread::spawn(move || {
            busy_rx.recv().ok()?;
            let first = listener.accept_pending().ok()?;
            let second = listener.accept_pending().ok()?;
            let session = second
                .handshake(2, &default_server_config(), 10_000, None)
                .ok()?;
            drop(first);
            Some(session)
        });

        let result = NpSession::connect(TEST_RUN_DIR, &svc, &default_client_config());
        // Dropping the hook releases the server thread if connect never waited.
        *CONNECT_BUSY_HOOK.lock().unwrap() = None;
        if result.is_err() {
            close_handle(holder);
        }
        let mut session = result.expect("busy pipe connect waits and succeeds");
        let accepted = server.join().expect("server join");
        assert!(accepted.is_some(), "server accepted the waiting client");

        session.close();
        close_handle(holder);
    }

    #[cfg(windows)]
    #[test]
    fn test_pipe_dacl_owner_only() {
        use crate::transport::win_security::{object_dacl_sddl, process_user_sid};
        ensure_run_dir();
        let svc = unique_service("rs_dacl");
        let mut listener =
            NpListener::bind(TEST_RUN_DIR, &svc, default_server_config()).expect("bind");
        let me = process_user_sid().expect("process user SID");
        let expected = format!("D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;{me})");
        assert_eq!(
            object_dacl_sddl(listener.handle).as_deref(),
            Some(expected.as_str())
        );

        // Instances created after an accept carry the same DACL.
        let pipe_name = build_pipe_name(TEST_RUN_DIR, &svc).expect("pipe name");
        let client = create_client_pipe(&pipe_name);
        let pending = listener.accept_pending().expect("accept pending");
        assert_eq!(
            object_dacl_sddl(listener.handle).as_deref(),
            Some(expected.as_str())
        );
        drop(pending);
        close_handle(client);
    }

    #[cfg(windows)]
    #[test]
    fn test_remote_client_rejected() {
        ensure_run_dir();
        let svc = unique_service("rs_remote");
        let _listener =
            NpListener::bind(TEST_RUN_DIR, &svc, default_server_config()).expect("bind");

        // The same pipe through the SMB loopback is a remote client.
        let pipe_name = build_pipe_name(TEST_RUN_DIR, &svc).expect("pipe name");
        let mut remote: Vec<u16> = r"\\127.0.0.1\".encode_utf16().collect();
        remote.extend_from_slice(&pipe_name[4..]);
        let h = unsafe {
            ffi::CreateFileW(
                remote.as_ptr(),
                ffi::GENERIC_READ | ffi::GENERIC_WRITE,
                0,
                ptr::null(),
                ffi::OPEN_EXISTING,
                0,
                0,
            )
        };
        if h != ffi::INVALID_HANDLE_VALUE {
            close_handle(h);
        }
        assert_eq!(h, ffi::INVALID_HANDLE_VALUE, "remote open is refused");
    }

    #[cfg(windows)]
    #[test]
    fn test_untrusted_server_rejected() {
        use crate::transport::win_security::{
            SECURITY_IDENTIFICATION_LEVEL, TRUSTED_OWNER_FOR_TESTS, client_impersonation_level,
            object_owner_sid,
        };
        ensure_run_dir();
        let svc = unique_service("rs_untrusted");
        let listener = NpListener::bind(TEST_RUN_DIR, &svc, default_server_config()).expect("bind");

        // BUILTIN\Users never owns the test pipe. Nobody accepts, so a client
        // that skipped the check fails on the deadline instead of hanging.
        TRUSTED_OWNER_FOR_TESTS.with(|t| *t.borrow_mut() = Some("S-1-5-32-545".into()));
        let result = NpSession::connect_with_timeout(
            TEST_RUN_DIR,
            &svc,
            &default_client_config(),
            2000,
            None,
        );
        TRUSTED_OWNER_FOR_TESTS.with(|t| *t.borrow_mut() = None);
        assert!(
            matches!(result, Err(NpError::UntrustedServer)),
            "got {:?}",
            result.err()
        );
        drop(listener);

        // Trusting the real owner lets the client through, and the server sees
        // only an identification-level token for it.
        let svc = unique_service("rs_trusted");
        let mut listener =
            NpListener::bind(TEST_RUN_DIR, &svc, default_server_config()).expect("bind");
        let owner = object_owner_sid(listener.handle).expect("pipe owner");
        let server = thread::spawn(move || {
            let session = listener.accept().ok()?;
            client_impersonation_level(session.handle())
        });
        TRUSTED_OWNER_FOR_TESTS.with(|t| *t.borrow_mut() = Some(owner));
        let result = NpSession::connect_with_timeout(
            TEST_RUN_DIR,
            &svc,
            &default_client_config(),
            10_000,
            None,
        );
        TRUSTED_OWNER_FOR_TESTS.with(|t| *t.borrow_mut() = None);
        let mut session = result.expect("connect accepts the trusted owner");
        let level = server.join().expect("server join");
        assert_eq!(
            level,
            Some(SECURITY_IDENTIFICATION_LEVEL),
            "server can only identify the client"
        );
        session.close();
    }

    #[cfg(windows)]
    #[test]
    fn test_preferred_profile_selected() {
        ensure_run_dir();
        let svc = unique_service("rs_pref");
        let preferred = crate::protocol::PROFILE_SHM_HYBRID;

        let scfg = ServerConfig {
            supported_profiles: PROFILE_BASELINE | preferred,
            preferred_profiles: preferred,
            ..default_server_config()
        };
        let mut listener = NpListener::bind(TEST_RUN_DIR, &svc, scfg).expect("bind");

        let server = thread::spawn(move || {
            let mut session = listener.accept().expect("accept");
            assert_eq!(session.selected_profile, preferred);
            session.close();
        });

        let ccfg = ClientConfig {
            supported_profiles: PROFILE_BASELINE | preferred,
            preferred_profiles: preferred,
            ..default_client_config()
        };
        let mut session = NpSession::connect(TEST_RUN_DIR, &svc, &ccfg).expect("connect");
        assert_eq!(session.selected_profile, preferred);
        session.close();
        server.join().expect("server join");
    }
}
