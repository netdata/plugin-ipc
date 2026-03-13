//! POSIX transport implementation for the Rust crate.

use crate::protocol::{
    decode_hello_ack_payload, decode_hello_payload, decode_increment_request,
    decode_increment_response, decode_message_header, encode_hello_ack_payload,
    encode_hello_payload, encode_increment_request, encode_increment_response,
    max_batch_total_size, message_total_size, Frame, HelloAckPayload, HelloPayload,
    IncrementRequest, IncrementResponse, CONTROL_HELLO_ACK_PAYLOAD_LEN, CONTROL_HELLO_PAYLOAD_LEN,
    FRAME_SIZE, MAX_PAYLOAD_DEFAULT, MESSAGE_VERSION,
};
use std::fs::{self, OpenOptions};
use std::io;
use std::mem::size_of;
use std::os::fd::{AsRawFd, RawFd};
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::thread;
use std::time::{Duration, Instant};

pub const PROFILE_UDS_SEQPACKET: u32 = 1u32 << 0;
pub const PROFILE_SHM_HYBRID: u32 = 1u32 << 1;
pub const PROFILE_SHM_FUTEX: u32 = 1u32 << 2;

#[cfg(target_os = "linux")]
const IMPLEMENTED_PROFILES: u32 = PROFILE_UDS_SEQPACKET | PROFILE_SHM_HYBRID;
#[cfg(not(target_os = "linux"))]
const IMPLEMENTED_PROFILES: u32 = PROFILE_UDS_SEQPACKET;
const DEFAULT_SUPPORTED_PROFILES: u32 = PROFILE_UDS_SEQPACKET;
const DEFAULT_PREFERRED_PROFILES: u32 = PROFILE_UDS_SEQPACKET;

const NEGOTIATION_MAGIC: u32 = 0x4e48_534b;
const NEGOTIATION_VERSION: u16 = 1;
const NEGOTIATION_HELLO: u16 = 1;
const NEGOTIATION_ACK: u16 = 2;
const NEGOTIATION_STATUS_OK: u32 = 0;
const NEGOTIATION_FRAME_SIZE: usize = FRAME_SIZE;
const NEGOTIATION_PAYLOAD_OFFSET: usize = 8;
const NEGOTIATION_STATUS_OFFSET: usize = 48;
const NEGOTIATION_DEFAULT_BATCH_ITEMS: u32 = 1;

const SHM_REGION_MAGIC: u32 = 0x4e53_484d;
const SHM_REGION_VERSION: u16 = 3;
const SHM_REGION_ALIGNMENT: usize = 64;
pub const SHM_DEFAULT_SPIN_TRIES: u32 = 128;

#[repr(C)]
struct RegionHeader {
    magic: u32,
    version: u16,
    header_len: u16,
    owner_pid: i32,
    owner_generation: u32,
    request_offset: u32,
    request_capacity: u32,
    response_offset: u32,
    response_capacity: u32,
    req_seq: AtomicU64,
    resp_seq: AtomicU64,
    req_len: AtomicU32,
    resp_len: AtomicU32,
    req_signal: AtomicU32,
    resp_signal: AtomicU32,
}

const REGION_HEADER_LEN: usize = size_of::<RegionHeader>();

#[derive(Clone, Debug)]
pub struct UdsSeqpacketConfig {
    pub run_dir: PathBuf,
    pub service_name: String,
    pub file_mode: u32,
    pub supported_profiles: u32,
    pub preferred_profiles: u32,
    pub max_request_payload_bytes: u32,
    pub max_request_batch_items: u32,
    pub max_response_payload_bytes: u32,
    pub max_response_batch_items: u32,
    pub auth_token: u64,
}

impl UdsSeqpacketConfig {
    pub fn new(run_dir: impl Into<PathBuf>, service_name: impl Into<String>) -> Self {
        Self {
            run_dir: run_dir.into(),
            service_name: service_name.into(),
            file_mode: 0o600,
            supported_profiles: DEFAULT_SUPPORTED_PROFILES,
            preferred_profiles: DEFAULT_PREFERRED_PROFILES,
            max_request_payload_bytes: MAX_PAYLOAD_DEFAULT,
            max_request_batch_items: NEGOTIATION_DEFAULT_BATCH_ITEMS,
            max_response_payload_bytes: MAX_PAYLOAD_DEFAULT,
            max_response_batch_items: NEGOTIATION_DEFAULT_BATCH_ITEMS,
            auth_token: 0,
        }
    }

    pub fn endpoint_path(&self) -> PathBuf {
        let mut path = self.run_dir.clone();
        path.push(format!("{}.sock", self.service_name));
        path
    }
}

#[derive(Debug)]
pub struct UdsSeqpacketServer {
    listen_fd: RawFd,
    conn_fd: Option<RawFd>,
    endpoint_path: PathBuf,
    shm_config: ShmConfig,
    shm_server: Option<ShmServer>,
    supported_profiles: u32,
    preferred_profiles: u32,
    max_request_payload_bytes: u32,
    max_request_batch_items: u32,
    max_response_payload_bytes: u32,
    max_response_batch_items: u32,
    auth_token: u64,
    negotiated_profile: u32,
    max_request_message_len: usize,
    max_response_message_len: usize,
}

#[derive(Debug)]
pub struct UdsSeqpacketClient {
    fd: RawFd,
    shm_client: Option<ShmClient>,
    supported_profiles: u32,
    preferred_profiles: u32,
    auth_token: u64,
    negotiated_profile: u32,
    max_request_message_len: usize,
    max_response_message_len: usize,
    next_request_id: u64,
}

#[derive(Clone, Copy, Debug)]
struct NegotiationResult {
    profile: u32,
    max_request_message_len: usize,
    max_response_message_len: usize,
}

#[derive(Clone, Debug)]
pub struct ShmConfig {
    pub run_dir: PathBuf,
    pub service_name: String,
    pub spin_tries: u32,
    pub file_mode: u32,
    pub max_request_message_bytes: u32,
    pub max_response_message_bytes: u32,
}

impl ShmConfig {
    pub fn new(run_dir: impl Into<PathBuf>, service_name: impl Into<String>) -> Self {
        Self {
            run_dir: run_dir.into(),
            service_name: service_name.into(),
            spin_tries: SHM_DEFAULT_SPIN_TRIES,
            file_mode: 0o600,
            max_request_message_bytes: FRAME_SIZE as u32,
            max_response_message_bytes: FRAME_SIZE as u32,
        }
    }

    pub fn endpoint_path(&self) -> PathBuf {
        let mut path = self.run_dir.clone();
        path.push(format!("{}.ipcshm", self.service_name));
        path
    }
}

#[derive(Debug)]
struct MappedRegion {
    fd: RawFd,
    ptr: *mut u8,
    mapping_len: usize,
    path: PathBuf,
    unlink_on_drop: bool,
    is_server: bool,
}

#[derive(Debug)]
pub struct ShmServer {
    region: MappedRegion,
    spin_tries: u32,
    last_request_seq: u64,
    last_response_seq: u64,
    max_request_message_len: usize,
    max_response_message_len: usize,
}

#[derive(Debug)]
pub struct ShmClient {
    region: MappedRegion,
    spin_tries: u32,
    next_request_seq: u64,
    max_request_message_len: usize,
    max_response_message_len: usize,
}

impl Drop for UdsSeqpacketServer {
    fn drop(&mut self) {
        if let Some(fd) = self.conn_fd.take() {
            unsafe {
                libc::close(fd);
            }
        }
        if self.listen_fd >= 0 {
            unsafe {
                libc::close(self.listen_fd);
            }
        }
        let _ = fs::remove_file(&self.endpoint_path);
    }
}

impl Drop for UdsSeqpacketClient {
    fn drop(&mut self) {
        if self.fd >= 0 {
            unsafe {
                libc::close(self.fd);
            }
        }
    }
}

impl MappedRegion {
    fn map(
        fd: RawFd,
        mapping_len: usize,
        path: PathBuf,
        unlink_on_drop: bool,
        is_server: bool,
    ) -> io::Result<Self> {
        let ptr = unsafe {
            libc::mmap(
                ptr::null_mut(),
                mapping_len,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                fd,
                0,
            )
        };
        if ptr == libc::MAP_FAILED {
            return Err(io::Error::last_os_error());
        }

        Ok(Self {
            fd,
            ptr: ptr as *mut u8,
            mapping_len,
            path,
            unlink_on_drop,
            is_server,
        })
    }

    fn header(&self) -> &RegionHeader {
        unsafe { &*(self.ptr as *const RegionHeader) }
    }

    fn owner_pid(&self) -> i32 {
        self.header().owner_pid
    }

    fn set_owner_pid(&self, pid: i32) {
        unsafe {
            (*(self.ptr as *mut RegionHeader)).owner_pid = pid;
        }
    }

    fn load_req_seq(&self) -> u64 {
        self.header().req_seq.load(Ordering::Acquire)
    }

    fn store_req_seq(&self, value: u64) {
        self.header().req_seq.store(value, Ordering::Release);
    }

    fn store_resp_seq(&self, value: u64) {
        self.header().resp_seq.store(value, Ordering::Release);
    }

    fn load_req_len(&self) -> u32 {
        self.header().req_len.load(Ordering::Acquire)
    }

    fn store_req_len(&self, value: u32) {
        self.header().req_len.store(value, Ordering::Release);
    }

    fn load_resp_len(&self) -> u32 {
        self.header().resp_len.load(Ordering::Acquire)
    }

    fn store_resp_len(&self, value: u32) {
        self.header().resp_len.store(value, Ordering::Release);
    }

    fn req_signal(&self) -> &AtomicU32 {
        &self.header().req_signal
    }

    fn resp_signal(&self) -> &AtomicU32 {
        &self.header().resp_signal
    }

    fn request_capacity(&self) -> usize {
        self.header().request_capacity as usize
    }

    fn response_capacity(&self) -> usize {
        self.header().response_capacity as usize
    }

    fn request_area_ptr(&self) -> *mut u8 {
        unsafe { self.ptr.add(self.header().request_offset as usize) }
    }

    fn response_area_ptr(&self) -> *mut u8 {
        unsafe { self.ptr.add(self.header().response_offset as usize) }
    }

    fn read_request_bytes(&self, len: usize, dst: &mut [u8]) {
        unsafe {
            ptr::copy_nonoverlapping(self.request_area_ptr(), dst.as_mut_ptr(), len);
        }
    }

    fn write_request_bytes(&self, src: &[u8]) {
        unsafe {
            ptr::copy_nonoverlapping(src.as_ptr(), self.request_area_ptr(), src.len());
        }
    }

    fn read_response_bytes(&self, len: usize, dst: &mut [u8]) {
        unsafe {
            ptr::copy_nonoverlapping(self.response_area_ptr(), dst.as_mut_ptr(), len);
        }
    }

    fn write_response_bytes(&self, src: &[u8]) {
        unsafe {
            ptr::copy_nonoverlapping(src.as_ptr(), self.response_area_ptr(), src.len());
        }
    }
}

impl Drop for MappedRegion {
    fn drop(&mut self) {
        if self.is_server {
            self.set_owner_pid(0);
        }

        unsafe {
            libc::munmap(self.ptr as *mut libc::c_void, self.mapping_len);
            libc::close(self.fd);
        }

        if self.unlink_on_drop {
            let _ = fs::remove_file(&self.path);
        }
    }
}

fn invalid_input(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}

fn config_is_valid(config: &UdsSeqpacketConfig) -> bool {
    !config.service_name.is_empty() && !config.run_dir.as_os_str().is_empty()
}

fn shm_config_is_valid(config: &ShmConfig) -> bool {
    !config.service_name.is_empty() && !config.run_dir.as_os_str().is_empty()
}

fn effective_mode(config: &UdsSeqpacketConfig) -> u32 {
    if config.file_mode == 0 {
        0o600
    } else {
        config.file_mode
    }
}

fn effective_shm_mode(config: &ShmConfig) -> u32 {
    if config.file_mode == 0 {
        0o600
    } else {
        config.file_mode
    }
}

fn effective_spin_tries(config: &ShmConfig) -> u32 {
    if config.spin_tries == 0 {
        SHM_DEFAULT_SPIN_TRIES
    } else {
        config.spin_tries
    }
}

fn effective_request_message_len(config: &ShmConfig) -> usize {
    if config.max_request_message_bytes != 0 {
        config.max_request_message_bytes as usize
    } else {
        FRAME_SIZE
    }
}

fn effective_response_message_len(config: &ShmConfig) -> usize {
    if config.max_response_message_bytes != 0 {
        config.max_response_message_bytes as usize
    } else {
        FRAME_SIZE
    }
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
) -> io::Result<(u32, u32, usize)> {
    if request_capacity == 0 || response_capacity == 0 {
        return Err(invalid_input(
            "request and response capacities must be non-zero",
        ));
    }

    let request_offset = align_up_size(REGION_HEADER_LEN, SHM_REGION_ALIGNMENT);
    let response_offset = align_up_size(request_offset + request_capacity, SHM_REGION_ALIGNMENT);
    let mapping_len = response_offset + response_capacity;

    if request_offset > u32::MAX as usize || response_offset > u32::MAX as usize {
        return Err(io::Error::from_raw_os_error(libc::EOVERFLOW));
    }

    Ok((request_offset as u32, response_offset as u32, mapping_len))
}

fn set_endpoint_lock(fd: RawFd, lock_type: libc::c_short) -> io::Result<()> {
    let mut lock: libc::flock = unsafe { std::mem::zeroed() };
    lock.l_type = lock_type;
    lock.l_whence = libc::SEEK_SET as libc::c_short;
    lock.l_start = 0;
    lock.l_len = 0;

    let rc = unsafe { libc::fcntl(fd, libc::F_SETLK, &lock) };
    if rc == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

fn lock_endpoint_fd(fd: RawFd) -> io::Result<()> {
    set_endpoint_lock(fd, libc::F_WRLCK as libc::c_short)
}

fn unlock_endpoint_fd(fd: RawFd) {
    let _ = set_endpoint_lock(fd, libc::F_UNLCK as libc::c_short);
}

fn endpoint_owned_by_live_server(fd: RawFd, owner_pid: i32) -> io::Result<bool> {
    if fd < 0 {
        return Err(invalid_input("fd must be non-negative"));
    }

    if owner_pid == unsafe { libc::getpid() } {
        return Ok(true);
    }

    match lock_endpoint_fd(fd) {
        Ok(()) => {
            unlock_endpoint_fd(fd);
            Ok(false)
        }
        Err(err) => match err.raw_os_error() {
            Some(code) if code == libc::EACCES || code == libc::EAGAIN => Ok(true),
            _ => Err(err),
        },
    }
}

fn signal_wait(signal: &AtomicU32, expected: u32, timeout: Option<Duration>) -> io::Result<()> {
    #[cfg(target_os = "linux")]
    {
        let timeout_storage = timeout.map(|duration| libc::timespec {
            tv_sec: duration.as_secs() as libc::time_t,
            tv_nsec: duration.subsec_nanos() as libc::c_long,
        });
        let timeout_ptr = timeout_storage
            .as_ref()
            .map(|ts| ts as *const libc::timespec)
            .unwrap_or(ptr::null());

        loop {
            let rc = unsafe {
                libc::syscall(
                    libc::SYS_futex as libc::c_long,
                    signal as *const AtomicU32 as *mut u32,
                    libc::FUTEX_WAIT,
                    expected,
                    timeout_ptr,
                    0,
                    0,
                )
            };
            if rc == 0 {
                return Ok(());
            }
            let err = io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if err != libc::EINTR {
                return Err(io::Error::from_raw_os_error(err));
            }
        }
    }

    #[cfg(not(target_os = "linux"))]
    {
        let _ = signal;
        let _ = expected;
        if let Some(duration) = timeout {
            if duration.is_zero() {
                return Err(io::Error::from_raw_os_error(libc::ETIMEDOUT));
            }
        }
        thread::sleep(Duration::from_micros(50));
        Err(io::Error::from_raw_os_error(libc::EAGAIN))
    }
}

fn signal_wake(signal: &AtomicU32) {
    signal.fetch_add(1, Ordering::Release);

    #[cfg(target_os = "linux")]
    unsafe {
        let _ = libc::syscall(
            libc::SYS_futex as libc::c_long,
            signal as *const AtomicU32 as *mut u32,
            libc::FUTEX_WAKE,
            1,
            0,
            0,
            0,
        );
    }
}

fn wait_for_sequence(
    seq: &AtomicU64,
    target: u64,
    signal: &AtomicU32,
    spin_tries: u32,
    timeout_ms: u32,
) -> io::Result<()> {
    let deadline = if timeout_ms > 0 {
        Some(Instant::now() + Duration::from_millis(timeout_ms as u64))
    } else {
        None
    };

    loop {
        if seq.load(Ordering::Acquire) >= target {
            return Ok(());
        }

        for _ in 0..spin_tries {
            std::hint::spin_loop();
            if seq.load(Ordering::Acquire) >= target {
                return Ok(());
            }
        }

        if let Some(deadline_at) = deadline {
            if Instant::now() >= deadline_at {
                return Err(io::Error::from_raw_os_error(libc::ETIMEDOUT));
            }
        }

        let expected = signal.load(Ordering::Acquire);
        if seq.load(Ordering::Acquire) >= target {
            return Ok(());
        }

        let wait_for = deadline.map(|deadline_at| {
            deadline_at
                .saturating_duration_since(Instant::now())
                .min(Duration::from_millis(timeout_ms as u64))
        });

        if let Err(err) = signal_wait(signal, expected, wait_for) {
            match err.raw_os_error() {
                Some(code) if code == libc::ETIMEDOUT || code == libc::EAGAIN => continue,
                _ => return Err(err),
            }
        }
    }
}

fn timeout_to_ms(timeout: Option<Duration>) -> u32 {
    timeout
        .map(|duration| duration.as_millis().min(u32::MAX as u128) as u32)
        .unwrap_or(0)
}

fn shm_config_from_uds(
    config: &UdsSeqpacketConfig,
    max_request_message_len: usize,
    max_response_message_len: usize,
) -> ShmConfig {
    ShmConfig {
        run_dir: config.run_dir.clone(),
        service_name: config.service_name.clone(),
        spin_tries: SHM_DEFAULT_SPIN_TRIES,
        file_mode: 0,
        max_request_message_bytes: max_request_message_len as u32,
        max_response_message_bytes: max_response_message_len as u32,
    }
}

fn create_shm_client_with_retry(
    config: &ShmConfig,
    timeout: Option<Duration>,
) -> io::Result<ShmClient> {
    let deadline = timeout.map(|duration| Instant::now() + duration);

    loop {
        match ShmClient::connect(config) {
            Ok(client) => return Ok(client),
            Err(err) => match err.raw_os_error() {
                Some(code)
                    if code == libc::ENOENT
                        || code == libc::ECONNREFUSED
                        || code == libc::EPROTO =>
                {
                    if let Some(limit) = deadline {
                        if Instant::now() >= limit {
                            return Err(io::Error::from_raw_os_error(libc::ETIMEDOUT));
                        }
                    }
                    thread::sleep(Duration::from_micros(500));
                }
                _ => return Err(err),
            },
        }
    }
}

fn effective_supported_profiles(config: &UdsSeqpacketConfig) -> u32 {
    let mut supported = if config.supported_profiles == 0 {
        DEFAULT_SUPPORTED_PROFILES
    } else {
        config.supported_profiles
    } & IMPLEMENTED_PROFILES;

    if supported == 0 {
        supported = DEFAULT_SUPPORTED_PROFILES;
    }
    supported
}

fn effective_preferred_profiles(config: &UdsSeqpacketConfig, supported: u32) -> u32 {
    let mut preferred = if config.preferred_profiles == 0 {
        supported
    } else {
        config.preferred_profiles
    } & supported;

    if preferred == 0 {
        preferred = supported;
    }
    preferred
}

fn select_profile(mask: u32) -> u32 {
    if (mask & PROFILE_UDS_SEQPACKET) != 0 {
        return PROFILE_UDS_SEQPACKET;
    }
    if (mask & PROFILE_SHM_HYBRID) != 0 {
        return PROFILE_SHM_HYBRID;
    }
    if (mask & PROFILE_SHM_FUTEX) != 0 {
        return PROFILE_SHM_FUTEX;
    }
    0
}

fn set_cloexec(fd: RawFd) -> io::Result<()> {
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFD, 0) };
    if flags < 0 {
        return Err(io::Error::last_os_error());
    }
    if (flags & libc::FD_CLOEXEC) != 0 {
        return Ok(());
    }
    let rc = unsafe { libc::fcntl(fd, libc::F_SETFD, flags | libc::FD_CLOEXEC) };
    if rc != 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

fn create_seqpacket_socket() -> io::Result<RawFd> {
    let fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET | libc::SOCK_CLOEXEC, 0) };
    if fd >= 0 {
        return Ok(fd);
    }

    let err = io::Error::last_os_error();
    let code = err.raw_os_error().unwrap_or(0);
    if code != libc::EINVAL && code != libc::ENOSYS {
        return Err(err);
    }

    let fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET, 0) };
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }
    set_cloexec(fd)?;
    Ok(fd)
}

fn make_sockaddr(path: &Path) -> io::Result<(libc::sockaddr_un, libc::socklen_t)> {
    let path_bytes = path.as_os_str().as_encoded_bytes();
    let mut addr: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    if path_bytes.len() + 1 > addr.sun_path.len() {
        return Err(io::Error::from_raw_os_error(libc::ENAMETOOLONG));
    }

    addr.sun_family = libc::AF_UNIX as libc::sa_family_t;
    for (idx, byte) in path_bytes.iter().enumerate() {
        addr.sun_path[idx] = *byte as libc::c_char;
    }
    addr.sun_path[path_bytes.len()] = 0;

    let len = (std::mem::offset_of!(libc::sockaddr_un, sun_path) + path_bytes.len() + 1)
        as libc::socklen_t;
    Ok((addr, len))
}

fn connect_with_timeout(fd: RawFd, path: &Path, _timeout: Option<Duration>) -> io::Result<()> {
    let (addr, addr_len) = make_sockaddr(path)?;
    let rc = unsafe { libc::connect(fd, &addr as *const _ as *const libc::sockaddr, addr_len) };
    if rc != 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

fn send_exact(fd: RawFd, buf: &[u8], _timeout: Option<Duration>) -> io::Result<()> {
    if buf.is_empty() {
        return Err(invalid_input("buffer must not be empty"));
    }

    let rc = unsafe {
        libc::send(
            fd,
            buf.as_ptr() as *const libc::c_void,
            buf.len(),
            libc::MSG_NOSIGNAL,
        )
    };
    if rc < 0 {
        return Err(io::Error::last_os_error());
    }
    if rc as usize != buf.len() {
        return Err(io::Error::from_raw_os_error(libc::EPROTO));
    }
    Ok(())
}

fn recv_exact(fd: RawFd, buf: &mut [u8], _timeout: Option<Duration>) -> io::Result<()> {
    if buf.is_empty() {
        return Err(invalid_input("buffer must not be empty"));
    }

    let rc = unsafe { libc::recv(fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len(), 0) };
    if rc < 0 {
        return Err(io::Error::last_os_error());
    }
    if rc as usize != buf.len() {
        return Err(io::Error::from_raw_os_error(libc::EPROTO));
    }
    Ok(())
}

fn send_seqpacket_message(fd: RawFd, buf: &[u8], _timeout: Option<Duration>) -> io::Result<()> {
    if buf.is_empty() {
        return Err(invalid_input("buffer must not be empty"));
    }

    let rc = unsafe {
        libc::send(
            fd,
            buf.as_ptr() as *const libc::c_void,
            buf.len(),
            libc::MSG_NOSIGNAL,
        )
    };
    if rc < 0 {
        return Err(io::Error::last_os_error());
    }
    if rc as usize != buf.len() {
        return Err(io::Error::from_raw_os_error(libc::EPROTO));
    }
    Ok(())
}

fn recv_seqpacket_message(
    fd: RawFd,
    buf: &mut [u8],
    _timeout: Option<Duration>,
) -> io::Result<usize> {
    if buf.is_empty() {
        return Err(invalid_input("buffer must not be empty"));
    }

    let mut iov = libc::iovec {
        iov_base: buf.as_mut_ptr() as *mut libc::c_void,
        iov_len: buf.len(),
    };
    let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
    msg.msg_iov = &mut iov;
    msg.msg_iovlen = 1;

    let rc = unsafe { libc::recvmsg(fd, &mut msg, 0) };
    if rc < 0 {
        return Err(io::Error::last_os_error());
    }
    if (msg.msg_flags & libc::MSG_TRUNC) != 0 {
        return Err(io::Error::from_raw_os_error(libc::EMSGSIZE));
    }
    if rc == 0 {
        return Err(io::Error::from_raw_os_error(libc::ECONNRESET));
    }
    Ok(rc as usize)
}

fn validate_message_for_send(message: &[u8], max_message_len: usize) -> io::Result<()> {
    if message.is_empty() {
        return Err(invalid_input("message must not be empty"));
    }
    if message.len() > max_message_len {
        return Err(io::Error::from_raw_os_error(libc::EMSGSIZE));
    }
    let header = decode_message_header(message)?;
    let total = message_total_size(&header)?;
    if total != message.len() {
        return Err(io::Error::from_raw_os_error(libc::EPROTO));
    }
    Ok(())
}

fn validate_received_message(
    message: &[u8],
    message_len: usize,
    max_message_len: usize,
) -> io::Result<()> {
    if message_len == 0 {
        return Err(invalid_input("message must not be empty"));
    }
    if message_len > max_message_len {
        return Err(io::Error::from_raw_os_error(libc::EMSGSIZE));
    }
    let header = decode_message_header(&message[..message_len])?;
    let total = message_total_size(&header)?;
    if total != message_len {
        return Err(io::Error::from_raw_os_error(libc::EPROTO));
    }
    Ok(())
}

fn write_u16_le(frame: &mut Frame, off: usize, value: u16) {
    frame[off..off + 2].copy_from_slice(&value.to_le_bytes());
}

fn write_u32_le(frame: &mut Frame, off: usize, value: u32) {
    frame[off..off + 4].copy_from_slice(&value.to_le_bytes());
}

fn read_u16_le(frame: &Frame, off: usize) -> u16 {
    u16::from_le_bytes([frame[off], frame[off + 1]])
}

fn read_u32_le(frame: &Frame, off: usize) -> u32 {
    u32::from_le_bytes([frame[off], frame[off + 1], frame[off + 2], frame[off + 3]])
}

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

fn encode_negotiation_header(ty: u16) -> Frame {
    let mut frame = [0u8; NEGOTIATION_FRAME_SIZE];
    write_u32_le(&mut frame, 0, NEGOTIATION_MAGIC);
    write_u16_le(&mut frame, 4, NEGOTIATION_VERSION);
    write_u16_le(&mut frame, 6, ty);
    frame
}

fn validate_negotiation_header(frame: &Frame, expected_type: u16) -> io::Result<()> {
    let magic = read_u32_le(frame, 0);
    let version = read_u16_le(frame, 4);
    let ty = read_u16_le(frame, 6);
    if magic != NEGOTIATION_MAGIC || version != NEGOTIATION_VERSION || ty != expected_type {
        return Err(io::Error::from_raw_os_error(libc::EPROTO));
    }
    Ok(())
}

fn write_hello_negotiation(
    fd: RawFd,
    payload: &HelloPayload,
    timeout: Option<Duration>,
) -> io::Result<()> {
    let mut frame = encode_negotiation_header(NEGOTIATION_HELLO);
    frame[NEGOTIATION_PAYLOAD_OFFSET..NEGOTIATION_PAYLOAD_OFFSET + CONTROL_HELLO_PAYLOAD_LEN]
        .copy_from_slice(&encode_hello_payload(payload));
    send_exact(fd, &frame, timeout)
}

fn write_ack_negotiation(
    fd: RawFd,
    payload: &HelloAckPayload,
    status: u32,
    timeout: Option<Duration>,
) -> io::Result<()> {
    let mut frame = encode_negotiation_header(NEGOTIATION_ACK);
    frame[NEGOTIATION_PAYLOAD_OFFSET..NEGOTIATION_PAYLOAD_OFFSET + CONTROL_HELLO_ACK_PAYLOAD_LEN]
        .copy_from_slice(&encode_hello_ack_payload(payload));
    write_u32_le(&mut frame, NEGOTIATION_STATUS_OFFSET, status);
    send_exact(fd, &frame, timeout)
}

fn read_hello_negotiation(fd: RawFd, timeout: Option<Duration>) -> io::Result<HelloPayload> {
    let mut frame = [0u8; NEGOTIATION_FRAME_SIZE];
    recv_exact(fd, &mut frame, timeout)?;
    validate_negotiation_header(&frame, NEGOTIATION_HELLO)?;
    decode_hello_payload(
        &frame[NEGOTIATION_PAYLOAD_OFFSET..NEGOTIATION_PAYLOAD_OFFSET + CONTROL_HELLO_PAYLOAD_LEN],
    )
}

fn read_ack_negotiation(
    fd: RawFd,
    timeout: Option<Duration>,
) -> io::Result<(HelloAckPayload, u32)> {
    let mut frame = [0u8; NEGOTIATION_FRAME_SIZE];
    recv_exact(fd, &mut frame, timeout)?;
    validate_negotiation_header(&frame, NEGOTIATION_ACK)?;
    let payload = decode_hello_ack_payload(
        &frame[NEGOTIATION_PAYLOAD_OFFSET
            ..NEGOTIATION_PAYLOAD_OFFSET + CONTROL_HELLO_ACK_PAYLOAD_LEN],
    )?;
    Ok((payload, read_u32_le(&frame, NEGOTIATION_STATUS_OFFSET)))
}

fn try_takeover_stale_socket(path: &Path) -> io::Result<bool> {
    let fd = create_seqpacket_socket()?;
    let result = (|| -> io::Result<bool> {
        let (addr, addr_len) = make_sockaddr(path)?;
        let rc = unsafe { libc::connect(fd, &addr as *const _ as *const libc::sockaddr, addr_len) };
        if rc == 0 {
            return Err(io::Error::from_raw_os_error(libc::EADDRINUSE));
        }

        match io::Error::last_os_error().raw_os_error() {
            Some(libc::ECONNREFUSED)
            | Some(libc::ENOENT)
            | Some(libc::ENOTSOCK)
            | Some(libc::ECONNRESET) => match fs::remove_file(path) {
                Ok(()) => Ok(true),
                Err(err) if err.raw_os_error() == Some(libc::ENOENT) => Ok(true),
                Err(err) => Err(err),
            },
            Some(code) => Err(io::Error::from_raw_os_error(code)),
            None => Err(io::Error::new(io::ErrorKind::Other, "connect failed")),
        }
    })();

    unsafe {
        libc::close(fd);
    }
    result
}

fn try_takeover_stale_shm_endpoint(path: &Path) -> io::Result<bool> {
    let file = OpenOptions::new().read(true).write(true).open(path)?;
    let meta = file.metadata()?;
    if meta.len() < REGION_HEADER_LEN as u64 {
        fs::remove_file(path)?;
        return Ok(true);
    }

    let dup_fd = unsafe { libc::dup(file.as_raw_fd()) };
    if dup_fd < 0 {
        return Err(io::Error::last_os_error());
    }

    let mapped = MappedRegion::map(
        dup_fd,
        meta.len() as usize,
        path.to_path_buf(),
        false,
        false,
    )?;
    let owner_state = if validate_region_header(mapped.header(), mapped.mapping_len).is_ok() {
        Some(endpoint_owned_by_live_server(
            mapped.fd,
            mapped.owner_pid(),
        )?)
    } else {
        None
    };
    drop(mapped);
    drop(file);

    if owner_state == Some(true) {
        return Err(io::Error::from_raw_os_error(libc::EADDRINUSE));
    }

    fs::remove_file(path)?;
    Ok(true)
}

fn validate_region_header(header: &RegionHeader, mapping_len: usize) -> io::Result<()> {
    if header.magic != SHM_REGION_MAGIC
        || header.version != SHM_REGION_VERSION
        || header.header_len as usize != REGION_HEADER_LEN
        || header.request_capacity == 0
        || header.response_capacity == 0
    {
        return Err(io::Error::from_raw_os_error(libc::EPROTO));
    }

    if header.request_offset < header.header_len as u32 {
        return Err(io::Error::from_raw_os_error(libc::EPROTO));
    }

    if header.response_offset
        < header
            .request_offset
            .saturating_add(header.request_capacity)
    {
        return Err(io::Error::from_raw_os_error(libc::EPROTO));
    }

    let required_len = header.response_offset as usize + header.response_capacity as usize;
    if required_len > mapping_len {
        return Err(io::Error::from_raw_os_error(libc::EPROTO));
    }

    Ok(())
}

fn create_server_region(config: &ShmConfig) -> io::Result<MappedRegion> {
    let path = config.endpoint_path();
    let mut fd = -1;
    let request_capacity = effective_request_message_len(config);
    let response_capacity = effective_response_message_len(config);
    let (request_offset, response_offset, mapping_len) =
        compute_region_layout(request_capacity, response_capacity)?;

    for _ in 0..2 {
        let open_res = OpenOptions::new()
            .read(true)
            .write(true)
            .create_new(true)
            .mode(effective_shm_mode(config))
            .open(&path);

        match open_res {
            Ok(file) => {
                let duplicated = unsafe { libc::dup(file.as_raw_fd()) };
                if duplicated < 0 {
                    return Err(io::Error::last_os_error());
                }
                drop(file);
                if let Err(err) = lock_endpoint_fd(duplicated) {
                    unsafe {
                        libc::close(duplicated);
                    }
                    return Err(err);
                }
                fd = duplicated;
                break;
            }
            Err(err) => {
                if err.kind() != io::ErrorKind::AlreadyExists {
                    return Err(err);
                }
                if !try_takeover_stale_shm_endpoint(&path)? {
                    return Err(io::Error::from_raw_os_error(libc::EADDRINUSE));
                }
            }
        }
    }

    if fd < 0 {
        return Err(io::Error::from_raw_os_error(libc::EEXIST));
    }

    if unsafe { libc::ftruncate(fd, mapping_len as libc::off_t) } != 0 {
        let err = io::Error::last_os_error();
        unsafe {
            libc::close(fd);
        }
        let _ = fs::remove_file(&path);
        return Err(err);
    }

    let region = MappedRegion::map(fd, mapping_len, path, true, true)?;
    unsafe {
        ptr::write_bytes(region.ptr, 0, mapping_len);
        let header = &mut *(region.ptr as *mut RegionHeader);
        header.magic = SHM_REGION_MAGIC;
        header.version = SHM_REGION_VERSION;
        header.header_len = REGION_HEADER_LEN as u16;
        header.owner_pid = libc::getpid();
        header.owner_generation = 1;
        header.request_offset = request_offset;
        header.request_capacity = request_capacity as u32;
        header.response_offset = response_offset;
        header.response_capacity = response_capacity as u32;

    }
    Ok(region)
}

fn open_client_region(config: &ShmConfig) -> io::Result<MappedRegion> {
    let path = config.endpoint_path();
    let file = OpenOptions::new().read(true).write(true).open(&path)?;
    let meta = file.metadata()?;
    if meta.len() < REGION_HEADER_LEN as u64 {
        return Err(io::Error::from_raw_os_error(libc::EPROTO));
    }

    let dup_fd = unsafe { libc::dup(file.as_raw_fd()) };
    if dup_fd < 0 {
        return Err(io::Error::last_os_error());
    }
    drop(file);

    let region = MappedRegion::map(dup_fd, meta.len() as usize, path, false, false)?;
    validate_region_header(region.header(), region.mapping_len)?;

    if region.request_capacity() < effective_request_message_len(config)
        || region.response_capacity() < effective_response_message_len(config)
    {
        return Err(io::Error::from_raw_os_error(libc::EMSGSIZE));
    }

    if !endpoint_owned_by_live_server(region.fd, region.owner_pid())? {
        return Err(io::Error::from_raw_os_error(libc::ECONNREFUSED));
    }

    Ok(region)
}

fn receive_request_bytes(
    server: &mut ShmServer,
    message: &mut [u8],
    timeout: Option<Duration>,
) -> io::Result<usize> {
    let target = server.last_request_seq + 1;
    wait_for_sequence(
        &server.region.header().req_seq,
        target,
        server.region.req_signal(),
        server.spin_tries,
        timeout_to_ms(timeout),
    )?;

    let published_seq = server.region.load_req_seq();
    let published_len = server.region.load_req_len() as usize;
    if published_len > server.max_request_message_len
        || published_len > server.region.request_capacity()
    {
        return Err(io::Error::from_raw_os_error(libc::EMSGSIZE));
    }
    if message.len() < published_len {
        return Err(io::Error::from_raw_os_error(libc::EMSGSIZE));
    }

    server.region.read_request_bytes(published_len, message);
    server.last_request_seq = published_seq;
    Ok(published_len)
}

fn send_response_bytes(server: &mut ShmServer, message: &[u8]) -> io::Result<()> {
    if server.last_request_seq == 0 || server.last_request_seq == server.last_response_seq {
        return Err(io::Error::from_raw_os_error(libc::EPROTO));
    }

    if message.len() > server.max_response_message_len
        || message.len() > server.region.response_capacity()
        || message.len() > u32::MAX as usize
    {
        return Err(io::Error::from_raw_os_error(libc::EMSGSIZE));
    }

    server.region.write_response_bytes(message);
    server.region.store_resp_len(message.len() as u32);
    server.region.store_resp_seq(server.last_request_seq);
    signal_wake(server.region.resp_signal());

    server.last_response_seq = server.last_request_seq;
    Ok(())
}

fn call_request_bytes(
    client: &mut ShmClient,
    request_message: &[u8],
    response_message: &mut [u8],
    timeout: Option<Duration>,
) -> io::Result<usize> {
    if request_message.len() > client.max_request_message_len
        || request_message.len() > client.region.request_capacity()
        || request_message.len() > u32::MAX as usize
    {
        return Err(io::Error::from_raw_os_error(libc::EMSGSIZE));
    }

    let seq = client.next_request_seq + 1;
    client.next_request_seq = seq;

    client.region.write_request_bytes(request_message);
    client.region.store_req_len(request_message.len() as u32);
    client.region.store_req_seq(seq);
    signal_wake(client.region.req_signal());

    wait_for_sequence(
        &client.region.header().resp_seq,
        seq,
        client.region.resp_signal(),
        client.spin_tries,
        timeout_to_ms(timeout),
    )?;

    let response_len = client.region.load_resp_len() as usize;
    if response_len > client.max_response_message_len
        || response_len > client.region.response_capacity()
    {
        return Err(io::Error::from_raw_os_error(libc::EMSGSIZE));
    }
    if response_message.len() < response_len {
        return Err(io::Error::from_raw_os_error(libc::EMSGSIZE));
    }

    client
        .region
        .read_response_bytes(response_len, response_message);
    Ok(response_len)
}

fn perform_server_handshake(
    server: &UdsSeqpacketServer,
    fd: RawFd,
    timeout: Option<Duration>,
) -> io::Result<NegotiationResult> {
    let hello = read_hello_negotiation(fd, timeout)?;
    let intersection_profiles = hello.supported_profiles & server.supported_profiles;

    let mut ack = HelloAckPayload {
        layout_version: hello.layout_version,
        flags: 0,
        server_supported_profiles: server.supported_profiles,
        intersection_profiles,
        selected_profile: 0,
        agreed_max_request_payload_bytes: negotiate_limit_u32(
            hello.max_request_payload_bytes,
            server.max_request_payload_bytes,
        ),
        agreed_max_request_batch_items: negotiate_limit_u32(
            hello.max_request_batch_items,
            server.max_request_batch_items,
        ),
        agreed_max_response_payload_bytes: negotiate_limit_u32(
            hello.max_response_payload_bytes,
            server.max_response_payload_bytes,
        ),
        agreed_max_response_batch_items: negotiate_limit_u32(
            hello.max_response_batch_items,
            server.max_response_batch_items,
        ),
    };
    let mut status = NEGOTIATION_STATUS_OK;

    if server.auth_token != 0 && hello.auth_token != server.auth_token {
        status = libc::EACCES as u32;
    } else {
        let mut candidates = intersection_profiles & server.preferred_profiles;
        if candidates == 0 {
            candidates = intersection_profiles;
        }
        ack.selected_profile = select_profile(candidates);
        if ack.selected_profile == 0 {
            status = libc::ENOTSUP as u32;
        } else if ack.agreed_max_request_payload_bytes == 0
            || ack.agreed_max_request_batch_items == 0
            || ack.agreed_max_response_payload_bytes == 0
            || ack.agreed_max_response_batch_items == 0
        {
            status = libc::EPROTO as u32;
        }
    }

    write_ack_negotiation(fd, &ack, status, timeout)?;
    if status != NEGOTIATION_STATUS_OK {
        return Err(io::Error::from_raw_os_error(status as i32));
    }

    Ok(NegotiationResult {
        profile: ack.selected_profile,
        max_request_message_len: compute_max_message_len(
            ack.agreed_max_request_payload_bytes,
            ack.agreed_max_request_batch_items,
        )?,
        max_response_message_len: compute_max_message_len(
            ack.agreed_max_response_payload_bytes,
            ack.agreed_max_response_batch_items,
        )?,
    })
}

fn perform_client_handshake(
    fd: RawFd,
    supported_profiles: u32,
    preferred_profiles: u32,
    max_request_payload_bytes: u32,
    max_request_batch_items: u32,
    max_response_payload_bytes: u32,
    max_response_batch_items: u32,
    auth_token: u64,
    timeout: Option<Duration>,
) -> io::Result<NegotiationResult> {
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
    };
    write_hello_negotiation(fd, &hello, timeout)?;

    let (ack, status) = read_ack_negotiation(fd, timeout)?;
    if status != NEGOTIATION_STATUS_OK {
        return Err(io::Error::from_raw_os_error(status as i32));
    }
    if (ack.intersection_profiles & supported_profiles) == 0
        || ack.selected_profile == 0
        || (ack.selected_profile & supported_profiles) == 0
        || ack.agreed_max_request_payload_bytes == 0
        || ack.agreed_max_request_batch_items == 0
        || ack.agreed_max_response_payload_bytes == 0
        || ack.agreed_max_response_batch_items == 0
    {
        return Err(io::Error::from_raw_os_error(libc::EPROTO));
    }
    Ok(NegotiationResult {
        profile: ack.selected_profile,
        max_request_message_len: compute_max_message_len(
            ack.agreed_max_request_payload_bytes,
            ack.agreed_max_request_batch_items,
        )?,
        max_response_message_len: compute_max_message_len(
            ack.agreed_max_response_payload_bytes,
            ack.agreed_max_response_batch_items,
        )?,
    })
}

impl UdsSeqpacketServer {
    pub fn bind(config: &UdsSeqpacketConfig) -> io::Result<Self> {
        if !config_is_valid(config) {
            return Err(invalid_input("run_dir and service_name must be set"));
        }

        let endpoint_path = config.endpoint_path();
        let supported_profiles = effective_supported_profiles(config);
        let preferred_profiles = effective_preferred_profiles(config, supported_profiles);
        let listen_fd = create_seqpacket_socket()?;
        let (addr, addr_len) = make_sockaddr(&endpoint_path)?;

        let mut bound = false;
        for _ in 0..2 {
            let rc = unsafe {
                libc::bind(
                    listen_fd,
                    &addr as *const _ as *const libc::sockaddr,
                    addr_len,
                )
            };
            if rc == 0 {
                bound = true;
                break;
            }

            let err = io::Error::last_os_error();
            if err.raw_os_error() != Some(libc::EADDRINUSE) {
                unsafe {
                    libc::close(listen_fd);
                }
                return Err(err);
            }

            if !try_takeover_stale_socket(&endpoint_path)? {
                unsafe {
                    libc::close(listen_fd);
                }
                return Err(io::Error::from_raw_os_error(libc::EADDRINUSE));
            }
        }

        if !bound {
            unsafe {
                libc::close(listen_fd);
            }
            return Err(io::Error::from_raw_os_error(libc::EADDRINUSE));
        }

        fs::set_permissions(
            &endpoint_path,
            fs::Permissions::from_mode(effective_mode(config)),
        )?;
        let rc = unsafe { libc::listen(listen_fd, 16) };
        if rc != 0 {
            let err = io::Error::last_os_error();
            unsafe {
                libc::close(listen_fd);
            }
            let _ = fs::remove_file(&endpoint_path);
            return Err(err);
        }

        Ok(Self {
            listen_fd,
            conn_fd: None,
            endpoint_path,
            shm_config: shm_config_from_uds(config, FRAME_SIZE, FRAME_SIZE),
            shm_server: None,
            supported_profiles,
            preferred_profiles,
            max_request_payload_bytes: effective_payload_limit(config.max_request_payload_bytes),
            max_request_batch_items: effective_batch_limit(config.max_request_batch_items),
            max_response_payload_bytes: effective_payload_limit(config.max_response_payload_bytes),
            max_response_batch_items: effective_batch_limit(config.max_response_batch_items),
            auth_token: config.auth_token,
            negotiated_profile: 0,
            max_request_message_len: 0,
            max_response_message_len: 0,
        })
    }

    pub fn accept(&mut self, timeout: Option<Duration>) -> io::Result<()> {
        if self.conn_fd.is_some() {
            return Err(io::Error::from_raw_os_error(libc::EISCONN));
        }

        if let Some(duration) = timeout {
            let mut pfd = libc::pollfd {
                fd: self.listen_fd,
                events: libc::POLLIN,
                revents: 0,
            };
            let timeout_ms = duration.as_millis().min(i32::MAX as u128) as libc::c_int;
            let rc = unsafe { libc::poll(&mut pfd, 1, timeout_ms) };
            if rc == 0 {
                return Err(io::Error::from_raw_os_error(libc::ETIMEDOUT));
            }
            if rc < 0 {
                return Err(io::Error::last_os_error());
            }
        }

        let fd =
            unsafe { libc::accept(self.listen_fd, std::ptr::null_mut(), std::ptr::null_mut()) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        if let Err(err) = set_cloexec(fd) {
            unsafe {
                libc::close(fd);
            }
            return Err(err);
        }

        match perform_server_handshake(self, fd, timeout) {
            Ok(negotiated) => {
                if negotiated.profile == PROFILE_SHM_HYBRID {
                    self.shm_server = None;
                    let mut shm_config = self.shm_config.clone();
                    shm_config.max_request_message_bytes =
                        negotiated.max_request_message_len as u32;
                    shm_config.max_response_message_bytes =
                        negotiated.max_response_message_len as u32;
                    self.shm_server = Some(ShmServer::create(&shm_config)?);
                } else {
                    self.shm_server = None;
                }
                self.negotiated_profile = negotiated.profile;
                self.max_request_message_len = negotiated.max_request_message_len;
                self.max_response_message_len = negotiated.max_response_message_len;
                self.conn_fd = Some(fd);
                Ok(())
            }
            Err(err) => {
                unsafe {
                    libc::close(fd);
                }
                Err(err)
            }
        }
    }

    pub fn receive_frame(&mut self, timeout: Option<Duration>) -> io::Result<Frame> {
        if self.negotiated_profile == PROFILE_SHM_HYBRID {
            return self
                .shm_server
                .as_mut()
                .ok_or_else(|| io::Error::from_raw_os_error(libc::EPROTO))?
                .receive_frame(timeout);
        }

        let fd = self
            .conn_fd
            .ok_or_else(|| io::Error::from_raw_os_error(libc::ENOTCONN))?;
        let mut frame = [0u8; FRAME_SIZE];
        recv_exact(fd, &mut frame, timeout)?;
        Ok(frame)
    }

    pub fn send_frame(&mut self, frame: &Frame, timeout: Option<Duration>) -> io::Result<()> {
        if self.negotiated_profile == PROFILE_SHM_HYBRID {
            let _ = timeout;
            return self
                .shm_server
                .as_mut()
                .ok_or_else(|| io::Error::from_raw_os_error(libc::EPROTO))?
                .send_frame(frame);
        }

        let fd = self
            .conn_fd
            .ok_or_else(|| io::Error::from_raw_os_error(libc::ENOTCONN))?;
        send_exact(fd, frame, timeout)
    }

    pub fn receive_message(
        &mut self,
        message: &mut [u8],
        timeout: Option<Duration>,
    ) -> io::Result<usize> {
        if self.negotiated_profile == PROFILE_SHM_HYBRID {
            return self
                .shm_server
                .as_mut()
                .ok_or_else(|| io::Error::from_raw_os_error(libc::EPROTO))?
                .receive_message(message, timeout);
        }

        let fd = self
            .conn_fd
            .ok_or_else(|| io::Error::from_raw_os_error(libc::ENOTCONN))?;
        if self.max_request_message_len == 0 || message.len() < self.max_request_message_len {
            return Err(io::Error::from_raw_os_error(libc::EMSGSIZE));
        }

        let message_len = recv_seqpacket_message(fd, message, timeout)?;
        validate_received_message(message, message_len, self.max_request_message_len)?;
        Ok(message_len)
    }

    pub fn send_message(&mut self, message: &[u8], timeout: Option<Duration>) -> io::Result<()> {
        if self.negotiated_profile == PROFILE_SHM_HYBRID {
            let _ = timeout;
            return self
                .shm_server
                .as_mut()
                .ok_or_else(|| io::Error::from_raw_os_error(libc::EPROTO))?
                .send_message(message);
        }

        let fd = self
            .conn_fd
            .ok_or_else(|| io::Error::from_raw_os_error(libc::ENOTCONN))?;
        if self.max_response_message_len == 0 {
            return Err(io::Error::from_raw_os_error(libc::EPROTO));
        }

        validate_message_for_send(message, self.max_response_message_len)?;
        send_seqpacket_message(fd, message, timeout)
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
        timeout: Option<Duration>,
    ) -> io::Result<()> {
        self.send_frame(&encode_increment_response(request_id, response), timeout)
    }

    pub fn negotiated_profile(&self) -> u32 {
        self.negotiated_profile
    }
}

impl UdsSeqpacketClient {
    pub fn connect(config: &UdsSeqpacketConfig, timeout: Option<Duration>) -> io::Result<Self> {
        if !config_is_valid(config) {
            return Err(invalid_input("run_dir and service_name must be set"));
        }

        let supported_profiles = effective_supported_profiles(config);
        let preferred_profiles = effective_preferred_profiles(config, supported_profiles);
        let fd = create_seqpacket_socket()?;
        let endpoint_path = config.endpoint_path();

        if let Err(err) = connect_with_timeout(fd, &endpoint_path, timeout) {
            unsafe {
                libc::close(fd);
            }
            return Err(err);
        }

        let max_request_payload_bytes = effective_payload_limit(config.max_request_payload_bytes);
        let max_request_batch_items = effective_batch_limit(config.max_request_batch_items);
        let max_response_payload_bytes = effective_payload_limit(config.max_response_payload_bytes);
        let max_response_batch_items = effective_batch_limit(config.max_response_batch_items);
        match perform_client_handshake(
            fd,
            supported_profiles,
            preferred_profiles,
            max_request_payload_bytes,
            max_request_batch_items,
            max_response_payload_bytes,
            max_response_batch_items,
            config.auth_token,
            timeout,
        ) {
            Ok(negotiated) => Ok(Self {
                fd,
                shm_client: if negotiated.profile == PROFILE_SHM_HYBRID {
                    Some(create_shm_client_with_retry(
                        &shm_config_from_uds(
                            config,
                            negotiated.max_request_message_len,
                            negotiated.max_response_message_len,
                        ),
                        timeout,
                    )?)
                } else {
                    None
                },
                supported_profiles,
                preferred_profiles,
                auth_token: config.auth_token,
                negotiated_profile: negotiated.profile,
                max_request_message_len: negotiated.max_request_message_len,
                max_response_message_len: negotiated.max_response_message_len,
                next_request_id: 1,
            }),
            Err(err) => {
                unsafe {
                    libc::close(fd);
                }
                Err(err)
            }
        }
    }

    pub fn call_frame(
        &mut self,
        request_frame: &Frame,
        timeout: Option<Duration>,
    ) -> io::Result<Frame> {
        if self.negotiated_profile == PROFILE_SHM_HYBRID {
            return self
                .shm_client
                .as_mut()
                .ok_or_else(|| io::Error::from_raw_os_error(libc::EPROTO))?
                .call_frame(request_frame, timeout);
        }

        send_exact(self.fd, request_frame, timeout)?;
        let mut response_frame = [0u8; FRAME_SIZE];
        recv_exact(self.fd, &mut response_frame, timeout)?;
        Ok(response_frame)
    }

    pub fn call_message(
        &mut self,
        request_message: &[u8],
        response_message: &mut [u8],
        timeout: Option<Duration>,
    ) -> io::Result<usize> {
        if self.negotiated_profile == PROFILE_SHM_HYBRID {
            return self
                .shm_client
                .as_mut()
                .ok_or_else(|| io::Error::from_raw_os_error(libc::EPROTO))?
                .call_message(request_message, response_message, timeout);
        }

        if self.max_request_message_len == 0 || self.max_response_message_len == 0 {
            return Err(io::Error::from_raw_os_error(libc::EPROTO));
        }
        if response_message.len() < self.max_response_message_len {
            return Err(io::Error::from_raw_os_error(libc::EMSGSIZE));
        }

        validate_message_for_send(request_message, self.max_request_message_len)?;
        send_seqpacket_message(self.fd, request_message, timeout)?;
        let response_len = recv_seqpacket_message(self.fd, response_message, timeout)?;
        validate_received_message(
            response_message,
            response_len,
            self.max_response_message_len,
        )?;
        Ok(response_len)
    }

    pub fn call_increment(
        &mut self,
        request: &IncrementRequest,
        timeout: Option<Duration>,
    ) -> io::Result<IncrementResponse> {
        let request_id = self.next_request_id;
        self.next_request_id += 1;

        let response_frame =
            self.call_frame(&encode_increment_request(request_id, request), timeout)?;
        let (response_id, response) = decode_increment_response(&response_frame)?;
        if response_id != request_id {
            return Err(io::Error::from_raw_os_error(libc::EPROTO));
        }
        Ok(response)
    }

    pub fn negotiated_profile(&self) -> u32 {
        self.negotiated_profile
    }

    pub fn supported_profiles(&self) -> u32 {
        self.supported_profiles
    }

    pub fn preferred_profiles(&self) -> u32 {
        self.preferred_profiles
    }

    pub fn auth_token(&self) -> u64 {
        self.auth_token
    }
}

impl ShmServer {
    pub fn create(config: &ShmConfig) -> io::Result<Self> {
        if !shm_config_is_valid(config) {
            return Err(invalid_input("run_dir and service_name must be set"));
        }

        Ok(Self {
            region: create_server_region(config)?,
            spin_tries: effective_spin_tries(config),
            last_request_seq: 0,
            last_response_seq: 0,
            max_request_message_len: effective_request_message_len(config),
            max_response_message_len: effective_response_message_len(config),
        })
    }

    pub fn receive_frame(&mut self, timeout: Option<Duration>) -> io::Result<Frame> {
        let mut frame = [0u8; FRAME_SIZE];
        let frame_len = receive_request_bytes(self, &mut frame, timeout)?;
        if frame_len != FRAME_SIZE {
            return Err(io::Error::from_raw_os_error(libc::EPROTO));
        }
        Ok(frame)
    }

    pub fn send_frame(&mut self, frame: &Frame) -> io::Result<()> {
        if self.max_response_message_len < FRAME_SIZE {
            return Err(io::Error::from_raw_os_error(libc::EMSGSIZE));
        }
        send_response_bytes(self, frame)
    }

    pub fn receive_message(
        &mut self,
        message: &mut [u8],
        timeout: Option<Duration>,
    ) -> io::Result<usize> {
        if message.len() < self.max_request_message_len {
            return Err(io::Error::from_raw_os_error(libc::EMSGSIZE));
        }

        let message_len = receive_request_bytes(self, message, timeout)?;
        validate_received_message(message, message_len, self.max_request_message_len)?;
        Ok(message_len)
    }

    pub fn send_message(&mut self, message: &[u8]) -> io::Result<()> {
        validate_message_for_send(message, self.max_response_message_len)?;
        send_response_bytes(self, message)
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
    ) -> io::Result<()> {
        self.send_frame(&encode_increment_response(request_id, response))
    }
}

impl ShmClient {
    pub fn connect(config: &ShmConfig) -> io::Result<Self> {
        if !shm_config_is_valid(config) {
            return Err(invalid_input("run_dir and service_name must be set"));
        }

        Ok(Self {
            region: open_client_region(config)?,
            spin_tries: effective_spin_tries(config),
            next_request_seq: 0,
            max_request_message_len: effective_request_message_len(config),
            max_response_message_len: effective_response_message_len(config),
        })
    }

    pub fn call_frame(
        &mut self,
        request_frame: &Frame,
        timeout: Option<Duration>,
    ) -> io::Result<Frame> {
        if self.max_request_message_len < FRAME_SIZE || self.max_response_message_len < FRAME_SIZE {
            return Err(io::Error::from_raw_os_error(libc::EMSGSIZE));
        }

        let mut response_frame = [0u8; FRAME_SIZE];
        let response_len = call_request_bytes(self, request_frame, &mut response_frame, timeout)?;
        if response_len != FRAME_SIZE {
            return Err(io::Error::from_raw_os_error(libc::EPROTO));
        }
        Ok(response_frame)
    }

    pub fn call_message(
        &mut self,
        request_message: &[u8],
        response_message: &mut [u8],
        timeout: Option<Duration>,
    ) -> io::Result<usize> {
        if response_message.len() < self.max_response_message_len {
            return Err(io::Error::from_raw_os_error(libc::EMSGSIZE));
        }

        validate_message_for_send(request_message, self.max_request_message_len)?;
        let response_len = call_request_bytes(self, request_message, response_message, timeout)?;
        validate_received_message(
            response_message,
            response_len,
            self.max_response_message_len,
        )?;
        Ok(response_len)
    }

    pub fn call_increment(
        &mut self,
        request: &IncrementRequest,
        timeout: Option<Duration>,
    ) -> io::Result<IncrementResponse> {
        let request_id = self.next_request_seq + 1;
        let response_frame =
            self.call_frame(&encode_increment_request(request_id, request), timeout)?;
        let (response_id, response) = decode_increment_response(&response_frame)?;
        if response_id != request_id {
            return Err(io::Error::from_raw_os_error(libc::EPROTO));
        }
        Ok(response)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::STATUS_OK;
    use std::process;
    use std::sync::mpsc;
    use std::thread;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_run_dir(name: &str) -> PathBuf {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("time")
            .as_nanos();
        let path =
            std::env::temp_dir().join(format!("netipc-rs-{name}-{}-{unique}", process::id()));
        fs::create_dir_all(&path).expect("create temp run dir");
        path
    }

    #[test]
    fn increment_roundtrip() {
        let run_dir = temp_run_dir("roundtrip");
        let server_cfg = UdsSeqpacketConfig::new(&run_dir, "service");
        let client_cfg = server_cfg.clone();

        let (ready_tx, ready_rx) = mpsc::channel();
        let handle = thread::spawn(move || -> io::Result<()> {
            let mut server = UdsSeqpacketServer::bind(&server_cfg)?;
            ready_tx.send(()).expect("signal ready");
            server.accept(Some(Duration::from_secs(5)))?;
            let (request_id, request) = server.receive_increment(Some(Duration::from_secs(5)))?;
            assert_eq!(request.value, 41);
            server.send_increment(
                request_id,
                &IncrementResponse {
                    status: STATUS_OK,
                    value: 42,
                },
                Some(Duration::from_secs(5)),
            )
        });

        ready_rx
            .recv_timeout(Duration::from_secs(5))
            .expect("server ready");
        let mut client = UdsSeqpacketClient::connect(&client_cfg, Some(Duration::from_secs(5)))
            .expect("connect client");
        let response = client
            .call_increment(
                &IncrementRequest { value: 41 },
                Some(Duration::from_secs(5)),
            )
            .expect("call increment");
        assert_eq!(client.negotiated_profile(), PROFILE_UDS_SEQPACKET);
        assert_eq!(response.value, 42);
        assert_eq!(response.status, STATUS_OK);
        handle
            .join()
            .expect("server thread")
            .expect("server result");
        let _ = fs::remove_dir_all(run_dir);
    }

    #[test]
    fn negotiated_shm_roundtrip() {
        let run_dir = temp_run_dir("negotiated-shm");
        let mut server_cfg = UdsSeqpacketConfig::new(&run_dir, "service");
        server_cfg.supported_profiles = PROFILE_UDS_SEQPACKET | PROFILE_SHM_HYBRID;
        server_cfg.preferred_profiles = PROFILE_SHM_HYBRID;
        let client_cfg = server_cfg.clone();

        let (ready_tx, ready_rx) = mpsc::channel();
        let handle = thread::spawn(move || -> io::Result<()> {
            let mut server = UdsSeqpacketServer::bind(&server_cfg)?;
            ready_tx.send(()).expect("signal ready");
            server.accept(Some(Duration::from_secs(5)))?;
            assert_eq!(server.negotiated_profile(), PROFILE_SHM_HYBRID);
            let (request_id, request) = server.receive_increment(Some(Duration::from_secs(5)))?;
            assert_eq!(request.value, 41);
            server.send_increment(
                request_id,
                &IncrementResponse {
                    status: STATUS_OK,
                    value: 42,
                },
                Some(Duration::from_secs(5)),
            )
        });

        ready_rx
            .recv_timeout(Duration::from_secs(5))
            .expect("server ready");
        let mut client = UdsSeqpacketClient::connect(&client_cfg, Some(Duration::from_secs(5)))
            .expect("connect client");
        let response = client
            .call_increment(
                &IncrementRequest { value: 41 },
                Some(Duration::from_secs(5)),
            )
            .expect("call increment");
        assert_eq!(client.negotiated_profile(), PROFILE_SHM_HYBRID);
        assert_eq!(response.value, 42);
        assert_eq!(response.status, STATUS_OK);
        handle
            .join()
            .expect("server thread")
            .expect("server result");
        let _ = fs::remove_dir_all(run_dir);
    }

    #[test]
    fn auth_mismatch_is_rejected() {
        let run_dir = temp_run_dir("auth");
        let mut server_cfg = UdsSeqpacketConfig::new(&run_dir, "service");
        server_cfg.auth_token = 111;
        let mut client_cfg = server_cfg.clone();
        client_cfg.auth_token = 222;

        let (ready_tx, ready_rx) = mpsc::channel();
        let handle = thread::spawn(move || {
            let mut server = UdsSeqpacketServer::bind(&server_cfg).expect("bind server");
            ready_tx.send(()).expect("signal ready");
            server
                .accept(Some(Duration::from_secs(5)))
                .expect_err("server accept should fail")
                .raw_os_error()
        });

        ready_rx
            .recv_timeout(Duration::from_secs(5))
            .expect("server ready");
        let err = UdsSeqpacketClient::connect(&client_cfg, Some(Duration::from_secs(5)))
            .expect_err("client connect should fail");
        assert_eq!(err.raw_os_error(), Some(libc::EACCES));
        assert_eq!(handle.join().expect("server thread"), Some(libc::EACCES));
        let _ = fs::remove_dir_all(run_dir);
    }

    #[test]
    fn shm_increment_roundtrip() {
        let run_dir = temp_run_dir("shm-roundtrip");
        let server_cfg = ShmConfig::new(&run_dir, "service");
        let client_cfg = server_cfg.clone();

        let (ready_tx, ready_rx) = mpsc::channel();
        let handle = thread::spawn(move || -> io::Result<()> {
            let mut server = ShmServer::create(&server_cfg)?;
            ready_tx.send(()).expect("signal ready");
            let (request_id, request) = server.receive_increment(Some(Duration::from_secs(5)))?;
            assert_eq!(request.value, 41);
            server.send_increment(
                request_id,
                &IncrementResponse {
                    status: STATUS_OK,
                    value: 42,
                },
            )
        });

        ready_rx
            .recv_timeout(Duration::from_secs(5))
            .expect("server ready");
        let mut client = ShmClient::connect(&client_cfg).expect("connect shm client");
        let response = client
            .call_increment(
                &IncrementRequest { value: 41 },
                Some(Duration::from_secs(5)),
            )
            .expect("call shm increment");
        assert_eq!(response.status, STATUS_OK);
        assert_eq!(response.value, 42);
        handle
            .join()
            .expect("server thread")
            .expect("server result");
        let _ = fs::remove_dir_all(run_dir);
    }

    #[test]
    fn shm_send_without_request_is_rejected() {
        let run_dir = temp_run_dir("shm-invalid-state");
        let server_cfg = ShmConfig::new(&run_dir, "service");
        let mut server = ShmServer::create(&server_cfg).expect("create shm server");

        let err = server
            .send_increment(
                1,
                &IncrementResponse {
                    status: STATUS_OK,
                    value: 1,
                },
            )
            .expect_err("send without request should fail");
        assert_eq!(err.raw_os_error(), Some(libc::EPROTO));
        let _ = fs::remove_dir_all(run_dir);
    }

    #[test]
    fn shm_server_reclaims_stale_endpoint() {
        let run_dir = temp_run_dir("shm-stale");
        let cfg = ShmConfig::new(&run_dir, "service");
        fs::write(cfg.endpoint_path(), b"stale").expect("write stale endpoint");

        let server = ShmServer::create(&cfg).expect("create server over stale endpoint");
        assert!(cfg.endpoint_path().exists());
        drop(server);
        let _ = fs::remove_dir_all(run_dir);
    }

    #[test]
    fn shm_client_rejects_unlocked_endpoint() {
        let run_dir = temp_run_dir("shm-unlocked");
        let cfg = ShmConfig::new(&run_dir, "service");
        let request_capacity = effective_request_message_len(&cfg);
        let response_capacity = effective_response_message_len(&cfg);
        let (request_offset, response_offset, mapping_len) =
            compute_region_layout(request_capacity, response_capacity)
                .expect("compute synthetic region layout");
        let mut region = vec![0u8; mapping_len];

        region[0..4].copy_from_slice(&SHM_REGION_MAGIC.to_ne_bytes());
        region[4..6].copy_from_slice(&SHM_REGION_VERSION.to_ne_bytes());
        region[6..8].copy_from_slice(&(REGION_HEADER_LEN as u16).to_ne_bytes());
        region[8..12].copy_from_slice(&424242i32.to_ne_bytes());
        region[12..16].copy_from_slice(&1u32.to_ne_bytes());
        region[16..20].copy_from_slice(&request_offset.to_ne_bytes());
        region[20..24].copy_from_slice(&(request_capacity as u32).to_ne_bytes());
        region[24..28].copy_from_slice(&response_offset.to_ne_bytes());
        region[28..32].copy_from_slice(&(response_capacity as u32).to_ne_bytes());
        fs::write(cfg.endpoint_path(), region).expect("write synthetic region");

        let err =
            ShmClient::connect(&cfg).expect_err("connect should fail without a live owner lock");
        assert_eq!(err.raw_os_error(), Some(libc::ECONNREFUSED));
        let _ = fs::remove_dir_all(run_dir);
    }
}
