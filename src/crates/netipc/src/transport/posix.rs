//! POSIX transport implementation for the Rust crate.

use crate::protocol::{
    decode_increment_request, decode_increment_response, encode_increment_request,
    encode_increment_response, Frame, IncrementRequest, IncrementResponse, FRAME_SIZE,
};
use std::fs::{self, OpenOptions};
use std::io;
use std::mem::{offset_of, size_of};
use std::os::fd::{AsRawFd, RawFd};
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::thread;
use std::time::{Duration, Instant};

pub const PROFILE_UDS_SEQPACKET: u32 = 1u32 << 0;
pub const PROFILE_SHM_HYBRID: u32 = 1u32 << 1;
pub const PROFILE_SHM_FUTEX: u32 = 1u32 << 2;

const IMPLEMENTED_PROFILES: u32 = PROFILE_UDS_SEQPACKET | PROFILE_SHM_HYBRID;
const DEFAULT_SUPPORTED_PROFILES: u32 = PROFILE_UDS_SEQPACKET;
const DEFAULT_PREFERRED_PROFILES: u32 = PROFILE_UDS_SEQPACKET;

const NEGOTIATION_MAGIC: u32 = 0x4e48_534b;
const NEGOTIATION_VERSION: u16 = 1;
const NEGOTIATION_HELLO: u16 = 1;
const NEGOTIATION_ACK: u16 = 2;
const NEGOTIATION_STATUS_OK: u32 = 0;
const NEGOTIATION_FRAME_SIZE: usize = FRAME_SIZE;

const SHM_REGION_MAGIC: u32 = 0x4e53_484d;
const SHM_REGION_VERSION: u16 = 1;
pub const SHM_DEFAULT_SPIN_TRIES: u32 = 20;

#[repr(C)]
struct RegionLayout {
    magic: u32,
    version: u16,
    reserved: u16,
    owner_pid: i32,
    owner_generation: u32,
    req_seq: u64,
    resp_seq: u64,
    req_sem: libc::sem_t,
    resp_sem: libc::sem_t,
    request_frame: Frame,
    response_frame: Frame,
}

const REGION_SIZE: usize = size_of::<RegionLayout>();
const OFF_OWNER_PID: usize = offset_of!(RegionLayout, owner_pid);
const OFF_REQ_SEQ: usize = offset_of!(RegionLayout, req_seq);
const OFF_RESP_SEQ: usize = offset_of!(RegionLayout, resp_seq);
const OFF_REQ_SEM: usize = offset_of!(RegionLayout, req_sem);
const OFF_RESP_SEM: usize = offset_of!(RegionLayout, resp_sem);
const OFF_REQUEST_FRAME: usize = offset_of!(RegionLayout, request_frame);
const OFF_RESPONSE_FRAME: usize = offset_of!(RegionLayout, response_frame);

#[derive(Clone, Debug)]
pub struct UdsSeqpacketConfig {
    pub run_dir: PathBuf,
    pub service_name: String,
    pub file_mode: u32,
    pub supported_profiles: u32,
    pub preferred_profiles: u32,
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
            auth_token: 0,
        }
    }

    pub fn endpoint_path(&self) -> PathBuf {
        let mut path = self.run_dir.clone();
        path.push(format!("{}.sock", self.service_name));
        path
    }
}

#[derive(Clone, Copy, Debug)]
struct NegotiationMessage {
    ty: u16,
    supported_profiles: u32,
    preferred_profiles: u32,
    intersection_profiles: u32,
    selected_profile: u32,
    auth_token: u64,
    status: u32,
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
    auth_token: u64,
    negotiated_profile: u32,
}

#[derive(Debug)]
pub struct UdsSeqpacketClient {
    fd: RawFd,
    shm_client: Option<ShmClient>,
    supported_profiles: u32,
    preferred_profiles: u32,
    auth_token: u64,
    negotiated_profile: u32,
    next_request_id: u64,
}

#[derive(Clone, Debug)]
pub struct ShmConfig {
    pub run_dir: PathBuf,
    pub service_name: String,
    pub spin_tries: u32,
    pub file_mode: u32,
}

impl ShmConfig {
    pub fn new(run_dir: impl Into<PathBuf>, service_name: impl Into<String>) -> Self {
        Self {
            run_dir: run_dir.into(),
            service_name: service_name.into(),
            spin_tries: SHM_DEFAULT_SPIN_TRIES,
            file_mode: 0o600,
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
    path: PathBuf,
    unlink_on_drop: bool,
    is_server: bool,
    sems_ready: bool,
}

#[derive(Debug)]
pub struct ShmServer {
    region: MappedRegion,
    spin_tries: u32,
    last_request_seq: u64,
    last_response_seq: u64,
}

#[derive(Debug)]
pub struct ShmClient {
    region: MappedRegion,
    spin_tries: u32,
    next_request_seq: u64,
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
    fn map(fd: RawFd, path: PathBuf, unlink_on_drop: bool, is_server: bool) -> io::Result<Self> {
        let ptr = unsafe {
            libc::mmap(
                ptr::null_mut(),
                REGION_SIZE,
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
            path,
            unlink_on_drop,
            is_server,
            sems_ready: false,
        })
    }

    fn sem_ptr(&self, offset: usize) -> *mut libc::sem_t {
        unsafe { self.ptr.add(offset) as *mut libc::sem_t }
    }

    fn load_seq(&self, offset: usize) -> u64 {
        unsafe {
            (self.ptr.add(offset) as *const AtomicU64)
                .as_ref()
                .unwrap()
                .load(Ordering::Acquire)
        }
    }

    fn store_seq(&self, offset: usize, value: u64) {
        unsafe {
            (self.ptr.add(offset) as *const AtomicU64)
                .cast_mut()
                .as_mut()
                .unwrap()
                .store(value, Ordering::Release)
        }
    }

    fn set_owner_pid(&self, pid: i32) {
        unsafe {
            *(self.ptr.add(OFF_OWNER_PID) as *mut i32) = pid;
        }
    }

    fn owner_pid(&self) -> i32 {
        unsafe { *(self.ptr.add(OFF_OWNER_PID) as *const i32) }
    }

    fn read_frame(&self, offset: usize) -> Frame {
        let mut frame = [0u8; FRAME_SIZE];
        unsafe {
            ptr::copy_nonoverlapping(self.ptr.add(offset), frame.as_mut_ptr(), FRAME_SIZE);
        }
        frame
    }

    fn write_frame(&self, offset: usize, frame: &Frame) {
        unsafe {
            ptr::copy_nonoverlapping(frame.as_ptr(), self.ptr.add(offset), FRAME_SIZE);
        }
    }
}

impl Drop for MappedRegion {
    fn drop(&mut self) {
        if self.is_server {
            self.set_owner_pid(0);
            if self.sems_ready {
                unsafe {
                    libc::sem_destroy(self.sem_ptr(OFF_REQ_SEM));
                    libc::sem_destroy(self.sem_ptr(OFF_RESP_SEM));
                }
            }
        }

        unsafe {
            libc::munmap(self.ptr as *mut libc::c_void, REGION_SIZE);
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

fn pid_is_alive(pid: i32) -> bool {
    if pid <= 1 {
        return false;
    }

    let rc = unsafe { libc::kill(pid, 0) };
    if rc == 0 {
        return true;
    }

    io::Error::last_os_error().raw_os_error() == Some(libc::EPERM)
}

fn sem_wait_timeout(sem: *mut libc::sem_t, timeout_ms: u32) -> io::Result<()> {
    if timeout_ms == 0 {
        loop {
            let rc = unsafe { libc::sem_wait(sem) };
            if rc == 0 {
                return Ok(());
            }
            let err = io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if err != libc::EINTR {
                return Err(io::Error::from_raw_os_error(err));
            }
        }
    }

    let mut ts = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    if unsafe { libc::clock_gettime(libc::CLOCK_REALTIME, &mut ts) } != 0 {
        return Err(io::Error::last_os_error());
    }

    let add_ns = (timeout_ms as i128) * 1_000_000i128;
    let total_ns = ts.tv_nsec as i128 + add_ns;
    ts.tv_sec += (total_ns / 1_000_000_000i128) as libc::time_t;
    ts.tv_nsec = (total_ns % 1_000_000_000i128) as libc::c_long;

    loop {
        let rc = unsafe { libc::sem_timedwait(sem, &ts) };
        if rc == 0 {
            return Ok(());
        }
        let err = io::Error::last_os_error().raw_os_error().unwrap_or(0);
        if err != libc::EINTR {
            return Err(io::Error::from_raw_os_error(err));
        }
    }
}

fn wait_for_sequence(
    region: &MappedRegion,
    seq_offset: usize,
    target: u64,
    sem_offset: usize,
    spin_tries: u32,
    timeout_ms: u32,
) -> io::Result<()> {
    let deadline = if timeout_ms > 0 {
        Some(Instant::now() + Duration::from_millis(timeout_ms as u64))
    } else {
        None
    };

    loop {
        if region.load_seq(seq_offset) >= target {
            return Ok(());
        }

        for _ in 0..spin_tries {
            std::hint::spin_loop();
            if region.load_seq(seq_offset) >= target {
                return Ok(());
            }
        }

        if let Some(deadline_at) = deadline {
            if Instant::now() >= deadline_at {
                return Err(io::Error::from_raw_os_error(libc::ETIMEDOUT));
            }
        }

        if timeout_ms == 0 {
            if let Err(err) = sem_wait_timeout(region.sem_ptr(sem_offset), 0) {
                if err.raw_os_error() == Some(libc::EINVAL) {
                    thread::sleep(Duration::from_micros(50));
                    continue;
                }
                return Err(err);
            }
            continue;
        }

        if let Err(err) = sem_wait_timeout(region.sem_ptr(sem_offset), 1) {
            match err.raw_os_error() {
                Some(code) if code == libc::ETIMEDOUT || code == libc::EINVAL => {
                    if code == libc::EINVAL {
                        thread::sleep(Duration::from_micros(50));
                    }
                    continue;
                }
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

fn shm_config_from_uds(config: &UdsSeqpacketConfig) -> ShmConfig {
    ShmConfig {
        run_dir: config.run_dir.clone(),
        service_name: config.service_name.clone(),
        spin_tries: SHM_DEFAULT_SPIN_TRIES,
        file_mode: 0,
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

fn write_u16_le(frame: &mut Frame, off: usize, value: u16) {
    frame[off..off + 2].copy_from_slice(&value.to_le_bytes());
}

fn write_u32_le(frame: &mut Frame, off: usize, value: u32) {
    frame[off..off + 4].copy_from_slice(&value.to_le_bytes());
}

fn write_u64_le(frame: &mut Frame, off: usize, value: u64) {
    frame[off..off + 8].copy_from_slice(&value.to_le_bytes());
}

fn read_u16_le(frame: &Frame, off: usize) -> u16 {
    u16::from_le_bytes([frame[off], frame[off + 1]])
}

fn read_u32_le(frame: &Frame, off: usize) -> u32 {
    u32::from_le_bytes([frame[off], frame[off + 1], frame[off + 2], frame[off + 3]])
}

fn read_u64_le(frame: &Frame, off: usize) -> u64 {
    u64::from_le_bytes([
        frame[off],
        frame[off + 1],
        frame[off + 2],
        frame[off + 3],
        frame[off + 4],
        frame[off + 5],
        frame[off + 6],
        frame[off + 7],
    ])
}

fn encode_negotiation_frame(message: NegotiationMessage) -> Frame {
    let mut frame = [0u8; NEGOTIATION_FRAME_SIZE];
    write_u32_le(&mut frame, 0, NEGOTIATION_MAGIC);
    write_u16_le(&mut frame, 4, NEGOTIATION_VERSION);
    write_u16_le(&mut frame, 6, message.ty);
    write_u32_le(&mut frame, 8, message.supported_profiles);
    write_u32_le(&mut frame, 12, message.preferred_profiles);
    write_u32_le(&mut frame, 16, message.intersection_profiles);
    write_u32_le(&mut frame, 20, message.selected_profile);
    write_u64_le(&mut frame, 24, message.auth_token);
    write_u32_le(&mut frame, 32, message.status);
    frame
}

fn decode_negotiation_frame(frame: &Frame, expected_type: u16) -> io::Result<NegotiationMessage> {
    let magic = read_u32_le(frame, 0);
    let version = read_u16_le(frame, 4);
    let ty = read_u16_le(frame, 6);
    if magic != NEGOTIATION_MAGIC || version != NEGOTIATION_VERSION || ty != expected_type {
        return Err(io::Error::from_raw_os_error(libc::EPROTO));
    }

    Ok(NegotiationMessage {
        ty,
        supported_profiles: read_u32_le(frame, 8),
        preferred_profiles: read_u32_le(frame, 12),
        intersection_profiles: read_u32_le(frame, 16),
        selected_profile: read_u32_le(frame, 20),
        auth_token: read_u64_le(frame, 24),
        status: read_u32_le(frame, 32),
    })
}

fn read_negotiation(
    fd: RawFd,
    expected_type: u16,
    timeout: Option<Duration>,
) -> io::Result<NegotiationMessage> {
    let mut frame = [0u8; NEGOTIATION_FRAME_SIZE];
    recv_exact(fd, &mut frame, timeout)?;
    decode_negotiation_frame(&frame, expected_type)
}

fn write_negotiation(
    fd: RawFd,
    message: NegotiationMessage,
    timeout: Option<Duration>,
) -> io::Result<()> {
    send_exact(fd, &encode_negotiation_frame(message), timeout)
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
    if meta.len() < REGION_SIZE as u64 {
        fs::remove_file(path)?;
        return Ok(true);
    }

    let dup_fd = unsafe { libc::dup(file.as_raw_fd()) };
    if dup_fd < 0 {
        return Err(io::Error::last_os_error());
    }

    let mapped = MappedRegion::map(dup_fd, path.to_path_buf(), false, false)?;
    let magic = unsafe { *(mapped.ptr.add(0) as *const u32) };
    let version = unsafe { *(mapped.ptr.add(4) as *const u16) };
    let owner_pid = mapped.owner_pid();
    drop(mapped);
    drop(file);

    if magic == SHM_REGION_MAGIC && version == SHM_REGION_VERSION && pid_is_alive(owner_pid) {
        return Err(io::Error::from_raw_os_error(libc::EADDRINUSE));
    }

    fs::remove_file(path)?;
    Ok(true)
}

fn create_server_region(config: &ShmConfig) -> io::Result<MappedRegion> {
    let path = config.endpoint_path();
    let mut fd = -1;

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
                fd = duplicated;
                drop(file);
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

    if unsafe { libc::ftruncate(fd, REGION_SIZE as libc::off_t) } != 0 {
        let err = io::Error::last_os_error();
        unsafe {
            libc::close(fd);
        }
        let _ = fs::remove_file(&path);
        return Err(err);
    }

    let mut region = MappedRegion::map(fd, path, true, true)?;
    unsafe {
        ptr::write_bytes(region.ptr, 0, REGION_SIZE);
        *(region.ptr.add(0) as *mut u32) = SHM_REGION_MAGIC;
        *(region.ptr.add(4) as *mut u16) = SHM_REGION_VERSION;
        *(region.ptr.add(OFF_OWNER_PID) as *mut i32) = libc::getpid();
        *(region.ptr.add(12) as *mut u32) = 1;

        if libc::sem_init(region.sem_ptr(OFF_REQ_SEM), 1, 0) != 0 {
            return Err(io::Error::last_os_error());
        }
        if libc::sem_init(region.sem_ptr(OFF_RESP_SEM), 1, 0) != 0 {
            libc::sem_destroy(region.sem_ptr(OFF_REQ_SEM));
            return Err(io::Error::last_os_error());
        }
    }
    region.sems_ready = true;
    Ok(region)
}

fn open_client_region(config: &ShmConfig) -> io::Result<MappedRegion> {
    let path = config.endpoint_path();
    let file = OpenOptions::new().read(true).write(true).open(&path)?;
    let dup_fd = unsafe { libc::dup(file.as_raw_fd()) };
    if dup_fd < 0 {
        return Err(io::Error::last_os_error());
    }
    drop(file);

    let region = MappedRegion::map(dup_fd, path, false, false)?;
    let magic = unsafe { *(region.ptr.add(0) as *const u32) };
    let version = unsafe { *(region.ptr.add(4) as *const u16) };

    if magic != SHM_REGION_MAGIC || version != SHM_REGION_VERSION {
        return Err(io::Error::from_raw_os_error(libc::EPROTO));
    }
    if !pid_is_alive(region.owner_pid()) {
        return Err(io::Error::from_raw_os_error(libc::ECONNREFUSED));
    }

    Ok(region)
}

fn perform_server_handshake(
    fd: RawFd,
    supported_profiles: u32,
    preferred_profiles: u32,
    auth_token: u64,
    timeout: Option<Duration>,
) -> io::Result<u32> {
    let hello = read_negotiation(fd, NEGOTIATION_HELLO, timeout)?;
    let intersection_profiles = hello.supported_profiles & supported_profiles;

    let mut ack = NegotiationMessage {
        ty: NEGOTIATION_ACK,
        supported_profiles,
        preferred_profiles,
        intersection_profiles,
        selected_profile: 0,
        auth_token: 0,
        status: NEGOTIATION_STATUS_OK,
    };

    if auth_token != 0 && hello.auth_token != auth_token {
        ack.status = libc::EACCES as u32;
    } else {
        let mut candidates = intersection_profiles & preferred_profiles;
        if candidates == 0 {
            candidates = intersection_profiles;
        }
        ack.selected_profile = select_profile(candidates);
        if ack.selected_profile == 0 {
            ack.status = libc::ENOTSUP as u32;
        }
    }

    write_negotiation(fd, ack, timeout)?;
    if ack.status != NEGOTIATION_STATUS_OK {
        return Err(io::Error::from_raw_os_error(ack.status as i32));
    }

    Ok(ack.selected_profile)
}

fn perform_client_handshake(
    fd: RawFd,
    supported_profiles: u32,
    preferred_profiles: u32,
    auth_token: u64,
    timeout: Option<Duration>,
) -> io::Result<u32> {
    let hello = NegotiationMessage {
        ty: NEGOTIATION_HELLO,
        supported_profiles,
        preferred_profiles,
        intersection_profiles: 0,
        selected_profile: 0,
        auth_token,
        status: NEGOTIATION_STATUS_OK,
    };
    write_negotiation(fd, hello, timeout)?;

    let ack = read_negotiation(fd, NEGOTIATION_ACK, timeout)?;
    if ack.status != NEGOTIATION_STATUS_OK {
        return Err(io::Error::from_raw_os_error(ack.status as i32));
    }
    if (ack.intersection_profiles & supported_profiles) == 0
        || ack.selected_profile == 0
        || (ack.selected_profile & supported_profiles) == 0
    {
        return Err(io::Error::from_raw_os_error(libc::EPROTO));
    }
    Ok(ack.selected_profile)
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
            shm_config: shm_config_from_uds(config),
            shm_server: None,
            supported_profiles,
            preferred_profiles,
            auth_token: config.auth_token,
            negotiated_profile: 0,
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

        match perform_server_handshake(
            fd,
            self.supported_profiles,
            self.preferred_profiles,
            self.auth_token,
            timeout,
        ) {
            Ok(profile) => {
                if profile == PROFILE_SHM_HYBRID {
                    self.shm_server = None;
                    self.shm_server = Some(ShmServer::create(&self.shm_config)?);
                } else {
                    self.shm_server = None;
                }
                self.negotiated_profile = profile;
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

        match perform_client_handshake(
            fd,
            supported_profiles,
            preferred_profiles,
            config.auth_token,
            timeout,
        ) {
            Ok(negotiated_profile) => Ok(Self {
                fd,
                shm_client: if negotiated_profile == PROFILE_SHM_HYBRID {
                    Some(create_shm_client_with_retry(
                        &shm_config_from_uds(config),
                        timeout,
                    )?)
                } else {
                    None
                },
                supported_profiles,
                preferred_profiles,
                auth_token: config.auth_token,
                negotiated_profile,
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
        })
    }

    pub fn receive_frame(&mut self, timeout: Option<Duration>) -> io::Result<Frame> {
        let target = self.last_request_seq + 1;
        wait_for_sequence(
            &self.region,
            OFF_REQ_SEQ,
            target,
            OFF_REQ_SEM,
            self.spin_tries,
            timeout_to_ms(timeout),
        )?;

        let frame = self.region.read_frame(OFF_REQUEST_FRAME);
        self.last_request_seq = self.region.load_seq(OFF_REQ_SEQ);
        Ok(frame)
    }

    pub fn send_frame(&mut self, frame: &Frame) -> io::Result<()> {
        if self.last_request_seq == 0 || self.last_request_seq == self.last_response_seq {
            return Err(io::Error::from_raw_os_error(libc::EPROTO));
        }

        self.region.write_frame(OFF_RESPONSE_FRAME, frame);
        self.region.store_seq(OFF_RESP_SEQ, self.last_request_seq);
        if unsafe { libc::sem_post(self.region.sem_ptr(OFF_RESP_SEM)) } != 0 {
            return Err(io::Error::last_os_error());
        }

        self.last_response_seq = self.last_request_seq;
        Ok(())
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
        })
    }

    pub fn call_frame(
        &mut self,
        request_frame: &Frame,
        timeout: Option<Duration>,
    ) -> io::Result<Frame> {
        let seq = self.next_request_seq + 1;
        self.next_request_seq = seq;

        self.region.write_frame(OFF_REQUEST_FRAME, request_frame);
        self.region.store_seq(OFF_REQ_SEQ, seq);
        if unsafe { libc::sem_post(self.region.sem_ptr(OFF_REQ_SEM)) } != 0 {
            return Err(io::Error::last_os_error());
        }

        wait_for_sequence(
            &self.region,
            OFF_RESP_SEQ,
            seq,
            OFF_RESP_SEM,
            self.spin_tries,
            timeout_to_ms(timeout),
        )?;

        Ok(self.region.read_frame(OFF_RESPONSE_FRAME))
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
}
