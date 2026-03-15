//! L1 Windows SHM transport.
//!
//! Shared memory data plane with spin + kernel event synchronization.
//! Uses CreateFileMappingW/MapViewOfFile for the region and auto-reset
//! kernel events for signaling (SHM_HYBRID profile).
//!
//! Wire-compatible with the C and Go implementations.

#[cfg(windows)]
mod ffi {
    #![allow(non_snake_case, non_camel_case_types, dead_code)]

    pub type HANDLE = isize;
    pub type DWORD = u32;
    pub type BOOL = i32;
    pub type LPCWSTR = *const u16;
    pub type LONG = i32;
    pub type LONG64 = i64;

    pub const INVALID_HANDLE_VALUE: HANDLE = -1;
    pub const PAGE_READWRITE: DWORD = 0x04;
    pub const FILE_MAP_ALL_ACCESS: DWORD = 0x000F001F;
    pub const EVENT_MODIFY_STATE: DWORD = 0x0002;
    pub const SYNCHRONIZE: DWORD = 0x00100000;
    pub const INFINITE: DWORD = 0xFFFFFFFF;
    pub const WAIT_OBJECT_0: DWORD = 0x00000000;
    pub const WAIT_TIMEOUT: DWORD = 0x00000102;

    extern "system" {
        pub fn CreateFileMappingW(
            hFile: HANDLE,
            lpFileMappingAttributes: *const core::ffi::c_void,
            flProtect: DWORD,
            dwMaximumSizeHigh: DWORD,
            dwMaximumSizeLow: DWORD,
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
            dwNumberOfBytesToMap: usize,
        ) -> *mut core::ffi::c_void;

        pub fn UnmapViewOfFile(lpBaseAddress: *const core::ffi::c_void) -> BOOL;

        pub fn CreateEventW(
            lpEventAttributes: *const core::ffi::c_void,
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

        pub fn CloseHandle(hObject: HANDLE) -> BOOL;

        pub fn GetLastError() -> DWORD;

        pub fn GetTickCount() -> DWORD;

        // Interlocked operations
        pub fn InterlockedExchange(Target: *mut LONG, Value: LONG) -> LONG;

        pub fn InterlockedCompareExchange(
            Destination: *mut LONG,
            Exchange: LONG,
            Comparand: LONG,
        ) -> LONG;

        pub fn InterlockedIncrement64(Addend: *mut LONG64) -> LONG64;

        pub fn InterlockedCompareExchange64(
            Destination: *mut LONG64,
            Exchange: LONG64,
            Comparand: LONG64,
        ) -> LONG64;
    }
}

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

/// Magic value: "NSWH" as u32 LE.
pub const REGION_MAGIC: u32 = 0x4e535748;
pub const REGION_VERSION: u32 = 3;
pub const HEADER_LEN: u32 = 128;
pub const CACHELINE_SIZE: u32 = 64;
pub const DEFAULT_SPIN_TRIES: u32 = 1024;
pub const BUSYWAIT_POLL_MASK: u32 = 1023;

pub const PROFILE_HYBRID: u32 = 0x02;
pub const PROFILE_BUSYWAIT: u32 = 0x04;

// Header field byte offsets
const OFF_MAGIC: usize = 0;
const OFF_VERSION: usize = 4;
const OFF_HEADER_LEN: usize = 8;
const OFF_PROFILE: usize = 12;
const OFF_REQ_OFFSET: usize = 16;
const OFF_REQ_CAPACITY: usize = 20;
const OFF_RESP_OFFSET: usize = 24;
const OFF_RESP_CAPACITY: usize = 28;
const OFF_SPIN_TRIES: usize = 32;
const OFF_REQ_LEN: usize = 36;
const OFF_RESP_LEN: usize = 40;
const OFF_REQ_CLIENT_CLOSED: usize = 44;
const OFF_REQ_SERVER_WAITING: usize = 48;
const OFF_RESP_SERVER_CLOSED: usize = 52;
const OFF_RESP_CLIENT_WAITING: usize = 56;
const OFF_REQ_SEQ: usize = 64;
const OFF_RESP_SEQ: usize = 72;

// FNV-1a 64-bit constants
const FNV1A_OFFSET_BASIS: u64 = 0xcbf29ce484222325;
const FNV1A_PRIME: u64 = 0x00000100000001B3;

// ---------------------------------------------------------------------------
//  Errors
// ---------------------------------------------------------------------------

/// Windows SHM transport errors.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum WinShmError {
    /// Invalid argument.
    BadParam(String),
    /// CreateFileMappingW failed.
    CreateMapping(u32),
    /// OpenFileMappingW failed.
    OpenMapping(u32),
    /// MapViewOfFile failed.
    MapView(u32),
    /// CreateEventW failed.
    CreateEvent(u32),
    /// OpenEventW failed.
    OpenEvent(u32),
    /// Header magic mismatch.
    BadMagic,
    /// Header version mismatch.
    BadVersion,
    /// header_len mismatch.
    BadHeader,
    /// Profile mismatch.
    BadProfile,
    /// Message exceeds area capacity.
    MsgTooLarge,
    /// Wait timed out.
    Timeout,
    /// Peer closed.
    Disconnected,
}

impl std::fmt::Display for WinShmError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            WinShmError::BadParam(s) => write!(f, "bad parameter: {s}"),
            WinShmError::CreateMapping(e) => write!(f, "CreateFileMappingW failed: {e}"),
            WinShmError::OpenMapping(e) => write!(f, "OpenFileMappingW failed: {e}"),
            WinShmError::MapView(e) => write!(f, "MapViewOfFile failed: {e}"),
            WinShmError::CreateEvent(e) => write!(f, "CreateEventW failed: {e}"),
            WinShmError::OpenEvent(e) => write!(f, "OpenEventW failed: {e}"),
            WinShmError::BadMagic => write!(f, "SHM header magic mismatch"),
            WinShmError::BadVersion => write!(f, "SHM header version mismatch"),
            WinShmError::BadHeader => write!(f, "SHM header_len mismatch"),
            WinShmError::BadProfile => write!(f, "SHM profile mismatch"),
            WinShmError::MsgTooLarge => write!(f, "message exceeds SHM area capacity"),
            WinShmError::Timeout => write!(f, "SHM wait timed out"),
            WinShmError::Disconnected => write!(f, "peer closed SHM session"),
        }
    }
}

impl std::error::Error for WinShmError {}

// ---------------------------------------------------------------------------
//  Role
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WinShmRole {
    Server = 1,
    Client = 2,
}

// ---------------------------------------------------------------------------
//  Compile-time header size assertion
// ---------------------------------------------------------------------------

// We don't map a Rust struct onto the header; we use raw pointer arithmetic.
// Assert that our offset constants are consistent.
const _: () = assert!(OFF_REQ_SEQ == 64);
const _: () = assert!(OFF_RESP_SEQ == 72);
const _: () = assert!(HEADER_LEN == 128);

// ---------------------------------------------------------------------------
//  Context
// ---------------------------------------------------------------------------

/// A handle to a Windows SHM region.
#[cfg(windows)]
pub struct WinShmContext {
    role: WinShmRole,
    mapping: ffi::HANDLE,
    base: *mut u8,
    region_size: usize,

    req_event: ffi::HANDLE,
    resp_event: ffi::HANDLE,

    profile: u32,
    request_offset: u32,
    request_capacity: u32,
    response_offset: u32,
    response_capacity: u32,
    spin_tries: u32,

    local_req_seq: i64,
    local_resp_seq: i64,
}

#[cfg(windows)]
unsafe impl Send for WinShmContext {}

#[cfg(windows)]
impl WinShmContext {
    pub fn role(&self) -> WinShmRole {
        self.role
    }

    /// Create a Windows SHM region (server side).
    pub fn server_create(
        run_dir: &str,
        service_name: &str,
        auth_token: u64,
        profile: u32,
        req_capacity: u32,
        resp_capacity: u32,
    ) -> Result<Self, WinShmError> {
        validate_service_name(service_name)?;
        validate_profile(profile)?;

        let hash = compute_shm_hash(run_dir, service_name, auth_token);
        let mapping_name = build_object_name(hash, service_name, profile, "mapping")?;

        let req_cap = align_cacheline(req_capacity);
        let resp_cap = align_cacheline(resp_capacity);
        let req_off = align_cacheline(HEADER_LEN);
        let resp_off = align_cacheline(req_off + req_cap);
        let region_size = (resp_off + resp_cap) as usize;

        // Create page-file backed mapping
        let mapping = unsafe {
            ffi::CreateFileMappingW(
                ffi::INVALID_HANDLE_VALUE,
                std::ptr::null(),
                ffi::PAGE_READWRITE,
                (region_size >> 32) as u32,
                (region_size & 0xFFFFFFFF) as u32,
                mapping_name.as_ptr(),
            )
        };
        if mapping == 0 {
            return Err(WinShmError::CreateMapping(last_error()));
        }

        let base = unsafe {
            ffi::MapViewOfFile(mapping, ffi::FILE_MAP_ALL_ACCESS, 0, 0, region_size)
        };
        if base.is_null() {
            let e = last_error();
            unsafe { ffi::CloseHandle(mapping) };
            return Err(WinShmError::MapView(e));
        }
        let base = base as *mut u8;

        // Zero region
        unsafe { std::ptr::write_bytes(base, 0, region_size) };

        // Write header
        write_u32(base, OFF_MAGIC, REGION_MAGIC);
        write_u32(base, OFF_VERSION, REGION_VERSION);
        write_u32(base, OFF_HEADER_LEN, HEADER_LEN);
        write_u32(base, OFF_PROFILE, profile);
        write_u32(base, OFF_REQ_OFFSET, req_off);
        write_u32(base, OFF_REQ_CAPACITY, req_cap);
        write_u32(base, OFF_RESP_OFFSET, resp_off);
        write_u32(base, OFF_RESP_CAPACITY, resp_cap);
        write_u32(base, OFF_SPIN_TRIES, DEFAULT_SPIN_TRIES);

        // Memory barrier
        std::sync::atomic::fence(std::sync::atomic::Ordering::Release);

        // Create events for HYBRID
        let (req_event, resp_event) = if profile == PROFILE_HYBRID {
            let re_name = build_object_name(hash, service_name, profile, "req_event")?;
            let re = unsafe { ffi::CreateEventW(std::ptr::null(), 0, 0, re_name.as_ptr()) };
            if re == 0 {
                let e = last_error();
                unsafe {
                    ffi::UnmapViewOfFile(base as *const _);
                    ffi::CloseHandle(mapping);
                }
                return Err(WinShmError::CreateEvent(e));
            }

            let rsp_name = build_object_name(hash, service_name, profile, "resp_event")?;
            let rsp = unsafe { ffi::CreateEventW(std::ptr::null(), 0, 0, rsp_name.as_ptr()) };
            if rsp == 0 {
                let e = last_error();
                unsafe {
                    ffi::CloseHandle(re);
                    ffi::UnmapViewOfFile(base as *const _);
                    ffi::CloseHandle(mapping);
                }
                return Err(WinShmError::CreateEvent(e));
            }
            (re, rsp)
        } else {
            (ffi::INVALID_HANDLE_VALUE, ffi::INVALID_HANDLE_VALUE)
        };

        Ok(WinShmContext {
            role: WinShmRole::Server,
            mapping,
            base,
            region_size,
            req_event,
            resp_event,
            profile,
            request_offset: req_off,
            request_capacity: req_cap,
            response_offset: resp_off,
            response_capacity: resp_cap,
            spin_tries: DEFAULT_SPIN_TRIES,
            local_req_seq: 0,
            local_resp_seq: 0,
        })
    }

    /// Attach to an existing Windows SHM region (client side).
    pub fn client_attach(
        run_dir: &str,
        service_name: &str,
        auth_token: u64,
        profile: u32,
    ) -> Result<Self, WinShmError> {
        validate_service_name(service_name)?;
        validate_profile(profile)?;

        let hash = compute_shm_hash(run_dir, service_name, auth_token);
        let mapping_name = build_object_name(hash, service_name, profile, "mapping")?;

        let mapping = unsafe {
            ffi::OpenFileMappingW(ffi::FILE_MAP_ALL_ACCESS, 0, mapping_name.as_ptr())
        };
        if mapping == 0 {
            return Err(WinShmError::OpenMapping(last_error()));
        }

        let base = unsafe {
            ffi::MapViewOfFile(mapping, ffi::FILE_MAP_ALL_ACCESS, 0, 0, 0)
        };
        if base.is_null() {
            let e = last_error();
            unsafe { ffi::CloseHandle(mapping) };
            return Err(WinShmError::MapView(e));
        }
        let base = base as *mut u8;

        // Acquire barrier
        std::sync::atomic::fence(std::sync::atomic::Ordering::Acquire);

        // Validate header
        let magic = read_u32(base, OFF_MAGIC);
        if magic != REGION_MAGIC {
            unsafe {
                ffi::UnmapViewOfFile(base as *const _);
                ffi::CloseHandle(mapping);
            }
            return Err(WinShmError::BadMagic);
        }

        let version = read_u32(base, OFF_VERSION);
        if version != REGION_VERSION {
            unsafe {
                ffi::UnmapViewOfFile(base as *const _);
                ffi::CloseHandle(mapping);
            }
            return Err(WinShmError::BadVersion);
        }

        let hdr_len = read_u32(base, OFF_HEADER_LEN);
        if hdr_len != HEADER_LEN {
            unsafe {
                ffi::UnmapViewOfFile(base as *const _);
                ffi::CloseHandle(mapping);
            }
            return Err(WinShmError::BadHeader);
        }

        let hdr_profile = read_u32(base, OFF_PROFILE);
        if hdr_profile != profile {
            unsafe {
                ffi::UnmapViewOfFile(base as *const _);
                ffi::CloseHandle(mapping);
            }
            return Err(WinShmError::BadProfile);
        }

        let req_off = read_u32(base, OFF_REQ_OFFSET);
        let req_cap = read_u32(base, OFF_REQ_CAPACITY);
        let resp_off = read_u32(base, OFF_RESP_OFFSET);
        let resp_cap = read_u32(base, OFF_RESP_CAPACITY);
        let spin = read_u32(base, OFF_SPIN_TRIES);
        let region_size = (resp_off + resp_cap) as usize;

        // Read current sequence numbers via interlocked
        let cur_req_seq = interlocked_read_i64(base, OFF_REQ_SEQ);
        let cur_resp_seq = interlocked_read_i64(base, OFF_RESP_SEQ);

        // Open events for HYBRID
        let (req_event, resp_event) = if profile == PROFILE_HYBRID {
            let re_name = build_object_name(hash, service_name, profile, "req_event")?;
            let re = unsafe {
                ffi::OpenEventW(ffi::EVENT_MODIFY_STATE | ffi::SYNCHRONIZE, 0, re_name.as_ptr())
            };
            if re == 0 {
                let e = last_error();
                unsafe {
                    ffi::UnmapViewOfFile(base as *const _);
                    ffi::CloseHandle(mapping);
                }
                return Err(WinShmError::OpenEvent(e));
            }

            let rsp_name = build_object_name(hash, service_name, profile, "resp_event")?;
            let rsp = unsafe {
                ffi::OpenEventW(
                    ffi::EVENT_MODIFY_STATE | ffi::SYNCHRONIZE,
                    0,
                    rsp_name.as_ptr(),
                )
            };
            if rsp == 0 {
                let e = last_error();
                unsafe {
                    ffi::CloseHandle(re);
                    ffi::UnmapViewOfFile(base as *const _);
                    ffi::CloseHandle(mapping);
                }
                return Err(WinShmError::OpenEvent(e));
            }
            (re, rsp)
        } else {
            (ffi::INVALID_HANDLE_VALUE, ffi::INVALID_HANDLE_VALUE)
        };

        Ok(WinShmContext {
            role: WinShmRole::Client,
            mapping,
            base,
            region_size,
            req_event,
            resp_event,
            profile,
            request_offset: req_off,
            request_capacity: req_cap,
            response_offset: resp_off,
            response_capacity: resp_cap,
            spin_tries: spin,
            local_req_seq: cur_req_seq,
            local_resp_seq: cur_resp_seq,
        })
    }

    /// Publish a message. Client sends to request area; server to response.
    pub fn send(&mut self, msg: &[u8]) -> Result<(), WinShmError> {
        if self.base.is_null() || msg.is_empty() {
            return Err(WinShmError::BadParam("null context or empty message".into()));
        }

        let (area_off, area_cap, len_off, seq_off, peer_waiting_off, peer_event) =
            match self.role {
                WinShmRole::Client => (
                    self.request_offset,
                    self.request_capacity,
                    OFF_REQ_LEN,
                    OFF_REQ_SEQ,
                    OFF_REQ_SERVER_WAITING,
                    self.req_event,
                ),
                WinShmRole::Server => (
                    self.response_offset,
                    self.response_capacity,
                    OFF_RESP_LEN,
                    OFF_RESP_SEQ,
                    OFF_RESP_CLIENT_WAITING,
                    self.resp_event,
                ),
            };

        if msg.len() > area_cap as usize {
            return Err(WinShmError::MsgTooLarge);
        }

        // 1. Write message data
        unsafe {
            std::ptr::copy_nonoverlapping(
                msg.as_ptr(),
                self.base.add(area_off as usize),
                msg.len(),
            );
        }

        // 2. Store message length (interlocked exchange)
        interlocked_exchange_i32(self.base, len_off, msg.len() as i32);

        // 3. Increment sequence number
        interlocked_increment_i64(self.base, seq_off);

        // 4. If HYBRID and peer waiting, signal event
        if self.profile == PROFILE_HYBRID {
            let waiting = interlocked_read_i32(self.base, peer_waiting_off);
            if waiting != 0 {
                unsafe { ffi::SetEvent(peer_event) };
            }
        }

        match self.role {
            WinShmRole::Client => self.local_req_seq += 1,
            WinShmRole::Server => self.local_resp_seq += 1,
        }

        Ok(())
    }

    /// Receive a message into the caller-provided buffer.
    pub fn receive(&mut self, buf: &mut [u8], timeout_ms: u32) -> Result<usize, WinShmError> {
        if self.base.is_null() {
            return Err(WinShmError::BadParam("null context".into()));
        }
        if buf.is_empty() {
            return Err(WinShmError::BadParam("empty buffer".into()));
        }

        let (area_off, len_off, seq_off, self_waiting_off, peer_closed_off, wait_event, expected_seq) =
            match self.role {
                WinShmRole::Server => (
                    self.request_offset,
                    OFF_REQ_LEN,
                    OFF_REQ_SEQ,
                    OFF_REQ_SERVER_WAITING,
                    OFF_REQ_CLIENT_CLOSED,
                    self.req_event,
                    self.local_req_seq + 1,
                ),
                WinShmRole::Client => (
                    self.response_offset,
                    OFF_RESP_LEN,
                    OFF_RESP_SEQ,
                    OFF_RESP_CLIENT_WAITING,
                    OFF_RESP_SERVER_CLOSED,
                    self.resp_event,
                    self.local_resp_seq + 1,
                ),
            };

        // Phase 1: spin
        let mut observed = false;
        let mut mlen: i32 = 0;
        for _ in 0..self.spin_tries {
            let cur = interlocked_read_i64(self.base, seq_off);
            if cur >= expected_seq {
                mlen = interlocked_read_i32(self.base, len_off);
                if mlen > 0 && (mlen as usize) <= buf.len() {
                    unsafe {
                        std::ptr::copy_nonoverlapping(
                            self.base.add(area_off as usize),
                            buf.as_mut_ptr(),
                            mlen as usize,
                        );
                    }
                }
                observed = true;
                break;
            }
            cpu_relax();
        }

        // Phase 2: kernel wait or busy-wait
        if !observed {
            if self.profile == PROFILE_HYBRID {
                interlocked_exchange_i32(self.base, self_waiting_off, 1);
                std::sync::atomic::fence(std::sync::atomic::Ordering::SeqCst);

                let cur = interlocked_read_i64(self.base, seq_off);
                if cur < expected_seq {
                    let wait_ms = if timeout_ms == 0 {
                        ffi::INFINITE
                    } else {
                        timeout_ms
                    };
                    let ret = unsafe { ffi::WaitForSingleObject(wait_event, wait_ms) };

                    interlocked_exchange_i32(self.base, self_waiting_off, 0);

                    // Check peer close
                    if interlocked_read_i32(self.base, peer_closed_off) != 0 {
                        self.advance_seq(expected_seq);
                        return Err(WinShmError::Disconnected);
                    }

                    if ret == ffi::WAIT_TIMEOUT {
                        return Err(WinShmError::Timeout);
                    }

                    let cur = interlocked_read_i64(self.base, seq_off);
                    if cur < expected_seq {
                        return Err(WinShmError::Timeout);
                    }
                } else {
                    interlocked_exchange_i32(self.base, self_waiting_off, 0);
                }

                // Copy after waking
                mlen = interlocked_read_i32(self.base, len_off);
                if mlen > 0 && (mlen as usize) <= buf.len() {
                    unsafe {
                        std::ptr::copy_nonoverlapping(
                            self.base.add(area_off as usize),
                            buf.as_mut_ptr(),
                            mlen as usize,
                        );
                    }
                }
            } else {
                // BUSYWAIT
                let start = unsafe { ffi::GetTickCount() };
                loop {
                    let cur = interlocked_read_i64(self.base, seq_off);
                    if cur >= expected_seq {
                        mlen = interlocked_read_i32(self.base, len_off);
                        if mlen > 0 && (mlen as usize) <= buf.len() {
                            unsafe {
                                std::ptr::copy_nonoverlapping(
                                    self.base.add(area_off as usize),
                                    buf.as_mut_ptr(),
                                    mlen as usize,
                                );
                            }
                        }
                        break;
                    }

                    if timeout_ms > 0 {
                        let elapsed = unsafe { ffi::GetTickCount() }.wrapping_sub(start);
                        if elapsed >= timeout_ms {
                            return Err(WinShmError::Timeout);
                        }
                    }

                    if interlocked_read_i32(self.base, peer_closed_off) != 0 {
                        self.advance_seq(expected_seq);
                        return Err(WinShmError::Disconnected);
                    }

                    cpu_relax();
                }
            }
        }

        self.advance_seq(expected_seq);

        if (mlen as usize) > buf.len() {
            return Err(WinShmError::MsgTooLarge);
        }

        Ok(mlen as usize)
    }

    fn advance_seq(&mut self, expected_seq: i64) {
        match self.role {
            WinShmRole::Server => self.local_req_seq = expected_seq,
            WinShmRole::Client => self.local_resp_seq = expected_seq,
        }
    }

    /// Close client (unmap, close handles, set close flag).
    pub fn close(&mut self) {
        if !self.base.is_null() {
            // Set close flag
            if self.role == WinShmRole::Client {
                interlocked_exchange_i32(self.base, OFF_REQ_CLIENT_CLOSED, 1);
            }
            std::sync::atomic::fence(std::sync::atomic::Ordering::SeqCst);
        }

        if self.profile == PROFILE_HYBRID && self.req_event != ffi::INVALID_HANDLE_VALUE {
            if self.role == WinShmRole::Client {
                unsafe { ffi::SetEvent(self.req_event) };
            }
        }

        self.cleanup_handles();
    }

    /// Destroy server (set close flag, signal, unmap, close handles).
    pub fn destroy(&mut self) {
        if !self.base.is_null() {
            interlocked_exchange_i32(self.base, OFF_RESP_SERVER_CLOSED, 1);
            std::sync::atomic::fence(std::sync::atomic::Ordering::SeqCst);
        }

        if self.profile == PROFILE_HYBRID && self.resp_event != ffi::INVALID_HANDLE_VALUE {
            unsafe { ffi::SetEvent(self.resp_event) };
        }

        self.cleanup_handles();
    }

    fn cleanup_handles(&mut self) {
        if !self.base.is_null() {
            unsafe { ffi::UnmapViewOfFile(self.base as *const _) };
            self.base = std::ptr::null_mut();
        }
        if self.mapping != 0 {
            unsafe { ffi::CloseHandle(self.mapping) };
            self.mapping = 0;
        }
        if self.req_event != ffi::INVALID_HANDLE_VALUE {
            unsafe { ffi::CloseHandle(self.req_event) };
            self.req_event = ffi::INVALID_HANDLE_VALUE;
        }
        if self.resp_event != ffi::INVALID_HANDLE_VALUE {
            unsafe { ffi::CloseHandle(self.resp_event) };
            self.resp_event = ffi::INVALID_HANDLE_VALUE;
        }
        self.region_size = 0;
    }
}

#[cfg(windows)]
impl Drop for WinShmContext {
    fn drop(&mut self) {
        match self.role {
            WinShmRole::Server => self.destroy(),
            WinShmRole::Client => self.close(),
        }
    }
}

// ---------------------------------------------------------------------------
//  Internal helpers
// ---------------------------------------------------------------------------

fn align_cacheline(v: u32) -> u32 {
    (v + (CACHELINE_SIZE - 1)) & !(CACHELINE_SIZE - 1)
}

fn validate_service_name(name: &str) -> Result<(), WinShmError> {
    if name.is_empty() {
        return Err(WinShmError::BadParam("empty service name".into()));
    }
    if name == "." || name == ".." {
        return Err(WinShmError::BadParam("service name cannot be '.' or '..'".into()));
    }
    for c in name.bytes() {
        match c {
            b'a'..=b'z' | b'A'..=b'Z' | b'0'..=b'9' | b'.' | b'_' | b'-' => {}
            _ => {
                return Err(WinShmError::BadParam(format!(
                    "service name contains invalid character: {:?}",
                    c as char,
                )))
            }
        }
    }
    Ok(())
}

fn validate_profile(profile: u32) -> Result<(), WinShmError> {
    if profile != PROFILE_HYBRID && profile != PROFILE_BUSYWAIT {
        return Err(WinShmError::BadParam(format!("invalid profile: {profile}")));
    }
    Ok(())
}

/// FNV-1a 64-bit hash.
pub fn fnv1a_64(data: &[u8]) -> u64 {
    let mut hash = FNV1A_OFFSET_BASIS;
    for &b in data {
        hash ^= b as u64;
        hash = hash.wrapping_mul(FNV1A_PRIME);
    }
    hash
}

fn compute_shm_hash(run_dir: &str, service_name: &str, auth_token: u64) -> u64 {
    let input = format!("{}\n{}\n{}", run_dir, service_name, auth_token);
    fnv1a_64(input.as_bytes())
}

fn build_object_name(
    hash: u64,
    service_name: &str,
    profile: u32,
    suffix: &str,
) -> Result<Vec<u16>, WinShmError> {
    let narrow = format!(
        "Local\\netipc-{:016x}-{}-p{}-{}",
        hash, service_name, profile, suffix
    );
    if narrow.len() >= 256 {
        return Err(WinShmError::BadParam("object name too long".into()));
    }
    let mut wide: Vec<u16> = narrow.encode_utf16().collect();
    wide.push(0);
    Ok(wide)
}

#[cfg(windows)]
fn last_error() -> u32 {
    unsafe { ffi::GetLastError() }
}

fn read_u32(base: *mut u8, offset: usize) -> u32 {
    unsafe { std::ptr::read_unaligned(base.add(offset) as *const u32) }
}

fn write_u32(base: *mut u8, offset: usize, val: u32) {
    unsafe { std::ptr::write_unaligned(base.add(offset) as *mut u32, val) };
}

#[cfg(windows)]
fn interlocked_read_i32(base: *mut u8, offset: usize) -> i32 {
    unsafe {
        ffi::InterlockedCompareExchange(base.add(offset) as *mut ffi::LONG, 0, 0)
    }
}

#[cfg(windows)]
fn interlocked_exchange_i32(base: *mut u8, offset: usize, val: i32) {
    unsafe {
        ffi::InterlockedExchange(base.add(offset) as *mut ffi::LONG, val);
    }
}

#[cfg(windows)]
fn interlocked_read_i64(base: *mut u8, offset: usize) -> i64 {
    unsafe {
        ffi::InterlockedCompareExchange64(base.add(offset) as *mut ffi::LONG64, 0, 0)
    }
}

#[cfg(windows)]
fn interlocked_increment_i64(base: *mut u8, offset: usize) {
    unsafe {
        ffi::InterlockedIncrement64(base.add(offset) as *mut ffi::LONG64);
    }
}

#[cfg(windows)]
#[inline]
fn cpu_relax() {
    #[cfg(any(target_arch = "x86_64", target_arch = "x86"))]
    unsafe {
        std::arch::asm!("pause", options(nomem, nostack));
    }
    #[cfg(target_arch = "aarch64")]
    unsafe {
        std::arch::asm!("yield", options(nomem, nostack));
    }
    #[cfg(not(any(target_arch = "x86_64", target_arch = "x86", target_arch = "aarch64")))]
    {
        std::sync::atomic::fence(std::sync::atomic::Ordering::SeqCst);
    }
}

// ---------------------------------------------------------------------------
//  Tests (non-Windows: compile-check only)
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_fnv1a_64_deterministic() {
        let h1 = fnv1a_64(b"C:\\Temp\\netdata\ncgroups-snapshot\n12345");
        let h2 = fnv1a_64(b"C:\\Temp\\netdata\ncgroups-snapshot\n12345");
        assert_eq!(h1, h2);
        assert_ne!(h1, 0);
    }

    #[test]
    fn test_fnv1a_64_different_tokens() {
        let h1 = fnv1a_64(b"dir\nsvc\n100");
        let h2 = fnv1a_64(b"dir\nsvc\n200");
        assert_ne!(h1, h2);
    }

    #[test]
    fn test_validate_service_name() {
        assert!(validate_service_name("cgroups-snapshot").is_ok());
        assert!(validate_service_name("test.v1").is_ok());
        assert!(validate_service_name("").is_err());
        assert!(validate_service_name(".").is_err());
        assert!(validate_service_name("..").is_err());
        assert!(validate_service_name("has/slash").is_err());
        assert!(validate_service_name("has space").is_err());
    }

    #[test]
    fn test_align_cacheline() {
        assert_eq!(align_cacheline(0), 0);
        assert_eq!(align_cacheline(1), 64);
        assert_eq!(align_cacheline(64), 64);
        assert_eq!(align_cacheline(65), 128);
        assert_eq!(align_cacheline(128), 128);
    }

    #[test]
    fn test_object_name_format() {
        let name = build_object_name(0xDEADBEEF, "test-svc", 2, "mapping").unwrap();
        // Check it's NUL-terminated
        assert_eq!(*name.last().unwrap(), 0);
        let narrow: String = name[..name.len() - 1]
            .iter()
            .map(|&c| c as u8 as char)
            .collect();
        assert!(narrow.starts_with("Local\\netipc-"));
        assert!(narrow.contains("-test-svc-p2-mapping"));
    }
}
