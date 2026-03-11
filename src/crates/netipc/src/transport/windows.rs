//! Windows transport implementation for the Rust crate.
//!
//! Provides Named Pipe and SHM HYBRID transports, mirroring the C
//! implementation in `netipc_named_pipe.c` / `netipc_shm_hybrid_win.c`.
//! The negotiation protocol and SHM region layout are wire-compatible
//! with the C version.

use crate::protocol::{
    decode_increment_request, decode_increment_response, encode_increment_request,
    encode_increment_response, Frame, IncrementRequest, IncrementResponse, FRAME_SIZE,
};
use std::io;
use std::path::PathBuf;
use std::ptr;
use std::sync::atomic::{AtomicI64, AtomicI32, Ordering, fence};
use std::time::Duration;

// ---------------------------------------------------------------------------
// Win32 FFI
// ---------------------------------------------------------------------------

#[allow(non_snake_case, non_camel_case_types, dead_code)]
mod win32 {
    pub type HANDLE = *mut core::ffi::c_void;
    pub type DWORD = u32;
    pub type BOOL = i32;
    pub type LPCWSTR = *const u16;
    pub type LPCSTR = *const u8;
    pub type LPVOID = *mut core::ffi::c_void;
    pub type SIZE_T = usize;

    pub const INVALID_HANDLE_VALUE: HANDLE = -1isize as HANDLE;
    pub const NULL_HANDLE: HANDLE = core::ptr::null_mut();

    pub const PAGE_READWRITE: DWORD = 0x04;
    pub const FILE_MAP_ALL_ACCESS: DWORD = 0xF001F;

    pub const GENERIC_READ: DWORD = 0x80000000;
    pub const GENERIC_WRITE: DWORD = 0x40000000;
    pub const OPEN_EXISTING: DWORD = 3;

    pub const PIPE_ACCESS_DUPLEX: DWORD = 0x00000003;
    pub const FILE_FLAG_FIRST_PIPE_INSTANCE: DWORD = 0x00080000;
    pub const PIPE_TYPE_MESSAGE: DWORD = 0x00000004;
    pub const PIPE_READMODE_MESSAGE: DWORD = 0x00000002;
    pub const PIPE_WAIT: DWORD = 0x00000000;
    pub const PIPE_NOWAIT: DWORD = 0x00000001;

    pub const WAIT_OBJECT_0: DWORD = 0;
    pub const WAIT_TIMEOUT: DWORD = 258;
    pub const INFINITE: DWORD = 0xFFFFFFFF;

    pub const ERROR_ALREADY_EXISTS: DWORD = 183;
    pub const ERROR_FILE_NOT_FOUND: DWORD = 2;
    pub const ERROR_PIPE_BUSY: DWORD = 231;
    pub const ERROR_PIPE_CONNECTED: DWORD = 535;
    pub const ERROR_PIPE_LISTENING: DWORD = 536;
    pub const ERROR_NO_DATA: DWORD = 232;
    pub const ERROR_BROKEN_PIPE: DWORD = 109;
    pub const ERROR_ACCESS_DENIED: DWORD = 5;
    pub const ERROR_NOT_SUPPORTED: DWORD = 50;

    pub const NMPWAIT_WAIT_FOREVER: DWORD = 0xFFFFFFFF;

    pub const FALSE: BOOL = 0;

    #[repr(C)]
    #[derive(Default)]
    pub struct FILETIME {
        pub dwLowDateTime: u32,
        pub dwHighDateTime: u32,
    }

    unsafe extern "system" {
        // File mapping (SHM)
        pub fn CreateFileMappingW(
            hFile: HANDLE,
            lpAttributes: LPVOID,
            flProtect: DWORD,
            dwMaxSizeHigh: DWORD,
            dwMaxSizeLow: DWORD,
            lpName: LPCWSTR,
        ) -> HANDLE;
        pub fn OpenFileMappingW(
            dwDesiredAccess: DWORD,
            bInheritHandle: BOOL,
            lpName: LPCWSTR,
        ) -> HANDLE;
        pub fn MapViewOfFile(
            hFileMappingObject: HANDLE,
            dwDesiredAccess: DWORD,
            dwFileOffsetHigh: DWORD,
            dwFileOffsetLow: DWORD,
            dwNumberOfBytesToMap: SIZE_T,
        ) -> LPVOID;
        pub fn UnmapViewOfFile(lpBaseAddress: LPVOID) -> BOOL;

        // Events
        pub fn CreateEventW(
            lpAttributes: LPVOID,
            bManualReset: BOOL,
            bInitialState: BOOL,
            lpName: LPCWSTR,
        ) -> HANDLE;
        pub fn OpenEventW(
            dwDesiredAccess: DWORD,
            bInheritHandle: BOOL,
            lpName: LPCWSTR,
        ) -> HANDLE;
        pub fn SetEvent(hEvent: HANDLE) -> BOOL;
        pub fn WaitForSingleObject(hHandle: HANDLE, dwMilliseconds: DWORD) -> DWORD;

        // Named pipes
        pub fn CreateNamedPipeW(
            lpName: LPCWSTR,
            dwOpenMode: DWORD,
            dwPipeMode: DWORD,
            nMaxInstances: DWORD,
            nOutBufferSize: DWORD,
            nInBufferSize: DWORD,
            nDefaultTimeOut: DWORD,
            lpSecurityAttributes: LPVOID,
        ) -> HANDLE;
        pub fn ConnectNamedPipe(hNamedPipe: HANDLE, lpOverlapped: LPVOID) -> BOOL;
        pub fn DisconnectNamedPipe(hNamedPipe: HANDLE) -> BOOL;
        pub fn SetNamedPipeHandleState(
            hNamedPipe: HANDLE,
            lpMode: *const DWORD,
            lpMaxCollectionCount: LPVOID,
            lpCollectDataTimeout: LPVOID,
        ) -> BOOL;
        pub fn WaitNamedPipeW(lpNamedPipeName: LPCWSTR, nTimeOut: DWORD) -> BOOL;
        pub fn PeekNamedPipe(
            hNamedPipe: HANDLE,
            lpBuffer: LPVOID,
            nBufferSize: DWORD,
            lpBytesRead: *mut DWORD,
            lpTotalBytesAvail: *mut DWORD,
            lpBytesLeftThisMessage: *mut DWORD,
        ) -> BOOL;
        pub fn FlushFileBuffers(hFile: HANDLE) -> BOOL;

        // File I/O
        pub fn CreateFileW(
            lpFileName: LPCWSTR,
            dwDesiredAccess: DWORD,
            dwShareMode: DWORD,
            lpSecurityAttributes: LPVOID,
            dwCreationDisposition: DWORD,
            dwFlagsAndAttributes: DWORD,
            hTemplateFile: HANDLE,
        ) -> HANDLE;
        pub fn ReadFile(
            hFile: HANDLE,
            lpBuffer: LPVOID,
            nNumberOfBytesToRead: DWORD,
            lpNumberOfBytesRead: *mut DWORD,
            lpOverlapped: LPVOID,
        ) -> BOOL;
        pub fn WriteFile(
            hFile: HANDLE,
            lpBuffer: *const u8,
            nNumberOfBytesToWrite: DWORD,
            lpNumberOfBytesWritten: *mut DWORD,
            lpOverlapped: LPVOID,
        ) -> BOOL;

        // General
        pub fn CloseHandle(hObject: HANDLE) -> BOOL;
        pub fn GetLastError() -> DWORD;
        pub fn GetTickCount64() -> u64;
        pub fn Sleep(dwMilliseconds: DWORD);

        // CPU measurement
        pub fn GetProcessTimes(
            hProcess: HANDLE,
            lpCreationTime: *mut FILETIME,
            lpExitTime: *mut FILETIME,
            lpKernelTime: *mut FILETIME,
            lpUserTime: *mut FILETIME,
        ) -> BOOL;
        pub fn GetCurrentProcess() -> HANDLE;
    }

    pub const SYNCHRONIZE: DWORD = 0x00100000;
    pub const EVENT_MODIFY_STATE: DWORD = 0x0002;
}

use win32::*;

// ---------------------------------------------------------------------------
// Profile constants (wire-compatible with the C implementation)
// ---------------------------------------------------------------------------

pub const PROFILE_NAMED_PIPE: u32 = 1u32 << 0;
pub const PROFILE_SHM_HYBRID: u32 = 1u32 << 1;
pub const PROFILE_SHM_BUSYWAIT: u32 = 1u32 << 2;
pub const PROFILE_SHM_WAITADDR: u32 = 1u32 << 3;

const IMPLEMENTED_PROFILES: u32 = PROFILE_NAMED_PIPE | PROFILE_SHM_HYBRID;
const DEFAULT_SUPPORTED_PROFILES: u32 = PROFILE_NAMED_PIPE;
const DEFAULT_PREFERRED_PROFILES: u32 = PROFILE_NAMED_PIPE;

pub const SHM_DEFAULT_SPIN_TRIES: u32 = 1024;

// ---------------------------------------------------------------------------
// Negotiation protocol constants
// ---------------------------------------------------------------------------

const NEGOTIATION_MAGIC: u32 = 0x4e48_534b;
const NEGOTIATION_VERSION: u16 = 1;
const NEGOTIATION_HELLO: u16 = 1;
const NEGOTIATION_ACK: u16 = 2;
const NEGOTIATION_STATUS_OK: u32 = 0;

const NEG_OFF_MAGIC: usize = 0;
const NEG_OFF_VERSION: usize = 4;
const NEG_OFF_TYPE: usize = 6;
const NEG_OFF_SUPPORTED: usize = 8;
const NEG_OFF_PREFERRED: usize = 12;
const NEG_OFF_INTERSECTION: usize = 16;
const NEG_OFF_SELECTED: usize = 20;
const NEG_OFF_AUTH_TOKEN: usize = 24;
const NEG_OFF_STATUS: usize = 32;

// ---------------------------------------------------------------------------
// SHM region layout (wire-compatible with the C netipc_win_shm_region)
// ---------------------------------------------------------------------------

const SHM_REGION_MAGIC: u32 = 0x4e53_5748;
const SHM_REGION_VERSION: u32 = 2;
const CACHELINE: usize = 64;

/// Header: 64 bytes (1 cache line)
///   magic(4) + version(4) + profile(4) + spin_tries(4) + reserved(48)
const HDR_SIZE: usize = CACHELINE;

/// Request slot: 128 bytes (2 cache lines)
///   seq(8) + client_closed(4) + server_waiting(4) + reserved(48) + frame(64)
const REQ_SLOT_SIZE: usize = CACHELINE + FRAME_SIZE;

/// Response slot: 128 bytes (2 cache lines)
///   seq(8) + server_closed(4) + client_waiting(4) + reserved(48) + frame(64)
const RESP_SLOT_SIZE: usize = CACHELINE + FRAME_SIZE;

const REGION_SIZE: usize = HDR_SIZE + REQ_SLOT_SIZE + RESP_SLOT_SIZE;

// Offsets within the mapped region
const OFF_HDR_MAGIC: usize = 0;
const OFF_HDR_VERSION: usize = 4;
const OFF_HDR_PROFILE: usize = 8;
const OFF_HDR_SPIN_TRIES: usize = 12;

const OFF_REQ: usize = HDR_SIZE;
const OFF_REQ_SEQ: usize = OFF_REQ;
const OFF_REQ_CLIENT_CLOSED: usize = OFF_REQ + 8;
const OFF_REQ_SERVER_WAITING: usize = OFF_REQ + 12;
const OFF_REQ_FRAME: usize = OFF_REQ + CACHELINE;

const OFF_RESP: usize = HDR_SIZE + REQ_SLOT_SIZE;
const OFF_RESP_SEQ: usize = OFF_RESP;
const OFF_RESP_SERVER_CLOSED: usize = OFF_RESP + 8;
const OFF_RESP_CLIENT_WAITING: usize = OFF_RESP + 12;
const OFF_RESP_FRAME: usize = OFF_RESP + CACHELINE;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

#[derive(Clone, Debug)]
pub struct NamedPipeConfig {
    pub run_dir: PathBuf,
    pub service_name: String,
    pub supported_profiles: u32,
    pub preferred_profiles: u32,
    pub auth_token: u64,
    pub shm_spin_tries: u32,
}

impl NamedPipeConfig {
    pub fn new(run_dir: impl Into<PathBuf>, service_name: impl Into<String>) -> Self {
        Self {
            run_dir: run_dir.into(),
            service_name: service_name.into(),
            supported_profiles: DEFAULT_SUPPORTED_PROFILES,
            preferred_profiles: DEFAULT_PREFERRED_PROFILES,
            auth_token: 0,
            shm_spin_tries: 0,
        }
    }
}

// ---------------------------------------------------------------------------
// Helper: wide-string encoding
// ---------------------------------------------------------------------------

fn to_wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

fn win32_error(msg: &str) -> io::Error {
    let code = unsafe { GetLastError() };
    io::Error::new(
        io::ErrorKind::Other,
        format!("{msg}: win32 error {code}"),
    )
}

fn protocol_error(msg: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg)
}

fn now_ms() -> u64 {
    unsafe { GetTickCount64() }
}

fn close_handle(h: HANDLE) {
    if !h.is_null() && h != INVALID_HANDLE_VALUE {
        unsafe { CloseHandle(h); }
    }
}

// ---------------------------------------------------------------------------
// FNV-1a hash for endpoint naming
// ---------------------------------------------------------------------------

fn fnv1a64(data: &[u8]) -> u64 {
    let mut hash: u64 = 14695981039346656037;
    for &b in data {
        hash ^= b as u64;
        hash = hash.wrapping_mul(1099511628211);
    }
    hash
}

fn endpoint_hash(config: &NamedPipeConfig) -> u64 {
    let mut hash: u64 = 14695981039346656037;
    for &b in config.run_dir.to_string_lossy().as_bytes() {
        hash ^= b as u64;
        hash = hash.wrapping_mul(1099511628211);
    }
    hash ^= b'\n' as u64;
    hash = hash.wrapping_mul(1099511628211);
    for &b in config.service_name.as_bytes() {
        hash ^= b as u64;
        hash = hash.wrapping_mul(1099511628211);
    }
    hash ^= b'\n' as u64;
    hash = hash.wrapping_mul(1099511628211);
    for &b in &config.auth_token.to_ne_bytes() {
        hash ^= b as u64;
        hash = hash.wrapping_mul(1099511628211);
    }
    hash
}

fn sanitize_service(name: &str) -> String {
    let mut out = String::with_capacity(name.len().min(95));
    for ch in name.chars().take(95) {
        if ch.is_ascii_alphanumeric() || ch == '-' || ch == '_' || ch == '.' {
            out.push(ch);
        } else {
            out.push('_');
        }
    }
    if out.is_empty() {
        out.push_str("service");
    }
    out
}

fn build_pipe_name(config: &NamedPipeConfig) -> String {
    let hash = fnv1a64(config.run_dir.to_string_lossy().as_bytes());
    let svc = sanitize_service(&config.service_name);
    format!("\\\\.\\pipe\\netipc-{hash:016x}-{svc}")
}

fn build_kernel_object_name(config: &NamedPipeConfig, profile: u32, suffix: &str) -> String {
    let hash = endpoint_hash(config);
    let svc = sanitize_service(&config.service_name);
    format!("Local\\netipc-{hash:016x}-{svc}-p{profile}-{suffix}")
}

fn effective_supported(config: &NamedPipeConfig) -> u32 {
    let mut s = config.supported_profiles;
    if s == 0 { s = DEFAULT_SUPPORTED_PROFILES; }
    s &= IMPLEMENTED_PROFILES;
    if s == 0 { s = DEFAULT_SUPPORTED_PROFILES; }
    s
}

fn effective_preferred(config: &NamedPipeConfig, supported: u32) -> u32 {
    let mut p = config.preferred_profiles;
    if p == 0 { p = supported; }
    p &= supported;
    if p == 0 { p = supported; }
    p
}

fn effective_spin_tries(config: &NamedPipeConfig) -> u32 {
    if config.shm_spin_tries != 0 {
        config.shm_spin_tries
    } else {
        SHM_DEFAULT_SPIN_TRIES
    }
}

fn is_shm_profile(profile: u32) -> bool {
    profile == PROFILE_SHM_HYBRID
        || profile == PROFILE_SHM_BUSYWAIT
        || profile == PROFILE_SHM_WAITADDR
}

fn select_profile(candidates: u32) -> u32 {
    if (candidates & PROFILE_SHM_WAITADDR) != 0 { return PROFILE_SHM_WAITADDR; }
    if (candidates & PROFILE_SHM_BUSYWAIT) != 0 { return PROFILE_SHM_BUSYWAIT; }
    if (candidates & PROFILE_SHM_HYBRID) != 0 { return PROFILE_SHM_HYBRID; }
    if (candidates & PROFILE_NAMED_PIPE) != 0 { return PROFILE_NAMED_PIPE; }
    0
}

// ---------------------------------------------------------------------------
// Negotiation frames
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, Debug)]
struct NegMessage {
    ty: u16,
    supported: u32,
    preferred: u32,
    intersection: u32,
    selected: u32,
    auth_token: u64,
    status: u32,
}

fn encode_neg(msg: &NegMessage) -> Frame {
    let mut frame = [0u8; FRAME_SIZE];
    frame[NEG_OFF_MAGIC..NEG_OFF_MAGIC + 4].copy_from_slice(&NEGOTIATION_MAGIC.to_le_bytes());
    frame[NEG_OFF_VERSION..NEG_OFF_VERSION + 2].copy_from_slice(&NEGOTIATION_VERSION.to_le_bytes());
    frame[NEG_OFF_TYPE..NEG_OFF_TYPE + 2].copy_from_slice(&msg.ty.to_le_bytes());
    frame[NEG_OFF_SUPPORTED..NEG_OFF_SUPPORTED + 4].copy_from_slice(&msg.supported.to_le_bytes());
    frame[NEG_OFF_PREFERRED..NEG_OFF_PREFERRED + 4].copy_from_slice(&msg.preferred.to_le_bytes());
    frame[NEG_OFF_INTERSECTION..NEG_OFF_INTERSECTION + 4].copy_from_slice(&msg.intersection.to_le_bytes());
    frame[NEG_OFF_SELECTED..NEG_OFF_SELECTED + 4].copy_from_slice(&msg.selected.to_le_bytes());
    frame[NEG_OFF_AUTH_TOKEN..NEG_OFF_AUTH_TOKEN + 8].copy_from_slice(&msg.auth_token.to_le_bytes());
    frame[NEG_OFF_STATUS..NEG_OFF_STATUS + 4].copy_from_slice(&msg.status.to_le_bytes());
    frame
}

fn decode_neg(frame: &Frame, expected_ty: u16) -> io::Result<NegMessage> {
    let magic = u32::from_le_bytes(frame[NEG_OFF_MAGIC..NEG_OFF_MAGIC + 4].try_into().unwrap());
    let version = u16::from_le_bytes(frame[NEG_OFF_VERSION..NEG_OFF_VERSION + 2].try_into().unwrap());
    let ty = u16::from_le_bytes(frame[NEG_OFF_TYPE..NEG_OFF_TYPE + 2].try_into().unwrap());
    if magic != NEGOTIATION_MAGIC || version != NEGOTIATION_VERSION || ty != expected_ty {
        return Err(protocol_error("invalid negotiation frame"));
    }
    Ok(NegMessage {
        ty,
        supported: u32::from_le_bytes(frame[NEG_OFF_SUPPORTED..NEG_OFF_SUPPORTED + 4].try_into().unwrap()),
        preferred: u32::from_le_bytes(frame[NEG_OFF_PREFERRED..NEG_OFF_PREFERRED + 4].try_into().unwrap()),
        intersection: u32::from_le_bytes(frame[NEG_OFF_INTERSECTION..NEG_OFF_INTERSECTION + 4].try_into().unwrap()),
        selected: u32::from_le_bytes(frame[NEG_OFF_SELECTED..NEG_OFF_SELECTED + 4].try_into().unwrap()),
        auth_token: u64::from_le_bytes(frame[NEG_OFF_AUTH_TOKEN..NEG_OFF_AUTH_TOKEN + 8].try_into().unwrap()),
        status: u32::from_le_bytes(frame[NEG_OFF_STATUS..NEG_OFF_STATUS + 4].try_into().unwrap()),
    })
}

// ---------------------------------------------------------------------------
// Pipe I/O helpers
// ---------------------------------------------------------------------------

fn pipe_read_frame(pipe: HANDLE, timeout_ms: u32) -> io::Result<Frame> {
    if timeout_ms != 0 {
        let deadline = now_ms() + timeout_ms as u64;
        loop {
            let mut avail: DWORD = 0;
            let ok = unsafe {
                PeekNamedPipe(pipe, ptr::null_mut(), 0, ptr::null_mut(), &mut avail, ptr::null_mut())
            };
            if ok == 0 {
                return Err(win32_error("PeekNamedPipe"));
            }
            if avail != 0 { break; }
            if now_ms() >= deadline {
                return Err(io::Error::new(io::ErrorKind::TimedOut, "pipe read timeout"));
            }
            unsafe { Sleep(1); }
        }
    }

    let mut frame = [0u8; FRAME_SIZE];
    let mut bytes_read: DWORD = 0;
    let ok = unsafe {
        ReadFile(pipe, frame.as_mut_ptr() as LPVOID, FRAME_SIZE as DWORD, &mut bytes_read, ptr::null_mut())
    };
    if ok == 0 {
        return Err(win32_error("ReadFile"));
    }
    if bytes_read as usize != FRAME_SIZE {
        return Err(protocol_error("short pipe read"));
    }
    Ok(frame)
}

fn pipe_write_frame(pipe: HANDLE, frame: &Frame) -> io::Result<()> {
    let mut written: DWORD = 0;
    let ok = unsafe {
        WriteFile(pipe, frame.as_ptr(), FRAME_SIZE as DWORD, &mut written, ptr::null_mut())
    };
    if ok == 0 {
        return Err(win32_error("WriteFile"));
    }
    if written as usize != FRAME_SIZE {
        return Err(protocol_error("short pipe write"));
    }
    Ok(())
}

fn set_pipe_mode(pipe: HANDLE, wait_mode: DWORD) -> io::Result<()> {
    let mode = PIPE_READMODE_MESSAGE | wait_mode;
    let ok = unsafe {
        SetNamedPipeHandleState(pipe, &mode, ptr::null_mut(), ptr::null_mut())
    };
    if ok == 0 {
        Err(win32_error("SetNamedPipeHandleState"))
    } else {
        Ok(())
    }
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

fn server_handshake(
    pipe: HANDLE,
    supported: u32,
    preferred: u32,
    auth_token: u64,
    timeout_ms: u32,
) -> io::Result<u32> {
    let hello_frame = pipe_read_frame(pipe, timeout_ms)?;
    let hello = decode_neg(&hello_frame, NEGOTIATION_HELLO)?;

    let mut ack = NegMessage {
        ty: NEGOTIATION_ACK,
        supported,
        preferred,
        intersection: hello.supported & supported,
        selected: 0,
        auth_token: 0,
        status: NEGOTIATION_STATUS_OK,
    };

    if auth_token != 0 && hello.auth_token != auth_token {
        ack.status = ERROR_ACCESS_DENIED;
    } else {
        let mut candidates = ack.intersection & preferred;
        if candidates == 0 { candidates = ack.intersection; }
        ack.selected = select_profile(candidates);
        if ack.selected == 0 {
            ack.status = ERROR_NOT_SUPPORTED;
        }
    }

    let ack_frame = encode_neg(&ack);
    pipe_write_frame(pipe, &ack_frame)?;

    if ack.status != NEGOTIATION_STATUS_OK {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            format!("negotiation failed: status {}", ack.status),
        ));
    }
    Ok(ack.selected)
}

fn client_handshake(
    pipe: HANDLE,
    supported: u32,
    preferred: u32,
    auth_token: u64,
    timeout_ms: u32,
) -> io::Result<u32> {
    let hello = NegMessage {
        ty: NEGOTIATION_HELLO,
        supported,
        preferred,
        intersection: 0,
        selected: 0,
        auth_token,
        status: NEGOTIATION_STATUS_OK,
    };
    pipe_write_frame(pipe, &encode_neg(&hello))?;
    let ack_frame = pipe_read_frame(pipe, timeout_ms)?;
    let ack = decode_neg(&ack_frame, NEGOTIATION_ACK)?;

    if ack.status != NEGOTIATION_STATUS_OK {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            format!("server rejected: status {}", ack.status),
        ));
    }
    if ack.selected == 0
        || (ack.selected & supported) == 0
        || (ack.intersection & supported) == 0
    {
        return Err(protocol_error("invalid negotiated profile"));
    }
    Ok(ack.selected)
}

// ---------------------------------------------------------------------------
// SHM region atomics (via raw pointers)
// ---------------------------------------------------------------------------

#[inline]
unsafe fn load_i64_acquire(ptr: *const i64) -> i64 {
    let atom = &*(ptr as *const AtomicI64);
    atom.load(Ordering::Acquire)
}

#[inline]
unsafe fn store_i64_release(ptr: *mut i64, val: i64) {
    let atom = &*(ptr as *const AtomicI64);
    atom.store(val, Ordering::Release);
}

#[inline]
unsafe fn load_i32_acquire(ptr: *const i32) -> i32 {
    let atom = &*(ptr as *const AtomicI32);
    atom.load(Ordering::Acquire)
}

#[inline]
unsafe fn store_i32_release(ptr: *mut i32, val: i32) {
    let atom = &*(ptr as *const AtomicI32);
    atom.store(val, Ordering::Release);
}

// ---------------------------------------------------------------------------
// SHM Server
// ---------------------------------------------------------------------------

struct ShmServer {
    mapping: HANDLE,
    request_event: HANDLE,
    response_event: HANDLE,
    region: *mut u8,
    last_request_seq: i64,
    active_request_seq: i64,
    spin_tries: u32,
}

impl ShmServer {
    fn create(config: &NamedPipeConfig, profile: u32) -> io::Result<Self> {
        let mapping_name = to_wide(&build_kernel_object_name(config, profile, "shm"));
        let req_event_name = to_wide(&build_kernel_object_name(config, profile, "req"));
        let resp_event_name = to_wide(&build_kernel_object_name(config, profile, "resp"));

        let mapping = unsafe {
            CreateFileMappingW(
                INVALID_HANDLE_VALUE, ptr::null_mut(), PAGE_READWRITE,
                0, REGION_SIZE as DWORD, mapping_name.as_ptr(),
            )
        };
        if mapping.is_null() {
            return Err(win32_error("CreateFileMappingW"));
        }
        if unsafe { GetLastError() } == ERROR_ALREADY_EXISTS {
            close_handle(mapping);
            return Err(io::Error::new(io::ErrorKind::AlreadyExists, "SHM already exists"));
        }

        let region = unsafe {
            MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, REGION_SIZE)
        } as *mut u8;
        if region.is_null() {
            close_handle(mapping);
            return Err(win32_error("MapViewOfFile"));
        }

        // Zero and init header
        unsafe {
            ptr::write_bytes(region, 0, REGION_SIZE);
            ptr::copy_nonoverlapping(SHM_REGION_MAGIC.to_le_bytes().as_ptr(), region.add(OFF_HDR_MAGIC), 4);
            ptr::copy_nonoverlapping(SHM_REGION_VERSION.to_le_bytes().as_ptr(), region.add(OFF_HDR_VERSION), 4);
            ptr::copy_nonoverlapping(profile.to_le_bytes().as_ptr(), region.add(OFF_HDR_PROFILE), 4);
            let spin = effective_spin_tries(config);
            ptr::copy_nonoverlapping(spin.to_le_bytes().as_ptr(), region.add(OFF_HDR_SPIN_TRIES), 4);
        }

        // Create events (auto-reset)
        let request_event = unsafe {
            CreateEventW(ptr::null_mut(), FALSE, FALSE, req_event_name.as_ptr())
        };
        if request_event.is_null() {
            unsafe { UnmapViewOfFile(region as LPVOID); }
            close_handle(mapping);
            return Err(win32_error("CreateEventW(req)"));
        }
        let response_event = unsafe {
            CreateEventW(ptr::null_mut(), FALSE, FALSE, resp_event_name.as_ptr())
        };
        if response_event.is_null() {
            close_handle(request_event);
            unsafe { UnmapViewOfFile(region as LPVOID); }
            close_handle(mapping);
            return Err(win32_error("CreateEventW(resp)"));
        }

        Ok(ShmServer {
            mapping,
            request_event,
            response_event,
            region,
            last_request_seq: 0,
            active_request_seq: 0,
            spin_tries: effective_spin_tries(config),
        })
    }

    fn receive_frame(&mut self, timeout_ms: u32) -> io::Result<Frame> {
        let deadline = if timeout_ms == 0 { 0u64 } else { now_ms() + timeout_ms as u64 };
        let mut spins = self.spin_tries;

        loop {
            let current = unsafe { load_i64_acquire(self.region.add(OFF_REQ_SEQ) as *const i64) };
            if current != self.last_request_seq {
                let mut frame = [0u8; FRAME_SIZE];
                unsafe {
                    ptr::copy_nonoverlapping(self.region.add(OFF_REQ_FRAME), frame.as_mut_ptr(), FRAME_SIZE);
                }
                self.active_request_seq = current;
                self.last_request_seq = current;
                return Ok(frame);
            }

            if unsafe { load_i32_acquire(self.region.add(OFF_REQ_CLIENT_CLOSED) as *const i32) } != 0 {
                return Err(io::Error::new(io::ErrorKind::BrokenPipe, "client closed"));
            }

            std::hint::spin_loop();

            if spins != 0 {
                spins -= 1;
                continue;
            }

            // Mark waiting, then re-check.
            // SeqCst fence prevents store-load reordering: ensures the store of
            // SERVER_WAITING=1 is globally visible before we re-read REQ_SEQ.
            // Without this, the client's store of REQ_SEQ and our store of
            // SERVER_WAITING can miss each other (classic Dekker race on x86).
            unsafe { store_i32_release(self.region.add(OFF_REQ_SERVER_WAITING) as *mut i32, 1); }
            fence(Ordering::SeqCst);

            let current = unsafe { load_i64_acquire(self.region.add(OFF_REQ_SEQ) as *const i64) };
            if current != self.last_request_seq {
                unsafe { store_i32_release(self.region.add(OFF_REQ_SERVER_WAITING) as *mut i32, 0); }
                let mut frame = [0u8; FRAME_SIZE];
                unsafe {
                    ptr::copy_nonoverlapping(self.region.add(OFF_REQ_FRAME), frame.as_mut_ptr(), FRAME_SIZE);
                }
                self.active_request_seq = current;
                self.last_request_seq = current;
                return Ok(frame);
            }

            let wait_ms = if deadline == 0 {
                INFINITE
            } else {
                let now = now_ms();
                if now >= deadline {
                    unsafe { store_i32_release(self.region.add(OFF_REQ_SERVER_WAITING) as *mut i32, 0); }
                    return Err(io::Error::new(io::ErrorKind::TimedOut, "SHM receive timeout"));
                }
                (deadline - now) as DWORD
            };

            let rc = unsafe { WaitForSingleObject(self.request_event, wait_ms) };
            unsafe { store_i32_release(self.region.add(OFF_REQ_SERVER_WAITING) as *mut i32, 0); }

            if rc == WAIT_OBJECT_0 {
                spins = self.spin_tries;
                continue;
            }
            if rc == WAIT_TIMEOUT {
                return Err(io::Error::new(io::ErrorKind::TimedOut, "SHM receive timeout"));
            }
            return Err(win32_error("WaitForSingleObject(req)"));
        }
    }

    fn send_frame(&mut self, frame: &Frame) -> io::Result<()> {
        if self.active_request_seq == 0 {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "no active request"));
        }
        unsafe {
            ptr::copy_nonoverlapping(frame.as_ptr(), self.region.add(OFF_RESP_FRAME), FRAME_SIZE);
            store_i64_release(self.region.add(OFF_RESP_SEQ) as *mut i64, self.active_request_seq);

            // SeqCst fence prevents store-load reordering: ensures the store of
            // RESP_SEQ is globally visible before we read CLIENT_WAITING.
            fence(Ordering::SeqCst);

            // Conditional SetEvent
            if load_i32_acquire(self.region.add(OFF_RESP_CLIENT_WAITING) as *const i32) != 0 {
                SetEvent(self.response_event);
            }
        }
        self.active_request_seq = 0;
        Ok(())
    }
}

impl Drop for ShmServer {
    fn drop(&mut self) {
        if !self.region.is_null() {
            unsafe {
                store_i32_release(self.region.add(OFF_RESP_SERVER_CLOSED) as *mut i32, 1);
            }
            // Wake up any waiting client
            if self.request_event != INVALID_HANDLE_VALUE && !self.request_event.is_null() {
                unsafe { SetEvent(self.request_event); }
            }
            if self.response_event != INVALID_HANDLE_VALUE && !self.response_event.is_null() {
                unsafe { SetEvent(self.response_event); }
            }
            unsafe { UnmapViewOfFile(self.region as LPVOID); }
        }
        close_handle(self.response_event);
        close_handle(self.request_event);
        close_handle(self.mapping);
    }
}

// ---------------------------------------------------------------------------
// SHM Client
// ---------------------------------------------------------------------------

struct ShmClient {
    mapping: HANDLE,
    request_event: HANDLE,
    response_event: HANDLE,
    region: *mut u8,
    next_request_seq: i64,
    spin_tries: u32,
}

impl ShmClient {
    fn create(config: &NamedPipeConfig, profile: u32, timeout_ms: u32) -> io::Result<Self> {
        let mapping_name = to_wide(&build_kernel_object_name(config, profile, "shm"));
        let req_event_name = to_wide(&build_kernel_object_name(config, profile, "req"));
        let resp_event_name = to_wide(&build_kernel_object_name(config, profile, "resp"));

        let deadline = if timeout_ms == 0 { 0u64 } else { now_ms() + timeout_ms as u64 };

        let (mapping, request_event, response_event) = loop {
            let m = unsafe { OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping_name.as_ptr()) };
            if !m.is_null() {
                let re = unsafe {
                    OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, req_event_name.as_ptr())
                };
                let rsp = unsafe {
                    OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, resp_event_name.as_ptr())
                };
                if !re.is_null() && !rsp.is_null() {
                    break (m, re, rsp);
                }
                close_handle(rsp);
                close_handle(re);
                close_handle(m);
            }
            if deadline != 0 && now_ms() >= deadline {
                return Err(io::Error::new(io::ErrorKind::TimedOut, "SHM connect timeout"));
            }
            unsafe { Sleep(1); }
        };

        let region = unsafe {
            MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, REGION_SIZE)
        } as *mut u8;
        if region.is_null() {
            close_handle(response_event);
            close_handle(request_event);
            close_handle(mapping);
            return Err(win32_error("MapViewOfFile(client)"));
        }

        // Wait for region ready
        let ready_deadline = if timeout_ms == 0 { 0u64 } else { now_ms() + timeout_ms as u64 };
        loop {
            let magic = unsafe { u32::from_le_bytes(std::slice::from_raw_parts(region.add(OFF_HDR_MAGIC), 4).try_into().unwrap()) };
            let ver = unsafe { u32::from_le_bytes(std::slice::from_raw_parts(region.add(OFF_HDR_VERSION), 4).try_into().unwrap()) };
            let prof = unsafe { u32::from_le_bytes(std::slice::from_raw_parts(region.add(OFF_HDR_PROFILE), 4).try_into().unwrap()) };
            if magic == SHM_REGION_MAGIC && ver == SHM_REGION_VERSION && prof == profile {
                break;
            }
            if ready_deadline != 0 && now_ms() >= ready_deadline {
                unsafe { UnmapViewOfFile(region as LPVOID); }
                close_handle(response_event);
                close_handle(request_event);
                close_handle(mapping);
                return Err(io::Error::new(io::ErrorKind::TimedOut, "SHM region not ready"));
            }
            unsafe { Sleep(1); }
        }

        // Read spin_tries from region header
        let region_spin = unsafe {
            u32::from_le_bytes(std::slice::from_raw_parts(region.add(OFF_HDR_SPIN_TRIES), 4).try_into().unwrap())
        };
        let spin_tries = if region_spin != 0 { region_spin } else { effective_spin_tries(config) };

        let next_req_seq = unsafe { load_i64_acquire(region.add(OFF_REQ_SEQ) as *const i64) };

        Ok(ShmClient {
            mapping,
            request_event,
            response_event,
            region,
            next_request_seq: next_req_seq,
            spin_tries,
        })
    }

    fn call_frame(&mut self, request: &Frame, timeout_ms: u32) -> io::Result<Frame> {
        let request_seq = self.next_request_seq + 1;
        self.next_request_seq = request_seq;

        unsafe {
            ptr::copy_nonoverlapping(request.as_ptr(), self.region.add(OFF_REQ_FRAME), FRAME_SIZE);
            store_i64_release(self.region.add(OFF_REQ_SEQ) as *mut i64, request_seq);

            // SeqCst fence prevents store-load reordering: ensures the store of
            // REQ_SEQ is globally visible before we read SERVER_WAITING.
            fence(Ordering::SeqCst);

            // Conditional SetEvent
            if load_i32_acquire(self.region.add(OFF_REQ_SERVER_WAITING) as *const i32) != 0 {
                SetEvent(self.request_event);
            }
        }

        // Wait for response
        let deadline = if timeout_ms == 0 { 0u64 } else { now_ms() + timeout_ms as u64 };
        let mut spins = self.spin_tries;

        loop {
            let current = unsafe { load_i64_acquire(self.region.add(OFF_RESP_SEQ) as *const i64) };
            if current >= request_seq {
                let mut response = [0u8; FRAME_SIZE];
                unsafe {
                    ptr::copy_nonoverlapping(self.region.add(OFF_RESP_FRAME), response.as_mut_ptr(), FRAME_SIZE);
                }
                return Ok(response);
            }

            if unsafe { load_i32_acquire(self.region.add(OFF_RESP_SERVER_CLOSED) as *const i32) } != 0 {
                return Err(io::Error::new(io::ErrorKind::BrokenPipe, "server closed"));
            }

            std::hint::spin_loop();

            if spins != 0 {
                spins -= 1;
                continue;
            }

            // Mark waiting + SeqCst fence (same Dekker race prevention)
            unsafe { store_i32_release(self.region.add(OFF_RESP_CLIENT_WAITING) as *mut i32, 1); }
            fence(Ordering::SeqCst);

            let current = unsafe { load_i64_acquire(self.region.add(OFF_RESP_SEQ) as *const i64) };
            if current >= request_seq {
                unsafe { store_i32_release(self.region.add(OFF_RESP_CLIENT_WAITING) as *mut i32, 0); }
                let mut response = [0u8; FRAME_SIZE];
                unsafe {
                    ptr::copy_nonoverlapping(self.region.add(OFF_RESP_FRAME), response.as_mut_ptr(), FRAME_SIZE);
                }
                return Ok(response);
            }

            let wait_ms = if deadline == 0 {
                INFINITE
            } else {
                let now = now_ms();
                if now >= deadline {
                    unsafe { store_i32_release(self.region.add(OFF_RESP_CLIENT_WAITING) as *mut i32, 0); }
                    return Err(io::Error::new(io::ErrorKind::TimedOut, "SHM response timeout"));
                }
                (deadline - now) as DWORD
            };

            let rc = unsafe { WaitForSingleObject(self.response_event, wait_ms) };
            unsafe { store_i32_release(self.region.add(OFF_RESP_CLIENT_WAITING) as *mut i32, 0); }

            if rc == WAIT_OBJECT_0 {
                spins = self.spin_tries;
                continue;
            }
            if rc == WAIT_TIMEOUT {
                return Err(io::Error::new(io::ErrorKind::TimedOut, "SHM response timeout"));
            }
            return Err(win32_error("WaitForSingleObject(resp)"));
        }
    }
}

impl Drop for ShmClient {
    fn drop(&mut self) {
        if !self.region.is_null() {
            unsafe {
                store_i32_release(self.region.add(OFF_REQ_CLIENT_CLOSED) as *mut i32, 1);
            }
            if self.request_event != INVALID_HANDLE_VALUE && !self.request_event.is_null() {
                unsafe { SetEvent(self.request_event); }
            }
            if self.response_event != INVALID_HANDLE_VALUE && !self.response_event.is_null() {
                unsafe { SetEvent(self.response_event); }
            }
            unsafe { UnmapViewOfFile(self.region as LPVOID); }
        }
        close_handle(self.response_event);
        close_handle(self.request_event);
        close_handle(self.mapping);
    }
}

// ---------------------------------------------------------------------------
// Public API: NamedPipeServer
// ---------------------------------------------------------------------------

pub struct NamedPipeServer {
    pipe: HANDLE,
    run_dir: String,
    service_name: String,
    supported_profiles: u32,
    preferred_profiles: u32,
    auth_token: u64,
    shm_spin_tries: u32,
    negotiated_profile: u32,
    shm_server: Option<ShmServer>,
    connected: bool,
}

impl NamedPipeServer {
    pub fn bind(config: &NamedPipeConfig) -> io::Result<Self> {
        let pipe_name = to_wide(&build_pipe_name(config));
        let supported = effective_supported(config);
        let preferred = effective_preferred(config, supported);

        let pipe = unsafe {
            CreateNamedPipeW(
                pipe_name.as_ptr(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                1,
                FRAME_SIZE as DWORD * 4,
                FRAME_SIZE as DWORD * 4,
                0,
                ptr::null_mut(),
            )
        };
        if pipe == INVALID_HANDLE_VALUE {
            return Err(win32_error("CreateNamedPipeW"));
        }

        Ok(NamedPipeServer {
            pipe,
            run_dir: config.run_dir.to_string_lossy().into_owned(),
            service_name: config.service_name.clone(),
            supported_profiles: supported,
            preferred_profiles: preferred,
            auth_token: config.auth_token,
            shm_spin_tries: config.shm_spin_tries,
            negotiated_profile: 0,
            shm_server: None,
            connected: false,
        })
    }

    pub fn accept(&mut self, timeout: Option<Duration>) -> io::Result<()> {
        let timeout_ms = timeout.map(|d| d.as_millis() as u32).unwrap_or(0);

        set_pipe_mode(self.pipe, PIPE_NOWAIT)?;

        let deadline = if timeout_ms == 0 { 0u64 } else { now_ms() + timeout_ms as u64 };
        loop {
            let ok = unsafe { ConnectNamedPipe(self.pipe, ptr::null_mut()) };
            if ok != 0 { break; }
            let err = unsafe { GetLastError() };
            if err == ERROR_PIPE_CONNECTED { break; }
            if err != ERROR_PIPE_LISTENING && err != ERROR_NO_DATA {
                let _ = set_pipe_mode(self.pipe, PIPE_WAIT);
                return Err(win32_error("ConnectNamedPipe"));
            }
            if deadline != 0 && now_ms() >= deadline {
                let _ = set_pipe_mode(self.pipe, PIPE_WAIT);
                return Err(io::Error::new(io::ErrorKind::TimedOut, "accept timeout"));
            }
            unsafe { Sleep(1); }
        }

        set_pipe_mode(self.pipe, PIPE_WAIT)?;

        let profile = server_handshake(
            self.pipe, self.supported_profiles, self.preferred_profiles,
            self.auth_token, timeout_ms,
        )?;

        if is_shm_profile(profile) {
            let shm_config = NamedPipeConfig {
                run_dir: self.run_dir.clone().into(),
                service_name: self.service_name.clone(),
                supported_profiles: self.supported_profiles,
                preferred_profiles: self.preferred_profiles,
                auth_token: self.auth_token,
                shm_spin_tries: self.shm_spin_tries,
            };
            self.shm_server = Some(ShmServer::create(&shm_config, profile)?);
        }

        self.negotiated_profile = profile;
        self.connected = true;
        Ok(())
    }

    pub fn receive_frame(&mut self, timeout: Option<Duration>) -> io::Result<Frame> {
        let timeout_ms = timeout.map(|d| d.as_millis() as u32).unwrap_or(0);
        if let Some(ref mut shm) = self.shm_server {
            return shm.receive_frame(timeout_ms);
        }
        pipe_read_frame(self.pipe, timeout_ms)
    }

    pub fn send_frame(&mut self, frame: &Frame) -> io::Result<()> {
        if let Some(ref mut shm) = self.shm_server {
            return shm.send_frame(frame);
        }
        pipe_write_frame(self.pipe, frame)
    }

    pub fn receive_increment(&mut self, timeout: Option<Duration>) -> io::Result<(u64, IncrementRequest)> {
        let frame = self.receive_frame(timeout)?;
        decode_increment_request(&frame)
    }

    pub fn send_increment(&mut self, request_id: u64, response: &IncrementResponse, _timeout: Option<Duration>) -> io::Result<()> {
        let frame = encode_increment_response(request_id, response);
        self.send_frame(&frame)
    }

    pub fn negotiated_profile(&self) -> u32 {
        self.negotiated_profile
    }
}

impl Drop for NamedPipeServer {
    fn drop(&mut self) {
        // Drop SHM first
        self.shm_server.take();
        if self.pipe != INVALID_HANDLE_VALUE {
            if self.connected {
                unsafe {
                    FlushFileBuffers(self.pipe);
                    DisconnectNamedPipe(self.pipe);
                }
            }
            close_handle(self.pipe);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API: NamedPipeClient
// ---------------------------------------------------------------------------

pub struct NamedPipeClient {
    pipe: HANDLE,
    negotiated_profile: u32,
    shm_client: Option<ShmClient>,
    next_request_id: u64,
}

impl NamedPipeClient {
    pub fn connect(config: &NamedPipeConfig, timeout: Option<Duration>) -> io::Result<Self> {
        let timeout_ms = timeout.map(|d| d.as_millis() as u32).unwrap_or(0);
        let pipe_name = to_wide(&build_pipe_name(config));
        let supported = effective_supported(config);
        let preferred = effective_preferred(config, supported);

        let deadline = if timeout_ms == 0 { 0u64 } else { now_ms() + timeout_ms as u64 };
        let pipe = loop {
            let h = unsafe {
                CreateFileW(
                    pipe_name.as_ptr(),
                    GENERIC_READ | GENERIC_WRITE,
                    0, ptr::null_mut(),
                    OPEN_EXISTING, 0, NULL_HANDLE,
                )
            };
            if h != INVALID_HANDLE_VALUE {
                break h;
            }
            let err = unsafe { GetLastError() };
            if err != ERROR_FILE_NOT_FOUND && err != ERROR_PIPE_BUSY {
                return Err(win32_error("CreateFileW(pipe)"));
            }
            if deadline != 0 && now_ms() >= deadline {
                return Err(io::Error::new(io::ErrorKind::TimedOut, "pipe connect timeout"));
            }
            if err == ERROR_PIPE_BUSY {
                let wait = if timeout_ms == 0 { NMPWAIT_WAIT_FOREVER } else { 50 };
                unsafe { WaitNamedPipeW(pipe_name.as_ptr(), wait); }
            } else {
                unsafe { Sleep(1); }
            }
        };

        set_pipe_mode(pipe, PIPE_WAIT)?;

        let profile = client_handshake(pipe, supported, preferred, config.auth_token, timeout_ms)?;

        let shm_client = if is_shm_profile(profile) {
            let shm_config = NamedPipeConfig {
                run_dir: config.run_dir.clone(),
                service_name: config.service_name.clone(),
                supported_profiles: config.supported_profiles,
                preferred_profiles: config.preferred_profiles,
                auth_token: config.auth_token,
                shm_spin_tries: config.shm_spin_tries,
            };
            Some(ShmClient::create(&shm_config, profile, timeout_ms)?)
        } else {
            None
        };

        Ok(NamedPipeClient {
            pipe,
            negotiated_profile: profile,
            shm_client,
            next_request_id: 1,
        })
    }

    pub fn call_frame(&mut self, request: &Frame, timeout: Option<Duration>) -> io::Result<Frame> {
        let timeout_ms = timeout.map(|d| d.as_millis() as u32).unwrap_or(0);
        if let Some(ref mut shm) = self.shm_client {
            return shm.call_frame(request, timeout_ms);
        }
        pipe_write_frame(self.pipe, request)?;
        pipe_read_frame(self.pipe, timeout_ms)
    }

    pub fn call_increment(&mut self, request: &IncrementRequest, timeout: Option<Duration>) -> io::Result<IncrementResponse> {
        let id = self.next_request_id;
        self.next_request_id += 1;
        let req_frame = encode_increment_request(id, request);
        let resp_frame = self.call_frame(&req_frame, timeout)?;
        let (resp_id, response) = decode_increment_response(&resp_frame)?;
        if resp_id != id {
            return Err(protocol_error("response request_id mismatch"));
        }
        Ok(response)
    }

    pub fn negotiated_profile(&self) -> u32 {
        self.negotiated_profile
    }
}

impl Drop for NamedPipeClient {
    fn drop(&mut self) {
        self.shm_client.take();
        close_handle(self.pipe);
    }
}

// ---------------------------------------------------------------------------
// CPU measurement helper
// ---------------------------------------------------------------------------

pub fn self_cpu_seconds() -> f64 {
    unsafe {
        let mut creation = FILETIME::default();
        let mut exit = FILETIME::default();
        let mut kernel = FILETIME::default();
        let mut user = FILETIME::default();
        if GetProcessTimes(
            GetCurrentProcess(),
            &mut creation,
            &mut exit,
            &mut kernel,
            &mut user,
        ) == 0
        {
            return 0.0;
        }
        let k = ((kernel.dwHighDateTime as u64) << 32 | kernel.dwLowDateTime as u64) as f64 / 1e7;
        let u = ((user.dwHighDateTime as u64) << 32 | user.dwLowDateTime as u64) as f64 / 1e7;
        k + u
    }
}
