//! L1 POSIX SHM transport (Linux only).
//!
//! Shared memory data plane with spin+futex synchronization.
//! The SHM region carries the same outer protocol envelope as the UDS
//! transport. Higher levels see no difference.
//!
//! Wire-compatible with the C implementation in netipc_shm.c.

use std::path::{Path, PathBuf};
use std::ptr;

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

/// Magic value: "NSHM" as u32 LE.
pub const REGION_MAGIC: u32 = 0x4e53484d;
pub const REGION_VERSION: u16 = 3;
pub const REGION_ALIGNMENT: u32 = 64;
pub const HEADER_LEN: u16 = 64;
pub const DEFAULT_SPIN_TRIES: u32 = 128;

// Byte offsets of atomic fields in the region header.
const OFF_REQ_SEQ: usize = 32;
const OFF_RESP_SEQ: usize = 40;
const OFF_REQ_LEN: usize = 48;
const OFF_RESP_LEN: usize = 52;
const OFF_REQ_SIGNAL: usize = 56;
const OFF_RESP_SIGNAL: usize = 60;

// futex operations
const FUTEX_WAIT: i32 = 0;
const FUTEX_WAKE: i32 = 1;

// ---------------------------------------------------------------------------
//  Errors
// ---------------------------------------------------------------------------

/// SHM transport errors.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ShmError {
    /// SHM path exceeds limit.
    PathTooLong,
    /// open/shm_open failed.
    Open(i32),
    /// ftruncate failed.
    Truncate(i32),
    /// mmap failed.
    Mmap(i32),
    /// Header magic mismatch.
    BadMagic,
    /// Header version mismatch.
    BadVersion,
    /// header_len mismatch or corrupt.
    BadHeader,
    /// File too small / capacity mismatch.
    BadSize,
    /// Live server owns the region.
    AddrInUse,
    /// Server hasn't finished setup (retry).
    NotReady,
    /// Message exceeds area capacity.
    MsgTooLarge,
    /// Futex wait timed out.
    Timeout,
    /// Invalid argument.
    BadParam(String),
    /// Owner process has exited.
    PeerDead,
}

impl std::fmt::Display for ShmError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ShmError::PathTooLong => write!(f, "SHM path exceeds limit"),
            ShmError::Open(e) => write!(f, "open failed: errno {e}"),
            ShmError::Truncate(e) => write!(f, "ftruncate failed: errno {e}"),
            ShmError::Mmap(e) => write!(f, "mmap failed: errno {e}"),
            ShmError::BadMagic => write!(f, "SHM header magic mismatch"),
            ShmError::BadVersion => write!(f, "SHM header version mismatch"),
            ShmError::BadHeader => write!(f, "SHM header_len mismatch"),
            ShmError::BadSize => write!(f, "SHM file too small for declared areas"),
            ShmError::AddrInUse => write!(f, "SHM region owned by live server"),
            ShmError::NotReady => write!(f, "SHM server not ready"),
            ShmError::MsgTooLarge => write!(f, "message exceeds SHM area capacity"),
            ShmError::Timeout => write!(f, "SHM futex wait timed out"),
            ShmError::BadParam(s) => write!(f, "bad parameter: {s}"),
            ShmError::PeerDead => write!(f, "SHM owner process has exited"),
        }
    }
}

impl std::error::Error for ShmError {}

// ---------------------------------------------------------------------------
//  Role
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ShmRole {
    Server = 1,
    Client = 2,
}

// ---------------------------------------------------------------------------
//  Region header (64 bytes at offset 0)
// ---------------------------------------------------------------------------

/// On-disk layout. Not used directly for atomic accesses; we use
/// raw pointer arithmetic like the C implementation.
#[repr(C)]
struct RegionHeader {
    magic: u32,              //  0
    version: u16,            //  4
    header_len: u16,         //  6
    owner_pid: i32,          //  8
    owner_generation: u32,   // 12
    request_offset: u32,     // 16
    request_capacity: u32,   // 20
    response_offset: u32,    // 24
    response_capacity: u32,  // 28
    req_seq: u64,            // 32
    resp_seq: u64,           // 40
    req_len: u32,            // 48
    resp_len: u32,           // 52
    req_signal: u32,         // 56
    resp_signal: u32,        // 60
}

const _: () = assert!(std::mem::size_of::<RegionHeader>() == 64);

// ---------------------------------------------------------------------------
//  SHM context
// ---------------------------------------------------------------------------

/// A handle to a shared memory region (server or client side).
pub struct ShmContext {
    role: ShmRole,
    fd: i32,
    base: *mut u8,
    region_size: usize,

    // Cached from header
    request_offset: u32,
    request_capacity: u32,
    response_offset: u32,
    response_capacity: u32,

    // Sequence tracking
    local_req_seq: u64,
    local_resp_seq: u64,

    spin_tries: u32,
    owner_generation: u32,
    path: PathBuf,
}

// ShmContext is Send: the mmap pointer is process-global shared memory,
// and the context is used by one thread at a time (single in-flight).
unsafe impl Send for ShmContext {}

impl ShmContext {
    /// Returns the role.
    pub fn role(&self) -> ShmRole {
        self.role
    }

    /// Returns the raw file descriptor.
    pub fn fd(&self) -> i32 {
        self.fd
    }

    /// Check if the region's owner process is still alive.
    pub fn owner_alive(&self) -> bool {
        if self.base.is_null() {
            return false;
        }
        let hdr = self.base as *const RegionHeader;
        let pid = unsafe { (*hdr).owner_pid };
        if !pid_alive(pid) {
            return false;
        }
        // Verify generation matches to detect PID reuse.
        // Skip check if cached generation is 0 (legacy region).
        if self.owner_generation != 0 {
            let cur_gen = unsafe { (*hdr).owner_generation };
            if cur_gen != self.owner_generation {
                return false;
            }
        }
        true
    }

    /// Create a SHM region (server side).
    ///
    /// Creates `{run_dir}/{service_name}.ipcshm`, performs stale recovery.
    pub fn server_create(
        run_dir: &str,
        service_name: &str,
        req_capacity: u32,
        resp_capacity: u32,
    ) -> Result<Self, ShmError> {
        let path = build_shm_path(run_dir, service_name)?;

        // Stale recovery
        match check_shm_stale(&path) {
            StaleResult::LiveServer => return Err(ShmError::AddrInUse),
            _ => {}
        }

        // Round capacities to alignment
        let req_cap = align64(req_capacity);
        let resp_cap = align64(resp_capacity);

        let req_off = align64(HEADER_LEN as u32);
        let resp_off = align64(req_off + req_cap);
        let region_size = (resp_off + resp_cap) as usize;

        // Create the file
        let c_path = path_to_cstring(&path)?;
        let fd = unsafe {
            libc::open(
                c_path.as_ptr(),
                libc::O_RDWR | libc::O_CREAT | libc::O_TRUNC,
                0o600,
            )
        };
        if fd < 0 {
            return Err(ShmError::Open(errno()));
        }

        if unsafe { libc::ftruncate(fd, region_size as libc::off_t) } < 0 {
            let e = errno();
            unsafe {
                libc::close(fd);
                libc::unlink(c_path.as_ptr());
            }
            return Err(ShmError::Truncate(e));
        }

        let base = unsafe {
            libc::mmap(
                ptr::null_mut(),
                region_size,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                fd,
                0,
            )
        };
        if base == libc::MAP_FAILED {
            let e = errno();
            unsafe {
                libc::close(fd);
                libc::unlink(c_path.as_ptr());
            }
            return Err(ShmError::Mmap(e));
        }

        let base = base as *mut u8;

        // Zero the region
        unsafe { ptr::write_bytes(base, 0, region_size) };

        // Use a time-based generation to detect PID reuse across restarts.
        let generation = {
            let mut ts: libc::timespec = unsafe { std::mem::zeroed() };
            unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
            (ts.tv_sec as u32) ^ ((ts.tv_nsec >> 10) as u32)
        };

        // Write header
        let hdr = base as *mut RegionHeader;
        unsafe {
            (*hdr).magic = REGION_MAGIC;
            (*hdr).version = REGION_VERSION;
            (*hdr).header_len = HEADER_LEN;
            (*hdr).owner_pid = libc::getpid();
            (*hdr).owner_generation = generation;
            (*hdr).request_offset = req_off;
            (*hdr).request_capacity = req_cap;
            (*hdr).response_offset = resp_off;
            (*hdr).response_capacity = resp_cap;
        }

        // Release fence so clients see header writes
        std::sync::atomic::fence(std::sync::atomic::Ordering::Release);

        Ok(ShmContext {
            role: ShmRole::Server,
            fd,
            base,
            region_size,
            request_offset: req_off,
            request_capacity: req_cap,
            response_offset: resp_off,
            response_capacity: resp_cap,
            local_req_seq: 0,
            local_resp_seq: 0,
            spin_tries: DEFAULT_SPIN_TRIES,
            owner_generation: generation,
            path,
        })
    }

    /// Attach to an existing SHM region (client side).
    pub fn client_attach(run_dir: &str, service_name: &str) -> Result<Self, ShmError> {
        let path = build_shm_path(run_dir, service_name)?;
        let c_path = path_to_cstring(&path)?;

        let fd = unsafe { libc::open(c_path.as_ptr(), libc::O_RDWR) };
        if fd < 0 {
            return Err(ShmError::Open(errno()));
        }

        // Check file size
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::fstat(fd, &mut st) } < 0 {
            unsafe { libc::close(fd) };
            return Err(ShmError::Open(errno()));
        }

        let file_size = st.st_size as usize;
        if file_size < HEADER_LEN as usize {
            unsafe { libc::close(fd) };
            return Err(ShmError::NotReady);
        }

        let base = unsafe {
            libc::mmap(
                ptr::null_mut(),
                file_size,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                fd,
                0,
            )
        };
        if base == libc::MAP_FAILED {
            unsafe { libc::close(fd) };
            return Err(ShmError::Mmap(errno()));
        }

        let base = base as *mut u8;

        // Acquire fence to see server's header writes
        std::sync::atomic::fence(std::sync::atomic::Ordering::Acquire);

        let hdr = base as *const RegionHeader;
        let (magic, version, hdr_len) = unsafe {
            ((*hdr).magic, (*hdr).version, (*hdr).header_len)
        };

        if magic != REGION_MAGIC {
            unsafe {
                libc::munmap(base as *mut libc::c_void, file_size);
                libc::close(fd);
            }
            return Err(ShmError::BadMagic);
        }
        if version != REGION_VERSION {
            unsafe {
                libc::munmap(base as *mut libc::c_void, file_size);
                libc::close(fd);
            }
            return Err(ShmError::BadVersion);
        }
        if hdr_len != HEADER_LEN {
            unsafe {
                libc::munmap(base as *mut libc::c_void, file_size);
                libc::close(fd);
            }
            return Err(ShmError::BadHeader);
        }

        let (req_off, req_cap, resp_off, resp_cap) = unsafe {
            (
                (*hdr).request_offset,
                (*hdr).request_capacity,
                (*hdr).response_offset,
                (*hdr).response_capacity,
            )
        };

        // Validate region size
        let req_end = req_off as usize + req_cap as usize;
        let resp_end = resp_off as usize + resp_cap as usize;
        let needed = req_end.max(resp_end);
        if file_size < needed {
            unsafe {
                libc::munmap(base as *mut libc::c_void, file_size);
                libc::close(fd);
            }
            return Err(ShmError::BadSize);
        }

        // Read current sequence numbers
        let cur_req_seq = atomic_load_u64(base, OFF_REQ_SEQ);
        let cur_resp_seq = atomic_load_u64(base, OFF_RESP_SEQ);
        let generation = unsafe { (*hdr).owner_generation };

        Ok(ShmContext {
            role: ShmRole::Client,
            fd,
            base,
            region_size: file_size,
            request_offset: req_off,
            request_capacity: req_cap,
            response_offset: resp_off,
            response_capacity: resp_cap,
            local_req_seq: cur_req_seq,
            local_resp_seq: cur_resp_seq,
            spin_tries: DEFAULT_SPIN_TRIES,
            owner_generation: generation,
            path,
        })
    }

    /// Publish a message (client sends request, server sends response).
    ///
    /// The message must include the 32-byte outer header + payload,
    /// exactly as sent over UDS.
    pub fn send(&mut self, msg: &[u8]) -> Result<(), ShmError> {
        if self.base.is_null() || msg.is_empty() {
            return Err(ShmError::BadParam("null context or empty message".into()));
        }

        let (area_offset, area_capacity, seq_off, len_off, sig_off) = match self.role {
            ShmRole::Client => (
                self.request_offset,
                self.request_capacity,
                OFF_REQ_SEQ,
                OFF_REQ_LEN,
                OFF_REQ_SIGNAL,
            ),
            ShmRole::Server => (
                self.response_offset,
                self.response_capacity,
                OFF_RESP_SEQ,
                OFF_RESP_LEN,
                OFF_RESP_SIGNAL,
            ),
        };

        if msg.len() > area_capacity as usize {
            return Err(ShmError::MsgTooLarge);
        }

        // 1. Write message data into the area
        unsafe {
            ptr::copy_nonoverlapping(
                msg.as_ptr(),
                self.base.add(area_offset as usize),
                msg.len(),
            );
        }

        // 2. Store message length (release)
        atomic_store_u32(self.base, len_off, msg.len() as u32);

        // 3. Increment sequence number (release) to publish
        atomic_add_u64(self.base, seq_off, 1);

        // 4. Wake the peer via futex
        atomic_add_u32(self.base, sig_off, 1);
        futex_wake(unsafe { self.base.add(sig_off) as *mut u32 }, 1);

        // Track locally
        match self.role {
            ShmRole::Client => self.local_req_seq += 1,
            ShmRole::Server => self.local_resp_seq += 1,
        }

        Ok(())
    }

    /// Receive a message into the caller-provided buffer.
    ///
    /// On success, returns the number of bytes written to `buf`.
    /// Returns `MsgTooLarge` if the message exceeds `buf.len()`.
    pub fn receive(&mut self, buf: &mut [u8], timeout_ms: u32) -> Result<usize, ShmError> {
        if self.base.is_null() {
            return Err(ShmError::BadParam("null context".into()));
        }
        if buf.is_empty() {
            return Err(ShmError::BadParam("empty buffer".into()));
        }

        let (area_offset, seq_off, len_off, sig_off, expected_seq) = match self.role {
            ShmRole::Server => (
                self.request_offset,
                OFF_REQ_SEQ,
                OFF_REQ_LEN,
                OFF_REQ_SIGNAL,
                self.local_req_seq + 1,
            ),
            ShmRole::Client => (
                self.response_offset,
                OFF_RESP_SEQ,
                OFF_RESP_LEN,
                OFF_RESP_SIGNAL,
                self.local_resp_seq + 1,
            ),
        };

        // Phase 1: spin. Copy immediately on observing the advance.
        let mut observed = false;
        let mut mlen = 0usize;
        for _ in 0..self.spin_tries {
            let cur = atomic_load_u64(self.base, seq_off);
            if cur >= expected_seq {
                mlen = atomic_load_u32(self.base, len_off) as usize;
                if mlen > 0 && mlen <= buf.len() {
                    unsafe {
                        ptr::copy_nonoverlapping(
                            self.base.add(area_offset as usize),
                            buf.as_mut_ptr(),
                            mlen,
                        );
                    }
                }
                observed = true;
                break;
            }
            cpu_relax();
        }

        // Phase 2: futex wait with deadline-based retry loop.
        //
        // Handles spurious wakeups (EAGAIN when signal word changed
        // between read and syscall, or EINTR from signal delivery).
        // Computes a wall-clock deadline so total wait never exceeds
        // timeout_ms regardless of retries.
        if !observed {
            let deadline_ns: u64 = if timeout_ms > 0 {
                let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
                unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
                ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
                    + timeout_ms as u64 * 1_000_000
            } else {
                0
            };

            loop {
                let sig_val = atomic_load_u32(self.base, sig_off);

                let cur = atomic_load_u64(self.base, seq_off);
                if cur >= expected_seq {
                    break; // response arrived
                }

                // Compute remaining timeout for this futex_wait call
                let timeout = if deadline_ns > 0 {
                    let mut now_ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
                    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut now_ts) };
                    let now_val = now_ts.tv_sec as u64 * 1_000_000_000
                        + now_ts.tv_nsec as u64;
                    if now_val >= deadline_ns {
                        return Err(ShmError::Timeout);
                    }
                    let remain = deadline_ns - now_val;
                    Some(libc::timespec {
                        tv_sec: (remain / 1_000_000_000) as libc::time_t,
                        tv_nsec: (remain % 1_000_000_000) as libc::c_long,
                    })
                } else {
                    None
                };

                let ret = futex_wait(
                    unsafe { self.base.add(sig_off) as *mut u32 },
                    sig_val,
                    timeout.as_ref(),
                );

                if ret < 0 && errno() == libc::ETIMEDOUT {
                    return Err(ShmError::Timeout);
                }

                // EAGAIN (value changed) or EINTR (signal): re-check seq
            }

            // Copy immediately after observing the sequence advance
            mlen = atomic_load_u32(self.base, len_off) as usize;
            if mlen > 0 && mlen <= buf.len() {
                unsafe {
                    ptr::copy_nonoverlapping(
                        self.base.add(area_offset as usize),
                        buf.as_mut_ptr(),
                        mlen,
                    );
                }
            }
        }

        // Advance local tracking (message is consumed from SHM perspective)
        match self.role {
            ShmRole::Server => self.local_req_seq = expected_seq,
            ShmRole::Client => self.local_resp_seq = expected_seq,
        }

        // Message larger than caller buffer
        if mlen > buf.len() {
            return Err(ShmError::MsgTooLarge);
        }

        Ok(mlen)
    }

    /// Close client (munmap, close fd, no unlink).
    pub fn close(&mut self) {
        if !self.base.is_null() {
            unsafe { libc::munmap(self.base as *mut libc::c_void, self.region_size) };
            self.base = ptr::null_mut();
        }
        if self.fd >= 0 {
            unsafe { libc::close(self.fd) };
            self.fd = -1;
        }
        self.region_size = 0;
    }

    /// Destroy server (munmap, close, unlink).
    pub fn destroy(&mut self) {
        if !self.base.is_null() {
            unsafe { libc::munmap(self.base as *mut libc::c_void, self.region_size) };
            self.base = ptr::null_mut();
        }
        if self.fd >= 0 {
            unsafe { libc::close(self.fd) };
            self.fd = -1;
        }
        if !self.path.as_os_str().is_empty() {
            if let Ok(c) = std::ffi::CString::new(self.path.to_string_lossy().as_bytes()) {
                unsafe { libc::unlink(c.as_ptr()) };
            }
            self.path = PathBuf::new();
        }
        self.region_size = 0;
    }
}

impl Drop for ShmContext {
    fn drop(&mut self) {
        match self.role {
            ShmRole::Server => self.destroy(),
            ShmRole::Client => self.close(),
        }
    }
}

// ---------------------------------------------------------------------------
//  Internal helpers
// ---------------------------------------------------------------------------

fn align64(v: u32) -> u32 {
    (v + (REGION_ALIGNMENT - 1)) & !(REGION_ALIGNMENT - 1)
}

/// Validate service_name: only [a-zA-Z0-9._-], non-empty, not "." or "..".
fn validate_service_name(name: &str) -> Result<(), ShmError> {
    if name.is_empty() {
        return Err(ShmError::BadParam("empty service name".into()));
    }
    if name == "." || name == ".." {
        return Err(ShmError::BadParam("service name cannot be '.' or '..'".into()));
    }
    for c in name.bytes() {
        match c {
            b'a'..=b'z' | b'A'..=b'Z' | b'0'..=b'9' | b'.' | b'_' | b'-' => {}
            _ => return Err(ShmError::BadParam(
                format!("service name contains invalid character: {:?}", c as char),
            )),
        }
    }
    Ok(())
}

fn build_shm_path(run_dir: &str, service_name: &str) -> Result<PathBuf, ShmError> {
    validate_service_name(service_name)?;
    let path = Path::new(run_dir).join(format!("{service_name}.ipcshm"));
    if path.to_string_lossy().len() >= 256 {
        return Err(ShmError::PathTooLong);
    }
    Ok(path)
}

fn path_to_cstring(path: &Path) -> Result<std::ffi::CString, ShmError> {
    std::ffi::CString::new(path.to_string_lossy().as_bytes())
        .map_err(|_| ShmError::BadParam("path contains null byte".into()))
}

fn errno() -> i32 {
    unsafe { *libc::__errno_location() }
}

fn pid_alive(pid: i32) -> bool {
    if pid <= 0 {
        return false;
    }
    let ret = unsafe { libc::kill(pid, 0) };
    ret == 0 || errno() == libc::EPERM
}

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

// Atomic helpers: operate on raw pointers into the mmap'd region.
// These match the C implementation's __atomic builtins.

fn atomic_load_u64(base: *mut u8, offset: usize) -> u64 {
    let ptr = unsafe { base.add(offset) as *const std::sync::atomic::AtomicU64 };
    unsafe { (*ptr).load(std::sync::atomic::Ordering::Acquire) }
}

fn atomic_load_u32(base: *mut u8, offset: usize) -> u32 {
    let ptr = unsafe { base.add(offset) as *const std::sync::atomic::AtomicU32 };
    unsafe { (*ptr).load(std::sync::atomic::Ordering::Acquire) }
}

fn atomic_store_u32(base: *mut u8, offset: usize, val: u32) {
    let ptr = unsafe { base.add(offset) as *const std::sync::atomic::AtomicU32 };
    unsafe { (*ptr).store(val, std::sync::atomic::Ordering::Release) };
}

fn atomic_add_u64(base: *mut u8, offset: usize, val: u64) {
    let ptr = unsafe { base.add(offset) as *const std::sync::atomic::AtomicU64 };
    unsafe { (*ptr).fetch_add(val, std::sync::atomic::Ordering::Release) };
}

fn atomic_add_u32(base: *mut u8, offset: usize, val: u32) {
    let ptr = unsafe { base.add(offset) as *const std::sync::atomic::AtomicU32 };
    unsafe { (*ptr).fetch_add(val, std::sync::atomic::Ordering::Release) };
}

fn futex_wake(addr: *mut u32, count: i32) -> i32 {
    unsafe {
        libc::syscall(
            libc::SYS_futex,
            addr,
            FUTEX_WAKE,
            count,
            ptr::null::<libc::timespec>(),
            ptr::null::<u32>(),
            0i32,
        ) as i32
    }
}

fn futex_wait(addr: *mut u32, expected: u32, timeout: Option<&libc::timespec>) -> i32 {
    let tsp = match timeout {
        Some(ts) => ts as *const libc::timespec,
        None => ptr::null(),
    };
    unsafe {
        libc::syscall(
            libc::SYS_futex,
            addr,
            FUTEX_WAIT,
            expected,
            tsp,
            ptr::null::<u32>(),
            0i32,
        ) as i32
    }
}

// ---------------------------------------------------------------------------
//  Stale region recovery
// ---------------------------------------------------------------------------

enum StaleResult {
    NotExist,
    Recovered,
    LiveServer,
    Invalid,
}

fn check_shm_stale(path: &Path) -> StaleResult {
    let c_path = match path_to_cstring(path) {
        Ok(c) => c,
        Err(_) => return StaleResult::NotExist,
    };

    let mut st: libc::stat = unsafe { std::mem::zeroed() };
    if unsafe { libc::stat(c_path.as_ptr(), &mut st) } != 0 {
        return StaleResult::NotExist;
    }

    if (st.st_size as usize) < HEADER_LEN as usize {
        unsafe { libc::unlink(c_path.as_ptr()) };
        return StaleResult::Invalid;
    }

    let fd = unsafe { libc::open(c_path.as_ptr(), libc::O_RDONLY) };
    if fd < 0 {
        unsafe { libc::unlink(c_path.as_ptr()) };
        return StaleResult::Invalid;
    }

    let map = unsafe {
        libc::mmap(
            ptr::null_mut(),
            HEADER_LEN as usize,
            libc::PROT_READ,
            libc::MAP_SHARED,
            fd,
            0,
        )
    };
    unsafe { libc::close(fd) };

    if map == libc::MAP_FAILED {
        unsafe { libc::unlink(c_path.as_ptr()) };
        return StaleResult::Invalid;
    }

    let hdr = map as *const RegionHeader;
    let magic = unsafe { (*hdr).magic };
    if magic != REGION_MAGIC {
        unsafe {
            libc::munmap(map, HEADER_LEN as usize);
            libc::unlink(c_path.as_ptr());
        }
        return StaleResult::Invalid;
    }

    let owner = unsafe { (*hdr).owner_pid };
    let gen = unsafe { (*hdr).owner_generation };
    unsafe { libc::munmap(map, HEADER_LEN as usize) };

    if pid_alive(owner) && gen != 0 {
        return StaleResult::LiveServer;
    }

    // Dead owner or zero generation (PID reuse / legacy) — stale
    unsafe { libc::unlink(c_path.as_ptr()) };
    StaleResult::Recovered
}

// ---------------------------------------------------------------------------
//  Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol;
    use std::thread;

    const TEST_RUN_DIR: &str = "/tmp/nipc_shm_rust_test";

    fn ensure_run_dir() {
        let _ = std::fs::create_dir_all(TEST_RUN_DIR);
    }

    fn cleanup_shm(service: &str) {
        let path = format!("{TEST_RUN_DIR}/{service}.ipcshm");
        let _ = std::fs::remove_file(&path);
    }

    /// Build a complete message (outer header + payload) for SHM.
    fn build_message(kind: u16, code: u16, message_id: u64, payload: &[u8]) -> Vec<u8> {
        let hdr = protocol::Header {
            magic: protocol::MAGIC_MSG,
            version: protocol::VERSION,
            header_len: protocol::HEADER_LEN,
            kind,
            code,
            flags: 0,
            transport_status: protocol::STATUS_OK,
            payload_len: payload.len() as u32,
            item_count: 1,
            message_id,
        };
        let mut buf = vec![0u8; protocol::HEADER_SIZE + payload.len()];
        hdr.encode(&mut buf[..protocol::HEADER_SIZE]);
        buf[protocol::HEADER_SIZE..].copy_from_slice(payload);
        buf
    }

    #[test]
    fn test_direct_roundtrip() {
        ensure_run_dir();
        let svc = "rs_shm_rt";
        cleanup_shm(svc);

        let svc_clone = svc.to_string();
        let server_thread = thread::spawn(move || {
            let mut ctx = ShmContext::server_create(TEST_RUN_DIR, &svc_clone, 4096, 4096)
                .expect("server create");

            // Receive request
            let mut buf = vec![0u8; 65536];
            let mlen = ctx.receive(&mut buf, 5000).expect("server receive");
            let msg = &buf[..mlen];
            assert!(msg.len() >= protocol::HEADER_SIZE);

            // Parse and echo as response
            let hdr = protocol::Header::decode(msg).expect("decode");
            let payload = msg[protocol::HEADER_SIZE..].to_vec();
            let resp = build_message(
                protocol::KIND_RESPONSE,
                hdr.code,
                hdr.message_id,
                &payload,
            );
            ctx.send(&resp).expect("server send");
            ctx.destroy();
        });

        // Wait for server to create region
        thread::sleep(std::time::Duration::from_millis(50));

        let mut client =
            ShmContext::client_attach(TEST_RUN_DIR, svc).expect("client attach");

        let payload = vec![0xCA, 0xFE, 0xBA, 0xBE];
        let msg = build_message(
            protocol::KIND_REQUEST,
            protocol::METHOD_INCREMENT,
            42,
            &payload,
        );
        client.send(&msg).expect("client send");

        let mut resp_buf = vec![0u8; 65536];
        let rlen = client.receive(&mut resp_buf, 5000).expect("client receive");
        let resp = &resp_buf[..rlen];
        assert_eq!(resp.len(), protocol::HEADER_SIZE + payload.len());

        let rhdr = protocol::Header::decode(resp).expect("decode response");
        assert_eq!(rhdr.kind, protocol::KIND_RESPONSE);
        assert_eq!(rhdr.message_id, 42);
        assert_eq!(&resp[protocol::HEADER_SIZE..], &payload[..]);

        client.close();
        server_thread.join().unwrap();
        cleanup_shm(svc);
    }

    #[test]
    fn test_multiple_roundtrips() {
        ensure_run_dir();
        let svc = "rs_shm_multi";
        cleanup_shm(svc);

        let svc_clone = svc.to_string();
        let server_thread = thread::spawn(move || {
            let mut ctx = ShmContext::server_create(TEST_RUN_DIR, &svc_clone, 4096, 4096)
                .expect("server create");

            let mut buf = vec![0u8; 65536];
            for _ in 0..10 {
                let mlen = ctx.receive(&mut buf, 5000).expect("server receive");
                let msg = &buf[..mlen];
                let hdr = protocol::Header::decode(msg).expect("decode");
                let payload = msg[protocol::HEADER_SIZE..].to_vec();
                let resp = build_message(
                    protocol::KIND_RESPONSE,
                    hdr.code,
                    hdr.message_id,
                    &payload,
                );
                ctx.send(&resp).expect("server send");
            }
            ctx.destroy();
        });

        thread::sleep(std::time::Duration::from_millis(50));
        let mut client =
            ShmContext::client_attach(TEST_RUN_DIR, svc).expect("client attach");

        let mut resp_buf = vec![0u8; 65536];
        for i in 0u64..10 {
            let payload = vec![i as u8];
            let msg = build_message(
                protocol::KIND_REQUEST,
                1,
                i + 1,
                &payload,
            );
            client.send(&msg).expect("client send");
            let rlen = client.receive(&mut resp_buf, 5000).expect("client receive");
            let resp = &resp_buf[..rlen];
            let rhdr = protocol::Header::decode(resp).expect("decode");
            assert_eq!(rhdr.kind, protocol::KIND_RESPONSE);
            assert_eq!(rhdr.message_id, i + 1);
            assert_eq!(resp[protocol::HEADER_SIZE], i as u8);
        }

        client.close();
        server_thread.join().unwrap();
        cleanup_shm(svc);
    }

    #[test]
    fn test_stale_recovery() {
        ensure_run_dir();
        let svc = "rs_shm_stale";
        cleanup_shm(svc);

        // Create a region, corrupt owner_pid to simulate dead process
        let mut first = ShmContext::server_create(TEST_RUN_DIR, svc, 1024, 1024)
            .expect("first create");
        let hdr = first.base as *mut RegionHeader;
        unsafe { (*hdr).owner_pid = 99999 }; // very unlikely alive
        first.close(); // close without unlink

        // Should succeed via stale recovery
        let mut second = ShmContext::server_create(TEST_RUN_DIR, svc, 2048, 2048)
            .expect("stale recovery create");
        assert!(second.request_capacity >= 2048);
        second.destroy();
        cleanup_shm(svc);
    }

    #[test]
    fn test_large_message() {
        ensure_run_dir();
        let svc = "rs_shm_large";
        cleanup_shm(svc);

        let svc_clone = svc.to_string();
        let server_thread = thread::spawn(move || {
            let mut ctx = ShmContext::server_create(TEST_RUN_DIR, &svc_clone, 65536, 65536)
                .expect("server create");
            let mut buf = vec![0u8; 65536];
            let mlen = ctx.receive(&mut buf, 5000).expect("server receive");
            let msg = &buf[..mlen];
            let hdr = protocol::Header::decode(msg).expect("decode");
            let payload = msg[protocol::HEADER_SIZE..].to_vec();
            let resp = build_message(
                protocol::KIND_RESPONSE,
                hdr.code,
                hdr.message_id,
                &payload,
            );
            ctx.send(&resp).expect("server send");
            ctx.destroy();
        });

        thread::sleep(std::time::Duration::from_millis(50));
        let mut client =
            ShmContext::client_attach(TEST_RUN_DIR, svc).expect("client attach");

        // 60000 bytes of payload
        let payload: Vec<u8> = (0..60000).map(|i| (i & 0xFF) as u8).collect();
        let msg = build_message(protocol::KIND_REQUEST, 1, 999, &payload);
        client.send(&msg).expect("send large");

        let mut resp_buf = vec![0u8; 65536];
        let rlen = client.receive(&mut resp_buf, 5000).expect("receive large");
        let resp = &resp_buf[..rlen];
        assert_eq!(resp.len(), protocol::HEADER_SIZE + payload.len());
        assert_eq!(&resp[protocol::HEADER_SIZE..], &payload[..]);

        client.close();
        server_thread.join().unwrap();
        cleanup_shm(svc);
    }
}
