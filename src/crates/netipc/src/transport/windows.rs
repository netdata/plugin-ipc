//! Windows transport implementation for the Rust crate.
//!
//! Provides Named Pipe and SHM HYBRID transports, mirroring the C
//! implementation in `netipc_named_pipe.c` / `netipc_shm_hybrid_win.c`.
//! The negotiation protocol and SHM region layout are wire-compatible
//! with the C version.

use crate::protocol::{
    decode_chunk_header, decode_hello_ack_payload, decode_hello_payload, decode_increment_request,
    decode_increment_response, decode_message_header, encode_chunk_header,
    encode_hello_ack_payload, encode_hello_payload, encode_increment_request,
    encode_increment_response, max_batch_total_size, message_total_size, ChunkHeader, Frame,
    HelloAckPayload, HelloPayload, IncrementRequest, IncrementResponse, CHUNK_HEADER_LEN,
    CHUNK_MAGIC, CHUNK_VERSION, CONTROL_HELLO_ACK_PAYLOAD_LEN, CONTROL_HELLO_PAYLOAD_LEN,
    FRAME_SIZE, MAX_PAYLOAD_DEFAULT, MESSAGE_HEADER_LEN, MESSAGE_VERSION,
};
use std::io;
use std::path::PathBuf;
use std::ptr;
use std::sync::atomic::{fence, AtomicI32, AtomicI64, Ordering};
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
    pub const ERROR_PIPE_NOT_CONNECTED: DWORD = 233;
    pub const ERROR_NO_DATA: DWORD = 232;
    pub const ERROR_BROKEN_PIPE: DWORD = 109;
    pub const ERROR_ACCESS_DENIED: DWORD = 5;
    pub const ERROR_NOT_SUPPORTED: DWORD = 50;
    pub const ERROR_INVALID_PARAMETER: DWORD = 87;

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
        pub fn OpenEventW(dwDesiredAccess: DWORD, bInheritHandle: BOOL, lpName: LPCWSTR) -> HANDLE;
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
const NEGOTIATION_PAYLOAD_OFFSET: usize = 8;
const NEGOTIATION_STATUS_OFFSET: usize = 48;
const NEGOTIATION_DEFAULT_BATCH_ITEMS: u32 = 1;

const NEG_OFF_MAGIC: usize = 0;
const NEG_OFF_VERSION: usize = 4;
const NEG_OFF_TYPE: usize = 6;

// ---------------------------------------------------------------------------
// SHM region layout (wire-compatible with the C netipc_win_shm_header v3)
// ---------------------------------------------------------------------------

const SHM_REGION_MAGIC: u32 = 0x4e53_5748;
const SHM_REGION_VERSION: u32 = 3;
const CACHELINE: usize = 64;
const HDR_SIZE: usize = 128;

const OFF_HDR_MAGIC: usize = 0;
const OFF_HDR_VERSION: usize = 4;
const OFF_HDR_HEADER_LEN: usize = 8;
const OFF_HDR_PROFILE: usize = 12;
const OFF_HDR_REQ_OFFSET: usize = 16;
const OFF_HDR_REQ_CAPACITY: usize = 20;
const OFF_HDR_RESP_OFFSET: usize = 24;
const OFF_HDR_RESP_CAPACITY: usize = 28;
const OFF_HDR_SPIN_TRIES: usize = 32;
const OFF_REQ_LEN: usize = 36;
const OFF_RESP_LEN: usize = 40;
const OFF_REQ_CLIENT_CLOSED: usize = 44;
const OFF_REQ_SERVER_WAITING: usize = 48;
const OFF_RESP_SERVER_CLOSED: usize = 52;
const OFF_RESP_CLIENT_WAITING: usize = 56;
const OFF_REQ_SEQ: usize = 64;
const OFF_RESP_SEQ: usize = 72;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

#[derive(Clone, Debug)]
pub struct NamedPipeConfig {
    pub run_dir: PathBuf,
    pub service_name: String,
    pub supported_profiles: u32,
    pub preferred_profiles: u32,
    pub max_request_payload_bytes: u32,
    pub max_request_batch_items: u32,
    pub max_response_payload_bytes: u32,
    pub max_response_batch_items: u32,
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
            max_request_payload_bytes: MAX_PAYLOAD_DEFAULT,
            max_request_batch_items: NEGOTIATION_DEFAULT_BATCH_ITEMS,
            max_response_payload_bytes: MAX_PAYLOAD_DEFAULT,
            max_response_batch_items: NEGOTIATION_DEFAULT_BATCH_ITEMS,
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
    io::Error::new(io::ErrorKind::Other, format!("{msg}: win32 error {code}"))
}

fn protocol_error(msg: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg)
}

fn now_ms() -> u64 {
    unsafe { GetTickCount64() }
}

fn close_handle(h: HANDLE) {
    if !h.is_null() && h != INVALID_HANDLE_VALUE {
        unsafe {
            CloseHandle(h);
        }
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
    if s == 0 {
        s = DEFAULT_SUPPORTED_PROFILES;
    }
    s &= IMPLEMENTED_PROFILES;
    if s == 0 {
        s = DEFAULT_SUPPORTED_PROFILES;
    }
    s
}

fn effective_preferred(config: &NamedPipeConfig, supported: u32) -> u32 {
    let mut p = config.preferred_profiles;
    if p == 0 {
        p = supported;
    }
    p &= supported;
    if p == 0 {
        p = supported;
    }
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
    if (candidates & PROFILE_SHM_WAITADDR) != 0 {
        return PROFILE_SHM_WAITADDR;
    }
    if (candidates & PROFILE_SHM_BUSYWAIT) != 0 {
        return PROFILE_SHM_BUSYWAIT;
    }
    if (candidates & PROFILE_SHM_HYBRID) != 0 {
        return PROFILE_SHM_HYBRID;
    }
    if (candidates & PROFILE_NAMED_PIPE) != 0 {
        return PROFILE_NAMED_PIPE;
    }
    0
}

// ---------------------------------------------------------------------------
// Negotiation frames
// ---------------------------------------------------------------------------

fn negotiate_limit_u32(offered: u32, local_limit: u32) -> u32 {
    if offered == 0 || local_limit == 0 {
        return 0;
    }
    offered.min(local_limit)
}

fn effective_payload_limit(value: u32) -> u32 {
    if value == 0 {
        MAX_PAYLOAD_DEFAULT
    } else {
        value
    }
}

fn effective_batch_limit(value: u32) -> u32 {
    if value == 0 {
        NEGOTIATION_DEFAULT_BATCH_ITEMS
    } else {
        value
    }
}

fn compute_max_message_len(max_payload_bytes: u32, max_batch_items: u32) -> io::Result<usize> {
    max_batch_total_size(max_payload_bytes, max_batch_items)
}

fn align_up_size(value: usize, alignment: usize) -> usize {
    let remainder = value % alignment;
    if remainder == 0 {
        value
    } else {
        value + (alignment - remainder)
    }
}

fn compute_region_layout(
    request_capacity: usize,
    response_capacity: usize,
) -> io::Result<(usize, usize, usize)> {
    if request_capacity == 0 || response_capacity == 0 {
        return Err(io::Error::from_raw_os_error(87));
    }

    let request_offset = align_up_size(HDR_SIZE, CACHELINE);
    let response_offset = align_up_size(request_offset + request_capacity, CACHELINE);
    let mapping_len = response_offset + response_capacity;
    Ok((request_offset, response_offset, mapping_len))
}

fn request_area(region: *mut u8) -> usize {
    unsafe {
        u32::from_le_bytes(
            std::slice::from_raw_parts(region.add(OFF_HDR_REQ_OFFSET), 4)
                .try_into()
                .unwrap(),
        ) as usize
    }
}

fn response_area(region: *mut u8) -> usize {
    unsafe {
        u32::from_le_bytes(
            std::slice::from_raw_parts(region.add(OFF_HDR_RESP_OFFSET), 4)
                .try_into()
                .unwrap(),
        ) as usize
    }
}

fn request_capacity(region: *mut u8) -> usize {
    unsafe {
        u32::from_le_bytes(
            std::slice::from_raw_parts(region.add(OFF_HDR_REQ_CAPACITY), 4)
                .try_into()
                .unwrap(),
        ) as usize
    }
}

fn response_capacity(region: *mut u8) -> usize {
    unsafe {
        u32::from_le_bytes(
            std::slice::from_raw_parts(region.add(OFF_HDR_RESP_CAPACITY), 4)
                .try_into()
                .unwrap(),
        ) as usize
    }
}

fn validate_region_header(region: *mut u8, mapping_len: usize, profile: u32) -> io::Result<u32> {
    let magic = unsafe {
        u32::from_le_bytes(
            std::slice::from_raw_parts(region.add(OFF_HDR_MAGIC), 4)
                .try_into()
                .unwrap(),
        )
    };
    let version = unsafe {
        u32::from_le_bytes(
            std::slice::from_raw_parts(region.add(OFF_HDR_VERSION), 4)
                .try_into()
                .unwrap(),
        )
    };
    let header_len = unsafe {
        u32::from_le_bytes(
            std::slice::from_raw_parts(region.add(OFF_HDR_HEADER_LEN), 4)
                .try_into()
                .unwrap(),
        ) as usize
    };
    let prof = unsafe {
        u32::from_le_bytes(
            std::slice::from_raw_parts(region.add(OFF_HDR_PROFILE), 4)
                .try_into()
                .unwrap(),
        )
    };
    let req_off = request_area(region);
    let req_cap = request_capacity(region);
    let resp_off = response_area(region);
    let resp_cap = response_capacity(region);
    let spin = unsafe {
        u32::from_le_bytes(
            std::slice::from_raw_parts(region.add(OFF_HDR_SPIN_TRIES), 4)
                .try_into()
                .unwrap(),
        )
    };

    if magic != SHM_REGION_MAGIC
        || version != SHM_REGION_VERSION
        || header_len != HDR_SIZE
        || prof != profile
        || req_cap == 0
        || resp_cap == 0
    {
        return Err(protocol_error("invalid SHM header"));
    }
    if req_off < header_len {
        return Err(protocol_error("invalid SHM request offset"));
    }
    if resp_off < req_off + req_cap {
        return Err(protocol_error("invalid SHM response offset"));
    }
    if resp_off + resp_cap > mapping_len {
        return Err(protocol_error("invalid SHM mapping length"));
    }
    Ok(spin)
}

fn encode_neg_header(ty: u16) -> Frame {
    let mut frame = [0u8; FRAME_SIZE];
    frame[NEG_OFF_MAGIC..NEG_OFF_MAGIC + 4].copy_from_slice(&NEGOTIATION_MAGIC.to_le_bytes());
    frame[NEG_OFF_VERSION..NEG_OFF_VERSION + 2].copy_from_slice(&NEGOTIATION_VERSION.to_le_bytes());
    frame[NEG_OFF_TYPE..NEG_OFF_TYPE + 2].copy_from_slice(&ty.to_le_bytes());
    frame
}

fn encode_hello_neg(payload: &HelloPayload) -> Frame {
    let mut frame = encode_neg_header(NEGOTIATION_HELLO);
    frame[NEGOTIATION_PAYLOAD_OFFSET..NEGOTIATION_PAYLOAD_OFFSET + CONTROL_HELLO_PAYLOAD_LEN]
        .copy_from_slice(&encode_hello_payload(payload));
    frame
}

fn encode_ack_neg(payload: &HelloAckPayload, status: u32) -> Frame {
    let mut frame = encode_neg_header(NEGOTIATION_ACK);
    frame[NEGOTIATION_PAYLOAD_OFFSET..NEGOTIATION_PAYLOAD_OFFSET + CONTROL_HELLO_ACK_PAYLOAD_LEN]
        .copy_from_slice(&encode_hello_ack_payload(payload));
    frame[NEGOTIATION_STATUS_OFFSET..NEGOTIATION_STATUS_OFFSET + 4]
        .copy_from_slice(&status.to_le_bytes());
    frame
}

fn decode_neg_header(frame: &Frame, expected_ty: u16) -> io::Result<()> {
    let magic = u32::from_le_bytes(frame[NEG_OFF_MAGIC..NEG_OFF_MAGIC + 4].try_into().unwrap());
    let version = u16::from_le_bytes(
        frame[NEG_OFF_VERSION..NEG_OFF_VERSION + 2]
            .try_into()
            .unwrap(),
    );
    let ty = u16::from_le_bytes(frame[NEG_OFF_TYPE..NEG_OFF_TYPE + 2].try_into().unwrap());
    if magic != NEGOTIATION_MAGIC || version != NEGOTIATION_VERSION || ty != expected_ty {
        return Err(protocol_error("invalid negotiation frame"));
    }
    Ok(())
}

fn decode_hello_neg(frame: &Frame) -> io::Result<HelloPayload> {
    decode_neg_header(frame, NEGOTIATION_HELLO)?;
    decode_hello_payload(
        &frame[NEGOTIATION_PAYLOAD_OFFSET..NEGOTIATION_PAYLOAD_OFFSET + CONTROL_HELLO_PAYLOAD_LEN],
    )
}

fn decode_ack_neg(frame: &Frame) -> io::Result<(HelloAckPayload, u32)> {
    decode_neg_header(frame, NEGOTIATION_ACK)?;
    let payload = decode_hello_ack_payload(
        &frame[NEGOTIATION_PAYLOAD_OFFSET
            ..NEGOTIATION_PAYLOAD_OFFSET + CONTROL_HELLO_ACK_PAYLOAD_LEN],
    )?;
    let status = u32::from_le_bytes(
        frame[NEGOTIATION_STATUS_OFFSET..NEGOTIATION_STATUS_OFFSET + 4]
            .try_into()
            .unwrap(),
    );
    Ok((payload, status))
}

// ---------------------------------------------------------------------------
// Pipe I/O helpers
// ---------------------------------------------------------------------------

fn wait_pipe_message(pipe: HANDLE, timeout_ms: u32) -> io::Result<DWORD> {
    let deadline = if timeout_ms == 0 {
        0u64
    } else {
        now_ms() + timeout_ms as u64
    };

    loop {
        let mut avail: DWORD = 0;
        let mut left: DWORD = 0;
        let ok = unsafe {
            PeekNamedPipe(
                pipe,
                ptr::null_mut(),
                0,
                ptr::null_mut(),
                &mut avail,
                &mut left,
            )
        };
        if ok == 0 {
            let code = unsafe { GetLastError() };
            if code == ERROR_BROKEN_PIPE
                || code == ERROR_NO_DATA
                || code == ERROR_PIPE_NOT_CONNECTED
            {
                return Err(io::Error::from_raw_os_error(code as i32));
            }
            return Err(io::Error::new(
                io::ErrorKind::Other,
                format!("PeekNamedPipe: win32 error {code}"),
            ));
        }
        if left != 0 || avail != 0 {
            return Ok(if left != 0 { left } else { avail });
        }
        if deadline != 0 && now_ms() >= deadline {
            return Err(io::Error::new(io::ErrorKind::TimedOut, "pipe read timeout"));
        }
        unsafe {
            Sleep(1);
        }
    }
}

fn drain_pipe_message(pipe: HANDLE) -> io::Result<()> {
    let mut scratch = [0u8; 256];
    loop {
        let mut bytes_read: DWORD = 0;
        let ok = unsafe {
            ReadFile(
                pipe,
                scratch.as_mut_ptr() as LPVOID,
                scratch.len() as DWORD,
                &mut bytes_read,
                ptr::null_mut(),
            )
        };
        if ok != 0 {
            return Ok(());
        }

        let error = unsafe { GetLastError() };
        if error == 234 {
            continue;
        }
        return Err(win32_error("ReadFile"));
    }
}

fn pipe_read_message(pipe: HANDLE, message: &mut [u8], timeout_ms: u32) -> io::Result<usize> {
    if message.is_empty() {
        return Err(protocol_error("buffer must not be empty"));
    }

    let message_len = wait_pipe_message(pipe, timeout_ms)? as usize;
    if message_len > message.len() {
        let _ = drain_pipe_message(pipe);
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "message exceeds negotiated size",
        ));
    }

    let mut bytes_read: DWORD = 0;
    let ok = unsafe {
        ReadFile(
            pipe,
            message.as_mut_ptr() as LPVOID,
            message_len as DWORD,
            &mut bytes_read,
            ptr::null_mut(),
        )
    };
    if ok == 0 {
        let error = unsafe { GetLastError() };
        if error == 234 {
            let _ = drain_pipe_message(pipe);
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "message exceeds negotiated size",
            ));
        }
        return Err(win32_error("ReadFile"));
    }
    if bytes_read as usize != message_len {
        return Err(protocol_error("short pipe read"));
    }
    Ok(message_len)
}

fn pipe_write_message(pipe: HANDLE, message: &[u8]) -> io::Result<()> {
    if message.is_empty() {
        return Err(protocol_error("message must not be empty"));
    }

    let mut written: DWORD = 0;
    let ok = unsafe {
        WriteFile(
            pipe,
            message.as_ptr(),
            message.len() as DWORD,
            &mut written,
            ptr::null_mut(),
        )
    };
    if ok == 0 {
        return Err(win32_error("WriteFile"));
    }
    if written as usize != message.len() {
        return Err(protocol_error("short pipe write"));
    }
    Ok(())
}

fn validate_message_for_send(message: &[u8], max_message_len: usize) -> io::Result<()> {
    if message.is_empty() {
        return Err(protocol_error("message must not be empty"));
    }
    if message.len() > max_message_len {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "message exceeds negotiated size",
        ));
    }
    let header = decode_message_header(message)?;
    let total = message_total_size(&header)?;
    if total != message.len() {
        return Err(protocol_error("message size does not match header"));
    }
    Ok(())
}

fn validate_received_message(
    message: &[u8],
    message_len: usize,
    max_message_len: usize,
) -> io::Result<()> {
    if message_len == 0 {
        return Err(protocol_error("message must not be empty"));
    }
    if message_len > max_message_len {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "message exceeds negotiated size",
        ));
    }
    let header = decode_message_header(&message[..message_len])?;
    let total = message_total_size(&header)?;
    if total != message_len {
        return Err(protocol_error("message size does not match header"));
    }
    Ok(())
}

fn compute_pipe_packet_size(
    max_request_message_len: usize,
    max_response_message_len: usize,
) -> io::Result<u32> {
    let logical_limit = max_request_message_len.max(max_response_message_len);
    if logical_limit <= CHUNK_HEADER_LEN || logical_limit > u32::MAX as usize {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "invalid named pipe packet size",
        ));
    }
    Ok(logical_limit as u32)
}

fn compute_chunk_payload_budget(packet_size: u32) -> io::Result<usize> {
    if packet_size as usize <= CHUNK_HEADER_LEN {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "packet size is too small for chunking",
        ));
    }
    Ok(packet_size as usize - CHUNK_HEADER_LEN)
}

fn send_chunked_message(pipe: HANDLE, message: &[u8], packet_size: u32) -> io::Result<()> {
    let chunk_payload_budget = compute_chunk_payload_budget(packet_size)?;
    let header = decode_message_header(message)?;
    let chunk_count = message.len().div_ceil(chunk_payload_budget) as u32;
    let mut packet = vec![0u8; packet_size as usize];
    let mut offset = 0usize;

    for chunk_index in 0..chunk_count {
        let remaining = message.len() - offset;
        let chunk_payload_len = remaining.min(chunk_payload_budget);
        let chunk_header = encode_chunk_header(&ChunkHeader {
            magic: CHUNK_MAGIC,
            version: CHUNK_VERSION,
            flags: 0,
            message_id: header.message_id,
            total_message_len: message.len() as u32,
            chunk_index,
            chunk_count,
            chunk_payload_len: chunk_payload_len as u32,
        })?;
        packet[..CHUNK_HEADER_LEN].copy_from_slice(&chunk_header);
        packet[CHUNK_HEADER_LEN..CHUNK_HEADER_LEN + chunk_payload_len]
            .copy_from_slice(&message[offset..offset + chunk_payload_len]);
        pipe_write_message(pipe, &packet[..CHUNK_HEADER_LEN + chunk_payload_len])?;
        offset += chunk_payload_len;
    }

    Ok(())
}

fn recv_transport_message(
    pipe: HANDLE,
    message: &mut [u8],
    max_message_len: usize,
    packet_size: u32,
    timeout_ms: u32,
) -> io::Result<usize> {
    let packet_capacity = if packet_size != 0 {
        packet_size as usize
    } else {
        max_message_len
    };
    if packet_capacity == 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "packet capacity must not be zero",
        ));
    }

    let mut packet = vec![0u8; packet_capacity];
    let packet_len = pipe_read_message(pipe, &mut packet, timeout_ms)?;
    if packet_len >= MESSAGE_HEADER_LEN {
        if let Ok(header) = decode_message_header(&packet[..packet_len]) {
            let total = message_total_size(&header)?;
            if total == packet_len {
                if packet_len > message.len() {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        "message buffer is smaller than received message",
                    ));
                }
                message[..packet_len].copy_from_slice(&packet[..packet_len]);
                validate_received_message(message, packet_len, max_message_len)?;
                return Ok(packet_len);
            }
        }
    }

    if packet_len < CHUNK_HEADER_LEN {
        return Err(protocol_error("invalid chunk packet length"));
    }

    let first_chunk = decode_chunk_header(&packet[..CHUNK_HEADER_LEN])?;
    if first_chunk.chunk_index != 0 || first_chunk.chunk_count < 2 {
        return Err(protocol_error("invalid first chunk header"));
    }
    if first_chunk.total_message_len as usize > message.len()
        || first_chunk.total_message_len as usize > max_message_len
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "chunked message exceeds negotiated size",
        ));
    }

    let mut payload_len = packet_len - CHUNK_HEADER_LEN;
    if payload_len != first_chunk.chunk_payload_len as usize {
        return Err(protocol_error("first chunk payload length mismatch"));
    }

    message[..payload_len].copy_from_slice(&packet[CHUNK_HEADER_LEN..packet_len]);
    let mut offset = payload_len;
    for expected_index in 1..first_chunk.chunk_count {
        let packet_len = pipe_read_message(pipe, &mut packet, timeout_ms)?;
        if packet_len < CHUNK_HEADER_LEN {
            return Err(protocol_error("invalid continuation chunk length"));
        }

        let chunk = decode_chunk_header(&packet[..CHUNK_HEADER_LEN])?;
        payload_len = packet_len - CHUNK_HEADER_LEN;
        if chunk.message_id != first_chunk.message_id
            || chunk.total_message_len != first_chunk.total_message_len
            || chunk.chunk_count != first_chunk.chunk_count
            || chunk.chunk_index != expected_index
            || payload_len != chunk.chunk_payload_len as usize
            || offset + payload_len > first_chunk.total_message_len as usize
        {
            return Err(protocol_error("invalid continuation chunk"));
        }

        message[offset..offset + payload_len]
            .copy_from_slice(&packet[CHUNK_HEADER_LEN..packet_len]);
        offset += payload_len;
    }

    if offset != first_chunk.total_message_len as usize {
        return Err(protocol_error("chunked message length mismatch"));
    }
    validate_received_message(message, offset, max_message_len)?;
    Ok(offset)
}

fn send_transport_message(
    pipe: HANDLE,
    message: &[u8],
    max_message_len: usize,
    packet_size: u32,
) -> io::Result<()> {
    validate_message_for_send(message, max_message_len)?;
    if packet_size != 0 && message.len() > packet_size as usize {
        return send_chunked_message(pipe, message, packet_size);
    }
    pipe_write_message(pipe, message)
}

fn pipe_read_frame(pipe: HANDLE, timeout_ms: u32) -> io::Result<Frame> {
    let mut buf = vec![0u8; FRAME_SIZE];
    let message_len = pipe_read_message(pipe, &mut buf, timeout_ms)?;
    if message_len != FRAME_SIZE {
        return Err(protocol_error("received non-frame message on frame path"));
    }
    let mut frame = [0u8; FRAME_SIZE];
    frame.copy_from_slice(&buf[..message_len]);
    Ok(frame)
}

fn pipe_write_frame(pipe: HANDLE, frame: &Frame) -> io::Result<()> {
    pipe_write_message(pipe, frame)
}

fn set_pipe_mode(pipe: HANDLE, wait_mode: DWORD) -> io::Result<()> {
    let mode = PIPE_READMODE_MESSAGE | wait_mode;
    let ok = unsafe { SetNamedPipeHandleState(pipe, &mode, ptr::null_mut(), ptr::null_mut()) };
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
    listener: &NamedPipeListener,
    pipe: HANDLE,
    timeout_ms: u32,
) -> io::Result<NegotiationResult> {
    let hello_frame = pipe_read_frame(pipe, timeout_ms)?;
    let hello = decode_hello_neg(&hello_frame)?;

    let local_max_request_message_len = compute_max_message_len(
        listener.max_request_payload_bytes,
        listener.max_request_batch_items,
    )?;
    let local_max_response_message_len = compute_max_message_len(
        listener.max_response_payload_bytes,
        listener.max_response_batch_items,
    )?;
    let local_packet_size = compute_pipe_packet_size(
        local_max_request_message_len,
        local_max_response_message_len,
    )?;

    let mut ack = HelloAckPayload {
        layout_version: hello.layout_version,
        flags: 0,
        server_supported_profiles: listener.supported_profiles,
        intersection_profiles: hello.supported_profiles & listener.supported_profiles,
        selected_profile: 0,
        agreed_max_request_payload_bytes: negotiate_limit_u32(
            hello.max_request_payload_bytes,
            listener.max_request_payload_bytes,
        ),
        agreed_max_request_batch_items: negotiate_limit_u32(
            hello.max_request_batch_items,
            listener.max_request_batch_items,
        ),
        agreed_max_response_payload_bytes: negotiate_limit_u32(
            hello.max_response_payload_bytes,
            listener.max_response_payload_bytes,
        ),
        agreed_max_response_batch_items: negotiate_limit_u32(
            hello.max_response_batch_items,
            listener.max_response_batch_items,
        ),
        agreed_packet_size: negotiate_limit_u32(hello.packet_size, local_packet_size),
    };
    let mut status = NEGOTIATION_STATUS_OK;

    if listener.auth_token != 0 && hello.auth_token != listener.auth_token {
        status = ERROR_ACCESS_DENIED;
    } else {
        let mut candidates = ack.intersection_profiles & listener.preferred_profiles;
        if candidates == 0 {
            candidates = ack.intersection_profiles;
        }
        ack.selected_profile = select_profile(candidates);
        if ack.selected_profile == 0 {
            status = ERROR_NOT_SUPPORTED;
        } else if ack.agreed_max_request_payload_bytes == 0
            || ack.agreed_max_request_batch_items == 0
            || ack.agreed_max_response_payload_bytes == 0
            || ack.agreed_max_response_batch_items == 0
            || ack.agreed_packet_size == 0
        {
            status = ERROR_INVALID_PARAMETER;
        }
    }

    let ack_frame = encode_ack_neg(&ack, status);
    pipe_write_frame(pipe, &ack_frame)?;

    if status != NEGOTIATION_STATUS_OK {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            format!("negotiation failed: status {}", status),
        ));
    }
    let max_request_message_len = compute_max_message_len(
        ack.agreed_max_request_payload_bytes,
        ack.agreed_max_request_batch_items,
    )?;
    let max_response_message_len = compute_max_message_len(
        ack.agreed_max_response_payload_bytes,
        ack.agreed_max_response_batch_items,
    )?;
    if ack.agreed_packet_size as usize > max_request_message_len.max(max_response_message_len) {
        return Err(protocol_error("invalid negotiated packet size"));
    }
    Ok(NegotiationResult {
        profile: ack.selected_profile,
        packet_size: ack.agreed_packet_size,
        agreed_max_request_payload_bytes: ack.agreed_max_request_payload_bytes,
        agreed_max_request_batch_items: ack.agreed_max_request_batch_items,
        agreed_max_response_payload_bytes: ack.agreed_max_response_payload_bytes,
        agreed_max_response_batch_items: ack.agreed_max_response_batch_items,
        max_request_message_len,
        max_response_message_len,
    })
}

fn client_handshake(
    pipe: HANDLE,
    supported_profiles: u32,
    preferred_profiles: u32,
    max_request_payload_bytes: u32,
    max_request_batch_items: u32,
    max_response_payload_bytes: u32,
    max_response_batch_items: u32,
    auth_token: u64,
    timeout_ms: u32,
) -> io::Result<NegotiationResult> {
    let local_max_request_message_len =
        compute_max_message_len(max_request_payload_bytes, max_request_batch_items)?;
    let local_max_response_message_len =
        compute_max_message_len(max_response_payload_bytes, max_response_batch_items)?;
    let local_packet_size = compute_pipe_packet_size(
        local_max_request_message_len,
        local_max_response_message_len,
    )?;

    let hello = HelloPayload {
        layout_version: MESSAGE_VERSION,
        flags: 0,
        supported_profiles,
        preferred_profiles,
        max_request_payload_bytes,
        max_request_batch_items,
        max_response_payload_bytes,
        max_response_batch_items,
        auth_token,
        packet_size: local_packet_size,
    };
    pipe_write_frame(pipe, &encode_hello_neg(&hello))?;
    let ack_frame = pipe_read_frame(pipe, timeout_ms)?;
    let (ack, status) = decode_ack_neg(&ack_frame)?;

    if status != NEGOTIATION_STATUS_OK {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            format!("server rejected: status {}", status),
        ));
    }
    if ack.selected_profile == 0
        || (ack.selected_profile & supported_profiles) == 0
        || (ack.intersection_profiles & supported_profiles) == 0
        || ack.agreed_max_request_payload_bytes == 0
        || ack.agreed_max_request_batch_items == 0
        || ack.agreed_max_response_payload_bytes == 0
        || ack.agreed_max_response_batch_items == 0
        || ack.agreed_packet_size == 0
        || ack.agreed_packet_size > local_packet_size
    {
        return Err(protocol_error("invalid negotiated profile"));
    }
    let max_request_message_len = compute_max_message_len(
        ack.agreed_max_request_payload_bytes,
        ack.agreed_max_request_batch_items,
    )?;
    let max_response_message_len = compute_max_message_len(
        ack.agreed_max_response_payload_bytes,
        ack.agreed_max_response_batch_items,
    )?;
    if ack.agreed_packet_size as usize > max_request_message_len.max(max_response_message_len) {
        return Err(protocol_error("invalid negotiated packet size"));
    }
    Ok(NegotiationResult {
        profile: ack.selected_profile,
        packet_size: ack.agreed_packet_size,
        agreed_max_request_payload_bytes: ack.agreed_max_request_payload_bytes,
        agreed_max_request_batch_items: ack.agreed_max_request_batch_items,
        agreed_max_response_payload_bytes: ack.agreed_max_response_payload_bytes,
        agreed_max_response_batch_items: ack.agreed_max_response_batch_items,
        max_request_message_len,
        max_response_message_len,
    })
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
    mapping_len: usize,
    last_request_seq: i64,
    active_request_seq: i64,
    spin_tries: u32,
    max_request_message_len: usize,
    max_response_message_len: usize,
}

impl ShmServer {
    fn create(config: &NamedPipeConfig, profile: u32) -> io::Result<Self> {
        let mapping_name = to_wide(&build_kernel_object_name(config, profile, "shm"));
        let req_event_name = to_wide(&build_kernel_object_name(config, profile, "req"));
        let resp_event_name = to_wide(&build_kernel_object_name(config, profile, "resp"));
        let max_request_message_len = compute_max_message_len(
            effective_payload_limit(config.max_request_payload_bytes),
            effective_batch_limit(config.max_request_batch_items),
        )?;
        let max_response_message_len = compute_max_message_len(
            effective_payload_limit(config.max_response_payload_bytes),
            effective_batch_limit(config.max_response_batch_items),
        )?;
        let (request_offset, response_offset, mapping_len) =
            compute_region_layout(max_request_message_len, max_response_message_len)?;

        let mapping = unsafe {
            CreateFileMappingW(
                INVALID_HANDLE_VALUE,
                ptr::null_mut(),
                PAGE_READWRITE,
                (mapping_len as u64 >> 32) as DWORD,
                (mapping_len as u64 & 0xffff_ffff) as DWORD,
                mapping_name.as_ptr(),
            )
        };
        if mapping.is_null() {
            return Err(win32_error("CreateFileMappingW"));
        }
        if unsafe { GetLastError() } == ERROR_ALREADY_EXISTS {
            close_handle(mapping);
            return Err(io::Error::new(
                io::ErrorKind::AlreadyExists,
                "SHM already exists",
            ));
        }

        let region =
            unsafe { MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, mapping_len) } as *mut u8;
        if region.is_null() {
            close_handle(mapping);
            return Err(win32_error("MapViewOfFile"));
        }

        // Zero and init header
        unsafe {
            ptr::write_bytes(region, 0, mapping_len);
            ptr::copy_nonoverlapping(
                SHM_REGION_MAGIC.to_le_bytes().as_ptr(),
                region.add(OFF_HDR_MAGIC),
                4,
            );
            ptr::copy_nonoverlapping(
                SHM_REGION_VERSION.to_le_bytes().as_ptr(),
                region.add(OFF_HDR_VERSION),
                4,
            );
            ptr::copy_nonoverlapping(
                (HDR_SIZE as u32).to_le_bytes().as_ptr(),
                region.add(OFF_HDR_HEADER_LEN),
                4,
            );
            ptr::copy_nonoverlapping(
                profile.to_le_bytes().as_ptr(),
                region.add(OFF_HDR_PROFILE),
                4,
            );
            ptr::copy_nonoverlapping(
                (request_offset as u32).to_le_bytes().as_ptr(),
                region.add(OFF_HDR_REQ_OFFSET),
                4,
            );
            ptr::copy_nonoverlapping(
                (max_request_message_len as u32).to_le_bytes().as_ptr(),
                region.add(OFF_HDR_REQ_CAPACITY),
                4,
            );
            ptr::copy_nonoverlapping(
                (response_offset as u32).to_le_bytes().as_ptr(),
                region.add(OFF_HDR_RESP_OFFSET),
                4,
            );
            ptr::copy_nonoverlapping(
                (max_response_message_len as u32).to_le_bytes().as_ptr(),
                region.add(OFF_HDR_RESP_CAPACITY),
                4,
            );
            let spin = effective_spin_tries(config);
            ptr::copy_nonoverlapping(
                spin.to_le_bytes().as_ptr(),
                region.add(OFF_HDR_SPIN_TRIES),
                4,
            );
        }

        // Create events (auto-reset)
        let request_event =
            unsafe { CreateEventW(ptr::null_mut(), FALSE, FALSE, req_event_name.as_ptr()) };
        if request_event.is_null() {
            unsafe {
                UnmapViewOfFile(region as LPVOID);
            }
            close_handle(mapping);
            return Err(win32_error("CreateEventW(req)"));
        }
        let response_event =
            unsafe { CreateEventW(ptr::null_mut(), FALSE, FALSE, resp_event_name.as_ptr()) };
        if response_event.is_null() {
            close_handle(request_event);
            unsafe {
                UnmapViewOfFile(region as LPVOID);
            }
            close_handle(mapping);
            return Err(win32_error("CreateEventW(resp)"));
        }

        Ok(ShmServer {
            mapping,
            request_event,
            response_event,
            region,
            mapping_len,
            last_request_seq: 0,
            active_request_seq: 0,
            spin_tries: effective_spin_tries(config),
            max_request_message_len,
            max_response_message_len,
        })
    }

    fn receive_bytes(
        &mut self,
        message: &mut [u8],
        message_capacity: usize,
        timeout_ms: u32,
    ) -> io::Result<usize> {
        if message_capacity > message.len() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "message buffer is smaller than requested capacity",
            ));
        }
        if message_capacity == 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "message capacity must not be zero",
            ));
        }
        if message_capacity > self.max_request_message_len {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "message capacity exceeds negotiated request size",
            ));
        }

        if message.len() < message_capacity {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "message buffer is smaller than negotiated request size",
            ));
        }

        let deadline = if timeout_ms == 0 {
            0u64
        } else {
            now_ms() + timeout_ms as u64
        };
        let mut spins = self.spin_tries;

        loop {
            let current = unsafe { load_i64_acquire(self.region.add(OFF_REQ_SEQ) as *const i64) };
            if current != self.last_request_seq {
                let message_len =
                    unsafe { load_i32_acquire(self.region.add(OFF_REQ_LEN) as *const i32) };
                if message_len < 0
                    || message_len as usize > message_capacity
                    || message_len as usize > request_capacity(self.region)
                {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "invalid SHM request length",
                    ));
                }
                unsafe {
                    ptr::copy_nonoverlapping(
                        self.region.add(request_area(self.region)),
                        message.as_mut_ptr(),
                        message_len as usize,
                    );
                }
                self.active_request_seq = current;
                self.last_request_seq = current;
                return Ok(message_len as usize);
            }

            if unsafe { load_i32_acquire(self.region.add(OFF_REQ_CLIENT_CLOSED) as *const i32) }
                != 0
            {
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
            unsafe {
                store_i32_release(self.region.add(OFF_REQ_SERVER_WAITING) as *mut i32, 1);
            }
            fence(Ordering::SeqCst);

            let current = unsafe { load_i64_acquire(self.region.add(OFF_REQ_SEQ) as *const i64) };
            if current != self.last_request_seq {
                unsafe {
                    store_i32_release(self.region.add(OFF_REQ_SERVER_WAITING) as *mut i32, 0);
                }
                let message_len =
                    unsafe { load_i32_acquire(self.region.add(OFF_REQ_LEN) as *const i32) };
                if message_len < 0
                    || message_len as usize > message_capacity
                    || message_len as usize > request_capacity(self.region)
                {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "invalid SHM request length",
                    ));
                }
                unsafe {
                    ptr::copy_nonoverlapping(
                        self.region.add(request_area(self.region)),
                        message.as_mut_ptr(),
                        message_len as usize,
                    );
                }
                self.active_request_seq = current;
                self.last_request_seq = current;
                return Ok(message_len as usize);
            }

            let wait_ms = if deadline == 0 {
                INFINITE
            } else {
                let now = now_ms();
                if now >= deadline {
                    unsafe {
                        store_i32_release(self.region.add(OFF_REQ_SERVER_WAITING) as *mut i32, 0);
                    }
                    return Err(io::Error::new(
                        io::ErrorKind::TimedOut,
                        "SHM receive timeout",
                    ));
                }
                (deadline - now) as DWORD
            };

            let rc = unsafe { WaitForSingleObject(self.request_event, wait_ms) };
            unsafe {
                store_i32_release(self.region.add(OFF_REQ_SERVER_WAITING) as *mut i32, 0);
            }

            if rc == WAIT_OBJECT_0 {
                spins = self.spin_tries;
                continue;
            }
            if rc == WAIT_TIMEOUT {
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "SHM receive timeout",
                ));
            }
            return Err(win32_error("WaitForSingleObject(req)"));
        }
    }

    fn receive_message(&mut self, message: &mut [u8], timeout_ms: u32) -> io::Result<usize> {
        let message_len = self.receive_bytes(message, self.max_request_message_len, timeout_ms)?;
        validate_received_message(message, message_len, self.max_request_message_len)?;
        Ok(message_len)
    }

    fn send_bytes(&mut self, message: &[u8], message_capacity: usize) -> io::Result<()> {
        if self.active_request_seq == 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "no active request",
            ));
        }
        if message_capacity == 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "message capacity must not be zero",
            ));
        }
        if message_capacity > self.max_response_message_len {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "message capacity exceeds negotiated response size",
            ));
        }
        if message.is_empty() {
            return Err(protocol_error("message must not be empty"));
        }
        if message.len() > message_capacity {
            return Err(protocol_error("message exceeds negotiated size"));
        }

        unsafe {
            ptr::copy_nonoverlapping(
                message.as_ptr(),
                self.region.add(response_area(self.region)),
                message.len(),
            );
            store_i32_release(
                self.region.add(OFF_RESP_LEN) as *mut i32,
                message.len() as i32,
            );
            store_i64_release(
                self.region.add(OFF_RESP_SEQ) as *mut i64,
                self.active_request_seq,
            );

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

    fn send_message(&mut self, message: &[u8]) -> io::Result<()> {
        validate_message_for_send(message, self.max_response_message_len)?;
        self.send_bytes(message, self.max_response_message_len)
    }

    fn receive_frame(&mut self, timeout_ms: u32) -> io::Result<Frame> {
        if self.max_request_message_len < FRAME_SIZE {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "negotiated request size is smaller than frame size",
            ));
        }
        let mut message = [0u8; FRAME_SIZE];
        let message_len = self.receive_bytes(&mut message, FRAME_SIZE, timeout_ms)?;
        if message_len != FRAME_SIZE {
            return Err(protocol_error("invalid SHM frame length"));
        }
        Ok(message)
    }

    fn send_frame(&mut self, frame: &Frame) -> io::Result<()> {
        if self.max_response_message_len < FRAME_SIZE {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "negotiated response size is smaller than frame size",
            ));
        }
        self.send_bytes(frame, FRAME_SIZE)
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
                unsafe {
                    SetEvent(self.request_event);
                }
            }
            if self.response_event != INVALID_HANDLE_VALUE && !self.response_event.is_null() {
                unsafe {
                    SetEvent(self.response_event);
                }
            }
            unsafe {
                UnmapViewOfFile(self.region as LPVOID);
            }
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
    mapping_len: usize,
    next_request_seq: i64,
    spin_tries: u32,
    max_request_message_len: usize,
    max_response_message_len: usize,
}

impl ShmClient {
    fn create(config: &NamedPipeConfig, profile: u32, timeout_ms: u32) -> io::Result<Self> {
        let mapping_name = to_wide(&build_kernel_object_name(config, profile, "shm"));
        let req_event_name = to_wide(&build_kernel_object_name(config, profile, "req"));
        let resp_event_name = to_wide(&build_kernel_object_name(config, profile, "resp"));
        let max_request_message_len = compute_max_message_len(
            effective_payload_limit(config.max_request_payload_bytes),
            effective_batch_limit(config.max_request_batch_items),
        )?;
        let max_response_message_len = compute_max_message_len(
            effective_payload_limit(config.max_response_payload_bytes),
            effective_batch_limit(config.max_response_batch_items),
        )?;
        let (request_offset, response_offset, mapping_len) =
            compute_region_layout(max_request_message_len, max_response_message_len)?;

        let deadline = if timeout_ms == 0 {
            0u64
        } else {
            now_ms() + timeout_ms as u64
        };

        let (mapping, request_event, response_event) = loop {
            let m = unsafe { OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping_name.as_ptr()) };
            if !m.is_null() {
                let re = unsafe {
                    OpenEventW(
                        SYNCHRONIZE | EVENT_MODIFY_STATE,
                        FALSE,
                        req_event_name.as_ptr(),
                    )
                };
                let rsp = unsafe {
                    OpenEventW(
                        SYNCHRONIZE | EVENT_MODIFY_STATE,
                        FALSE,
                        resp_event_name.as_ptr(),
                    )
                };
                if !re.is_null() && !rsp.is_null() {
                    break (m, re, rsp);
                }
                close_handle(rsp);
                close_handle(re);
                close_handle(m);
            }
            if deadline != 0 && now_ms() >= deadline {
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "SHM connect timeout",
                ));
            }
            unsafe {
                Sleep(1);
            }
        };

        let region =
            unsafe { MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, mapping_len) } as *mut u8;
        if region.is_null() {
            close_handle(response_event);
            close_handle(request_event);
            close_handle(mapping);
            return Err(win32_error("MapViewOfFile(client)"));
        }

        let ready_deadline = if timeout_ms == 0 {
            0u64
        } else {
            now_ms() + timeout_ms as u64
        };
        let region_spin = loop {
            match validate_region_header(region, mapping_len, profile) {
                Ok(spin) => break spin,
                Err(_) if ready_deadline != 0 && now_ms() >= ready_deadline => {
                    unsafe {
                        UnmapViewOfFile(region as LPVOID);
                    }
                    close_handle(response_event);
                    close_handle(request_event);
                    close_handle(mapping);
                    return Err(io::Error::new(
                        io::ErrorKind::TimedOut,
                        "SHM region not ready",
                    ));
                }
                Err(_) => unsafe {
                    Sleep(1);
                },
            }
        };
        if request_area(region) != request_offset
            || response_area(region) != response_offset
            || request_capacity(region) < max_request_message_len
            || response_capacity(region) < max_response_message_len
        {
            unsafe {
                UnmapViewOfFile(region as LPVOID);
            }
            close_handle(response_event);
            close_handle(request_event);
            close_handle(mapping);
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "SHM region capacity mismatch",
            ));
        }
        let spin_tries = if region_spin != 0 {
            region_spin
        } else {
            effective_spin_tries(config)
        };

        let next_req_seq = unsafe { load_i64_acquire(region.add(OFF_REQ_SEQ) as *const i64) };

        Ok(ShmClient {
            mapping,
            request_event,
            response_event,
            region,
            mapping_len,
            next_request_seq: next_req_seq,
            spin_tries,
            max_request_message_len,
            max_response_message_len,
        })
    }

    fn call_bytes(
        &mut self,
        request: &[u8],
        response: &mut [u8],
        request_capacity: usize,
        response_capacity_limit: usize,
        timeout_ms: u32,
    ) -> io::Result<usize> {
        if request_capacity == 0 || response_capacity_limit == 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "message capacity must not be zero",
            ));
        }
        if request_capacity > self.max_request_message_len
            || response_capacity_limit > self.max_response_message_len
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "message capacity exceeds negotiated limits",
            ));
        }
        if request.is_empty() {
            return Err(protocol_error("message must not be empty"));
        }
        if request.len() > request_capacity {
            return Err(protocol_error("message exceeds negotiated size"));
        }
        if response.len() < response_capacity_limit {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "response buffer is smaller than negotiated response size",
            ));
        }

        let request_seq = self.next_request_seq + 1;
        self.next_request_seq = request_seq;

        unsafe {
            ptr::copy_nonoverlapping(
                request.as_ptr(),
                self.region.add(request_area(self.region)),
                request.len(),
            );
            store_i32_release(
                self.region.add(OFF_REQ_LEN) as *mut i32,
                request.len() as i32,
            );
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
        let deadline = if timeout_ms == 0 {
            0u64
        } else {
            now_ms() + timeout_ms as u64
        };
        let mut spins = self.spin_tries;

        loop {
            let current = unsafe { load_i64_acquire(self.region.add(OFF_RESP_SEQ) as *const i64) };
            if current >= request_seq {
                let response_len =
                    unsafe { load_i32_acquire(self.region.add(OFF_RESP_LEN) as *const i32) };
                if response_len < 0
                    || response_len as usize > response_capacity_limit
                    || response_len as usize > response_capacity(self.region)
                {
                    return Err(protocol_error("invalid SHM response length"));
                }
                unsafe {
                    ptr::copy_nonoverlapping(
                        self.region.add(response_area(self.region)),
                        response.as_mut_ptr(),
                        response_len as usize,
                    );
                }
                return Ok(response_len as usize);
            }

            if unsafe { load_i32_acquire(self.region.add(OFF_RESP_SERVER_CLOSED) as *const i32) }
                != 0
            {
                return Err(io::Error::new(io::ErrorKind::BrokenPipe, "server closed"));
            }

            std::hint::spin_loop();

            if spins != 0 {
                spins -= 1;
                continue;
            }

            // Mark waiting + SeqCst fence (same Dekker race prevention)
            unsafe {
                store_i32_release(self.region.add(OFF_RESP_CLIENT_WAITING) as *mut i32, 1);
            }
            fence(Ordering::SeqCst);

            let current = unsafe { load_i64_acquire(self.region.add(OFF_RESP_SEQ) as *const i64) };
            if current >= request_seq {
                unsafe {
                    store_i32_release(self.region.add(OFF_RESP_CLIENT_WAITING) as *mut i32, 0);
                }
                let response_len =
                    unsafe { load_i32_acquire(self.region.add(OFF_RESP_LEN) as *const i32) };
                if response_len < 0
                    || response_len as usize > response_capacity_limit
                    || response_len as usize > response_capacity(self.region)
                {
                    return Err(protocol_error("invalid SHM response length"));
                }
                unsafe {
                    ptr::copy_nonoverlapping(
                        self.region.add(response_area(self.region)),
                        response.as_mut_ptr(),
                        response_len as usize,
                    );
                }
                return Ok(response_len as usize);
            }

            let wait_ms = if deadline == 0 {
                INFINITE
            } else {
                let now = now_ms();
                if now >= deadline {
                    unsafe {
                        store_i32_release(self.region.add(OFF_RESP_CLIENT_WAITING) as *mut i32, 0);
                    }
                    return Err(io::Error::new(
                        io::ErrorKind::TimedOut,
                        "SHM response timeout",
                    ));
                }
                (deadline - now) as DWORD
            };

            let rc = unsafe { WaitForSingleObject(self.response_event, wait_ms) };
            unsafe {
                store_i32_release(self.region.add(OFF_RESP_CLIENT_WAITING) as *mut i32, 0);
            }

            if rc == WAIT_OBJECT_0 {
                spins = self.spin_tries;
                continue;
            }
            if rc == WAIT_TIMEOUT {
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "SHM response timeout",
                ));
            }
            return Err(win32_error("WaitForSingleObject(resp)"));
        }
    }

    fn call_message(
        &mut self,
        request: &[u8],
        response: &mut [u8],
        timeout_ms: u32,
    ) -> io::Result<usize> {
        validate_message_for_send(request, self.max_request_message_len)?;
        let response_len = self.call_bytes(
            request,
            response,
            self.max_request_message_len,
            self.max_response_message_len,
            timeout_ms,
        )?;
        validate_received_message(response, response_len, self.max_response_message_len)?;
        Ok(response_len)
    }

    fn call_frame(&mut self, request: &Frame, timeout_ms: u32) -> io::Result<Frame> {
        if self.max_request_message_len < FRAME_SIZE || self.max_response_message_len < FRAME_SIZE {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "negotiated frame size exceeds SHM capacities",
            ));
        }
        let mut response = [0u8; FRAME_SIZE];
        let response_len =
            self.call_bytes(request, &mut response, FRAME_SIZE, FRAME_SIZE, timeout_ms)?;
        if response_len != FRAME_SIZE {
            return Err(protocol_error("invalid SHM frame length"));
        }
        Ok(response)
    }
}

impl Drop for ShmClient {
    fn drop(&mut self) {
        if !self.region.is_null() {
            unsafe {
                store_i32_release(self.region.add(OFF_REQ_CLIENT_CLOSED) as *mut i32, 1);
            }
            if self.request_event != INVALID_HANDLE_VALUE && !self.request_event.is_null() {
                unsafe {
                    SetEvent(self.request_event);
                }
            }
            if self.response_event != INVALID_HANDLE_VALUE && !self.response_event.is_null() {
                unsafe {
                    SetEvent(self.response_event);
                }
            }
            unsafe {
                UnmapViewOfFile(self.region as LPVOID);
            }
        }
        close_handle(self.response_event);
        close_handle(self.request_event);
        close_handle(self.mapping);
    }
}

// ---------------------------------------------------------------------------
// Public API: NamedPipeServer
// ---------------------------------------------------------------------------

pub struct NamedPipeListener {
    pipe: HANDLE,
    run_dir: String,
    service_name: String,
    supported_profiles: u32,
    preferred_profiles: u32,
    max_request_payload_bytes: u32,
    max_request_batch_items: u32,
    max_response_payload_bytes: u32,
    max_response_batch_items: u32,
    auth_token: u64,
    shm_spin_tries: u32,
}

pub struct NamedPipeSession {
    pipe: HANDLE,
    negotiated_profile: u32,
    packet_size: u32,
    max_request_message_len: usize,
    max_response_message_len: usize,
    shm_server: Option<ShmServer>,
    connected: bool,
}

pub struct NamedPipeServer {
    listener: NamedPipeListener,
    session: Option<NamedPipeSession>,
}

impl NamedPipeListener {
    pub fn bind(config: &NamedPipeConfig) -> io::Result<Self> {
        let pipe_name = to_wide(&build_pipe_name(config));
        let supported = effective_supported(config);
        let preferred = effective_preferred(config, supported);
        let max_request_payload_bytes = effective_payload_limit(config.max_request_payload_bytes);
        let max_request_batch_items = effective_batch_limit(config.max_request_batch_items);
        let max_response_payload_bytes = effective_payload_limit(config.max_response_payload_bytes);
        let max_response_batch_items = effective_batch_limit(config.max_response_batch_items);
        let max_request_message_len =
            compute_max_message_len(max_request_payload_bytes, max_request_batch_items)?;
        let max_response_message_len =
            compute_max_message_len(max_response_payload_bytes, max_response_batch_items)?;
        let max_default_message_len = max_request_message_len.max(max_response_message_len);

        let pipe = unsafe {
            CreateNamedPipeW(
                pipe_name.as_ptr(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                1,
                max_default_message_len as DWORD,
                max_default_message_len as DWORD,
                0,
                ptr::null_mut(),
            )
        };
        if pipe == INVALID_HANDLE_VALUE {
            return Err(win32_error("CreateNamedPipeW"));
        }

        Ok(NamedPipeListener {
            pipe,
            run_dir: config.run_dir.to_string_lossy().into_owned(),
            service_name: config.service_name.clone(),
            supported_profiles: supported,
            preferred_profiles: preferred,
            max_request_payload_bytes,
            max_request_batch_items,
            max_response_payload_bytes,
            max_response_batch_items,
            auth_token: config.auth_token,
            shm_spin_tries: config.shm_spin_tries,
        })
    }

    pub fn accept(&mut self, timeout: Option<Duration>) -> io::Result<NamedPipeSession> {
        let timeout_ms = timeout.map(|d| d.as_millis() as u32).unwrap_or(0);

        set_pipe_mode(self.pipe, PIPE_NOWAIT)?;

        let deadline = if timeout_ms == 0 {
            0u64
        } else {
            now_ms() + timeout_ms as u64
        };
        loop {
            let ok = unsafe { ConnectNamedPipe(self.pipe, ptr::null_mut()) };
            if ok != 0 {
                break;
            }
            let err = unsafe { GetLastError() };
            if err == ERROR_PIPE_CONNECTED {
                break;
            }
            if err != ERROR_PIPE_LISTENING && err != ERROR_NO_DATA {
                let _ = set_pipe_mode(self.pipe, PIPE_WAIT);
                return Err(win32_error("ConnectNamedPipe"));
            }
            if deadline != 0 && now_ms() >= deadline {
                let _ = set_pipe_mode(self.pipe, PIPE_WAIT);
                return Err(io::Error::new(io::ErrorKind::TimedOut, "accept timeout"));
            }
            unsafe {
                Sleep(1);
            }
        }

        set_pipe_mode(self.pipe, PIPE_WAIT)?;

        let negotiated = server_handshake(self, self.pipe, timeout_ms)?;

        let mut session = NamedPipeSession {
            pipe: self.pipe,
            negotiated_profile: negotiated.profile,
            packet_size: negotiated.packet_size,
            max_request_message_len: negotiated.max_request_message_len,
            max_response_message_len: negotiated.max_response_message_len,
            shm_server: None,
            connected: true,
        };
        if is_shm_profile(negotiated.profile) {
            let shm_config = NamedPipeConfig {
                run_dir: self.run_dir.clone().into(),
                service_name: self.service_name.clone(),
                supported_profiles: self.supported_profiles,
                preferred_profiles: self.preferred_profiles,
                max_request_payload_bytes: negotiated.agreed_max_request_payload_bytes,
                max_request_batch_items: negotiated.agreed_max_request_batch_items,
                max_response_payload_bytes: negotiated.agreed_max_response_payload_bytes,
                max_response_batch_items: negotiated.agreed_max_response_batch_items,
                auth_token: self.auth_token,
                shm_spin_tries: self.shm_spin_tries,
            };
            session.shm_server = Some(ShmServer::create(&shm_config, negotiated.profile)?);
        }

        Ok(session)
    }
}

impl NamedPipeSession {
    pub fn receive_message(
        &mut self,
        message: &mut [u8],
        timeout: Option<Duration>,
    ) -> io::Result<usize> {
        let timeout_ms = timeout.map(|d| d.as_millis() as u32).unwrap_or(0);
        if let Some(_) = self.shm_server {
            return self
                .shm_server
                .as_mut()
                .expect("shm server exists")
                .receive_message(message, timeout_ms);
        }
        if self.max_request_message_len == 0 || message.len() < self.max_request_message_len {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "message buffer is smaller than negotiated request size",
            ));
        }
        recv_transport_message(
            self.pipe,
            message,
            self.max_request_message_len,
            self.packet_size,
            timeout_ms,
        )
    }

    pub fn receive_frame(&mut self, timeout: Option<Duration>) -> io::Result<Frame> {
        let timeout_ms = timeout.map(|d| d.as_millis() as u32).unwrap_or(0);
        if let Some(ref mut shm) = self.shm_server {
            return shm.receive_frame(timeout_ms);
        }
        pipe_read_frame(self.pipe, timeout_ms)
    }

    pub fn send_message(&mut self, message: &[u8]) -> io::Result<()> {
        if let Some(_) = self.shm_server {
            return self
                .shm_server
                .as_mut()
                .expect("shm server exists")
                .send_message(message);
        }
        if self.max_response_message_len == 0 {
            return Err(io::Error::from_raw_os_error(87));
        }
        send_transport_message(
            self.pipe,
            message,
            self.max_response_message_len,
            self.packet_size,
        )
    }

    pub fn send_frame(&mut self, frame: &Frame) -> io::Result<()> {
        if let Some(ref mut shm) = self.shm_server {
            return shm.send_frame(frame);
        }
        pipe_write_frame(self.pipe, frame)
    }

    pub fn receive_increment(
        &mut self,
        timeout: Option<Duration>,
    ) -> io::Result<(u64, IncrementRequest)> {
        let frame = self.receive_frame(timeout)?;
        decode_increment_request(&frame)
    }

    pub fn send_increment(
        &mut self,
        request_id: u64,
        response: &IncrementResponse,
        _timeout: Option<Duration>,
    ) -> io::Result<()> {
        let frame = encode_increment_response(request_id, response);
        self.send_frame(&frame)
    }

    pub fn negotiated_profile(&self) -> u32 {
        self.negotiated_profile
    }
}

impl Drop for NamedPipeListener {
    fn drop(&mut self) {
        if self.pipe != INVALID_HANDLE_VALUE {
            close_handle(self.pipe);
        }
    }
}

impl Drop for NamedPipeSession {
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

impl NamedPipeServer {
    pub fn bind(config: &NamedPipeConfig) -> io::Result<Self> {
        Ok(Self {
            listener: NamedPipeListener::bind(config)?,
            session: None,
        })
    }

    pub fn accept(&mut self, timeout: Option<Duration>) -> io::Result<()> {
        if self.session.is_some() {
            return Err(io::Error::new(
                io::ErrorKind::AlreadyExists,
                "server is already connected",
            ));
        }
        self.session = Some(self.listener.accept(timeout)?);
        Ok(())
    }

    pub fn receive_message(
        &mut self,
        message: &mut [u8],
        timeout: Option<Duration>,
    ) -> io::Result<usize> {
        self.session
            .as_mut()
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotConnected, "server is not connected"))?
            .receive_message(message, timeout)
    }

    pub fn receive_frame(&mut self, timeout: Option<Duration>) -> io::Result<Frame> {
        self.session
            .as_mut()
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotConnected, "server is not connected"))?
            .receive_frame(timeout)
    }

    pub fn send_message(&mut self, message: &[u8]) -> io::Result<()> {
        self.session
            .as_mut()
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotConnected, "server is not connected"))?
            .send_message(message)
    }

    pub fn send_frame(&mut self, frame: &Frame) -> io::Result<()> {
        self.session
            .as_mut()
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotConnected, "server is not connected"))?
            .send_frame(frame)
    }

    pub fn receive_increment(
        &mut self,
        timeout: Option<Duration>,
    ) -> io::Result<(u64, IncrementRequest)> {
        self.session
            .as_mut()
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotConnected, "server is not connected"))?
            .receive_increment(timeout)
    }

    pub fn send_increment(
        &mut self,
        request_id: u64,
        response: &IncrementResponse,
        timeout: Option<Duration>,
    ) -> io::Result<()> {
        self.session
            .as_mut()
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotConnected, "server is not connected"))?
            .send_increment(request_id, response, timeout)
    }

    pub fn negotiated_profile(&self) -> u32 {
        self.session
            .as_ref()
            .map(NamedPipeSession::negotiated_profile)
            .unwrap_or(0)
    }
}

// ---------------------------------------------------------------------------
// Public API: NamedPipeClient
// ---------------------------------------------------------------------------

pub struct NamedPipeClient {
    pipe: HANDLE,
    negotiated_profile: u32,
    packet_size: u32,
    max_request_message_len: usize,
    max_response_message_len: usize,
    shm_client: Option<ShmClient>,
    next_request_id: u64,
}

#[derive(Clone, Copy, Debug)]
struct NegotiationResult {
    profile: u32,
    packet_size: u32,
    agreed_max_request_payload_bytes: u32,
    agreed_max_request_batch_items: u32,
    agreed_max_response_payload_bytes: u32,
    agreed_max_response_batch_items: u32,
    max_request_message_len: usize,
    max_response_message_len: usize,
}

impl NamedPipeClient {
    pub fn connect(config: &NamedPipeConfig, timeout: Option<Duration>) -> io::Result<Self> {
        let timeout_ms = timeout.map(|d| d.as_millis() as u32).unwrap_or(0);
        let pipe_name = to_wide(&build_pipe_name(config));
        let supported = effective_supported(config);
        let preferred = effective_preferred(config, supported);
        let max_request_payload_bytes = effective_payload_limit(config.max_request_payload_bytes);
        let max_request_batch_items = effective_batch_limit(config.max_request_batch_items);
        let max_response_payload_bytes = effective_payload_limit(config.max_response_payload_bytes);
        let max_response_batch_items = effective_batch_limit(config.max_response_batch_items);

        let deadline = if timeout_ms == 0 {
            0u64
        } else {
            now_ms() + timeout_ms as u64
        };
        let pipe = loop {
            let h = unsafe {
                CreateFileW(
                    pipe_name.as_ptr(),
                    GENERIC_READ | GENERIC_WRITE,
                    0,
                    ptr::null_mut(),
                    OPEN_EXISTING,
                    0,
                    NULL_HANDLE,
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
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "pipe connect timeout",
                ));
            }
            if err == ERROR_PIPE_BUSY {
                let wait = if timeout_ms == 0 {
                    NMPWAIT_WAIT_FOREVER
                } else {
                    50
                };
                unsafe {
                    WaitNamedPipeW(pipe_name.as_ptr(), wait);
                }
            } else {
                unsafe {
                    Sleep(1);
                }
            }
        };

        set_pipe_mode(pipe, PIPE_WAIT)?;

        let negotiated = client_handshake(
            pipe,
            supported,
            preferred,
            max_request_payload_bytes,
            max_request_batch_items,
            max_response_payload_bytes,
            max_response_batch_items,
            config.auth_token,
            timeout_ms,
        )?;

        let shm_client = if is_shm_profile(negotiated.profile) {
            let shm_config = NamedPipeConfig {
                run_dir: config.run_dir.clone(),
                service_name: config.service_name.clone(),
                supported_profiles: config.supported_profiles,
                preferred_profiles: config.preferred_profiles,
                max_request_payload_bytes: negotiated.agreed_max_request_payload_bytes,
                max_request_batch_items: negotiated.agreed_max_request_batch_items,
                max_response_payload_bytes: negotiated.agreed_max_response_payload_bytes,
                max_response_batch_items: negotiated.agreed_max_response_batch_items,
                auth_token: config.auth_token,
                shm_spin_tries: config.shm_spin_tries,
            };
            Some(ShmClient::create(
                &shm_config,
                negotiated.profile,
                timeout_ms,
            )?)
        } else {
            None
        };

        Ok(NamedPipeClient {
            pipe,
            negotiated_profile: negotiated.profile,
            packet_size: negotiated.packet_size,
            max_request_message_len: negotiated.max_request_message_len,
            max_response_message_len: negotiated.max_response_message_len,
            shm_client,
            next_request_id: 1,
        })
    }

    pub fn call_message(
        &mut self,
        request: &[u8],
        response: &mut [u8],
        timeout: Option<Duration>,
    ) -> io::Result<usize> {
        let timeout_ms = timeout.map(|d| d.as_millis() as u32).unwrap_or(0);
        if let Some(_) = self.shm_client {
            return self
                .shm_client
                .as_mut()
                .expect("shm client exists")
                .call_message(request, response, timeout_ms);
        }
        if self.max_request_message_len == 0 || self.max_response_message_len == 0 {
            return Err(io::Error::from_raw_os_error(87));
        }
        if response.len() < self.max_response_message_len {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "response buffer is smaller than negotiated response size",
            ));
        }
        send_transport_message(
            self.pipe,
            request,
            self.max_request_message_len,
            self.packet_size,
        )?;
        recv_transport_message(
            self.pipe,
            response,
            self.max_response_message_len,
            self.packet_size,
            timeout_ms,
        )
    }

    pub fn call_frame(&mut self, request: &Frame, timeout: Option<Duration>) -> io::Result<Frame> {
        let timeout_ms = timeout.map(|d| d.as_millis() as u32).unwrap_or(0);
        if let Some(ref mut shm) = self.shm_client {
            return shm.call_frame(request, timeout_ms);
        }
        pipe_write_frame(self.pipe, request)?;
        pipe_read_frame(self.pipe, timeout_ms)
    }

    pub fn call_increment(
        &mut self,
        request: &IncrementRequest,
        timeout: Option<Duration>,
    ) -> io::Result<IncrementResponse> {
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
