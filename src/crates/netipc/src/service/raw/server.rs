#[cfg(unix)]
use super::common::SERVER_POLL_TIMEOUT_MS;
use super::dispatch::DispatchHandler;
use crate::protocol::NipcError;
use crate::protocol::{HEADER_SIZE, MAX_PAYLOAD_DEFAULT};

#[cfg(unix)]
pub(super) use crate::transport::posix::ServerConfig;

#[cfg(unix)]
use super::server_session_unix::{handle_session_threaded, poll_fd};

#[cfg(unix)]
use crate::transport::posix::UdsListener;

#[cfg(target_os = "linux")]
use crate::protocol::{PROFILE_SHM_FUTEX, PROFILE_SHM_HYBRID};

#[cfg(target_os = "linux")]
use crate::transport::shm::ShmContext;

#[cfg(windows)]
pub(super) use crate::transport::windows::ServerConfig;

#[cfg(windows)]
use super::server_session_windows::handle_session_win_threaded;

#[cfg(windows)]
use crate::transport::windows::{NpListener, NpSession};

#[cfg(windows)]
use crate::transport::win_shm::{
    WinShmContext, PROFILE_BUSYWAIT as WIN_SHM_PROFILE_BUSYWAIT,
    PROFILE_HYBRID as WIN_SHM_PROFILE_HYBRID,
};

use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::Arc;

/// L2 managed server. Typed request/response dispatcher.
///
/// Handles accept, spawns a thread per session (up to worker_count),
/// reads requests, dispatches to handler, sends responses.
pub struct ManagedServer {
    pub(super) run_dir: String,
    pub(super) service_name: String,
    pub(super) server_config: ServerConfig,
    pub(super) expected_method_code: u16,
    pub(super) handler: Option<DispatchHandler>,
    pub(super) running: Arc<AtomicBool>,
    pub(super) learned_request_payload_bytes: Arc<AtomicU32>,
    pub(super) learned_response_payload_bytes: Arc<AtomicU32>,
    pub(super) next_session_id: u64,
    pub(super) worker_count: usize,
    #[cfg(windows)]
    pub(super) listener_handle: Arc<std::sync::Mutex<Option<usize>>>,
}

impl ManagedServer {
    /// Create a new managed server for a single service kind.
    pub fn new(
        run_dir: &str,
        service_name: &str,
        config: ServerConfig,
        expected_method_code: u16,
        handler: Option<DispatchHandler>,
    ) -> Self {
        Self::with_workers(
            run_dir,
            service_name,
            config,
            expected_method_code,
            handler,
            8,
        )
    }

    /// Create a managed server with an explicit worker count.
    pub fn with_workers(
        run_dir: &str,
        service_name: &str,
        config: ServerConfig,
        expected_method_code: u16,
        handler: Option<DispatchHandler>,
        worker_count: usize,
    ) -> Self {
        let learned_request = if config.max_request_payload_bytes != 0 {
            config.max_request_payload_bytes
        } else {
            MAX_PAYLOAD_DEFAULT
        };
        let learned_response = if config.max_response_payload_bytes != 0 {
            config.max_response_payload_bytes
        } else {
            MAX_PAYLOAD_DEFAULT
        };

        ManagedServer {
            run_dir: run_dir.to_string(),
            service_name: service_name.to_string(),
            server_config: config,
            expected_method_code,
            handler,
            running: Arc::new(AtomicBool::new(false)),
            learned_request_payload_bytes: Arc::new(AtomicU32::new(learned_request)),
            learned_response_payload_bytes: Arc::new(AtomicU32::new(learned_response)),
            next_session_id: 1,
            worker_count: if worker_count < 1 { 1 } else { worker_count },
            #[cfg(windows)]
            listener_handle: Arc::new(std::sync::Mutex::new(None)),
        }
    }

    /// Run the acceptor loop. Blocking. Accepts clients, spawns a
    /// thread per session (up to worker_count concurrent sessions).
    ///
    /// Returns when `stop()` is called or on fatal error.
    #[cfg(unix)]
    pub fn run(&mut self) -> Result<(), NipcError> {
        #[cfg(target_os = "linux")]
        crate::transport::shm::cleanup_stale(&self.run_dir, &self.service_name);

        let listener = UdsListener::bind(
            &self.run_dir,
            &self.service_name,
            self.server_config.clone(),
        )
        .map_err(|_| NipcError::BadLayout)?;

        self.running.store(true, Ordering::Release);

        let mut session_threads: Vec<std::thread::JoinHandle<()>> = Vec::new();

        while self.running.load(Ordering::Acquire) {
            let ready = poll_fd(listener.fd(), SERVER_POLL_TIMEOUT_MS as i32);
            if ready < 0 {
                break;
            }
            if ready == 0 {
                session_threads.retain(|t| !t.is_finished());
                continue;
            }

            let (session_id, accept_cfg, precreated_shm, ready) = self.prepare_unix_accept();
            if !ready {
                std::thread::sleep(std::time::Duration::from_millis(10));
                continue;
            }

            let session = match listener.accept_with_config(session_id, accept_cfg) {
                Ok(s) => s,
                Err(_) => {
                    #[cfg(target_os = "linux")]
                    if let Some(mut shm) = precreated_shm {
                        shm.destroy();
                    }
                    if !self.running.load(Ordering::Acquire) {
                        break;
                    }
                    std::thread::sleep(std::time::Duration::from_millis(10));
                    continue;
                }
            };

            session_threads.retain(|t| !t.is_finished());
            if session_threads.len() >= self.worker_count {
                #[cfg(target_os = "linux")]
                if let Some(mut shm) = precreated_shm {
                    shm.destroy();
                }
                drop(session);
                continue;
            }

            #[cfg(target_os = "linux")]
            let shm = match self.finalize_unix_shm(&session, precreated_shm) {
                Some(shm) => Some(shm),
                None if session.selected_profile == PROFILE_SHM_HYBRID
                    || session.selected_profile == PROFILE_SHM_FUTEX =>
                {
                    drop(session);
                    continue;
                }
                None => None,
            };
            #[cfg(not(target_os = "linux"))]
            let shm: Option<()> = None;

            let expected_method_code = self.expected_method_code;
            let handler = self.handler.clone();
            let running = self.running.clone();
            let learned_request_payload_bytes = self.learned_request_payload_bytes.clone();
            let learned_response_payload_bytes = self.learned_response_payload_bytes.clone();

            let t = std::thread::spawn(move || {
                handle_session_threaded(
                    session,
                    #[cfg(target_os = "linux")]
                    shm,
                    #[cfg(not(target_os = "linux"))]
                    shm,
                    expected_method_code,
                    handler,
                    running,
                    learned_request_payload_bytes,
                    learned_response_payload_bytes,
                );
            });
            session_threads.push(t);
        }

        for t in session_threads {
            let _ = t.join();
        }

        Ok(())
    }

    /// Windows: run the acceptor loop over Named Pipes.
    #[cfg(windows)]
    pub fn run(&mut self) -> Result<(), NipcError> {
        let mut listener = NpListener::bind(
            &self.run_dir,
            &self.service_name,
            self.server_config.clone(),
        )
        .map_err(|_| NipcError::BadLayout)?;

        *self.listener_handle.lock().unwrap() = Some(listener.handle() as usize);

        self.running.store(true, Ordering::Release);

        let mut session_threads: Vec<std::thread::JoinHandle<()>> = Vec::new();

        while self.running.load(Ordering::Acquire) {
            let (session_id, accept_cfg, prepared_shm, ready) = self.prepare_windows_accept();
            if !ready {
                std::thread::sleep(std::time::Duration::from_millis(10));
                continue;
            }

            let session = match listener.accept_with_config(session_id, accept_cfg) {
                Ok(s) => s,
                Err(_) => {
                    if let Some(mut prepared) = prepared_shm {
                        prepared.destroy_all();
                    }
                    if !self.running.load(Ordering::Acquire) {
                        break;
                    }
                    std::thread::sleep(std::time::Duration::from_millis(10));
                    continue;
                }
            };

            session_threads.retain(|t| !t.is_finished());
            if session_threads.len() >= self.worker_count {
                if let Some(mut prepared) = prepared_shm {
                    prepared.destroy_all();
                }
                drop(session);
                continue;
            }

            let shm = match self.finalize_windows_shm(&session, prepared_shm) {
                Some(shm) => Some(shm),
                None if session.selected_profile == WIN_SHM_PROFILE_HYBRID
                    || session.selected_profile == WIN_SHM_PROFILE_BUSYWAIT =>
                {
                    drop(session);
                    continue;
                }
                None => None,
            };

            if shm.is_none()
                && (session.selected_profile == WIN_SHM_PROFILE_HYBRID
                    || session.selected_profile == WIN_SHM_PROFILE_BUSYWAIT)
            {
                drop(session);
                continue;
            }

            let expected_method_code = self.expected_method_code;
            let handler = self.handler.clone();
            let running = self.running.clone();
            let learned_request_payload_bytes = self.learned_request_payload_bytes.clone();
            let learned_response_payload_bytes = self.learned_response_payload_bytes.clone();
            let t = std::thread::spawn(move || {
                handle_session_win_threaded(
                    session,
                    shm,
                    expected_method_code,
                    handler,
                    running,
                    learned_request_payload_bytes,
                    learned_response_payload_bytes,
                );
            });
            session_threads.push(t);
        }

        for t in session_threads {
            let _ = t.join();
        }

        Ok(())
    }

    /// Signal shutdown. On Windows, also closes the listener pipe to
    /// unblock ConnectNamedPipe in the accept loop.
    pub fn stop(&self) {
        self.running.store(false, Ordering::Release);

        #[cfg(windows)]
        {
            let mut guard = self.listener_handle.lock().unwrap();
            if let Some(h) = guard.take() {
                extern "system" {
                    fn CloseHandle(h: isize) -> i32;
                }
                unsafe {
                    CloseHandle(h as isize);
                }
            }
        }
    }

    /// Returns the internal running flag for diagnostics and test helpers.
    ///
    /// For reliable shutdown, call `stop()`. On Windows, flipping this flag
    /// alone does not wake a blocking listener accept.
    pub fn running_flag(&self) -> Arc<AtomicBool> {
        self.running.clone()
    }

    #[cfg(target_os = "linux")]
    fn prepare_unix_accept(&mut self) -> (u64, ServerConfig, Option<ShmContext>, bool) {
        let session_id = self.next_session_id;
        self.next_session_id += 1;

        let mut cfg = self.server_config.clone();
        cfg.max_request_payload_bytes = self.learned_request_payload_bytes.load(Ordering::Acquire);
        cfg.max_response_payload_bytes =
            self.learned_response_payload_bytes.load(Ordering::Acquire);

        let shm_profiles = cfg.supported_profiles & (PROFILE_SHM_HYBRID | PROFILE_SHM_FUTEX);
        if shm_profiles == 0 {
            return (session_id, cfg, None, true);
        }

        match ShmContext::server_create(
            &self.run_dir,
            &self.service_name,
            session_id,
            cfg.max_request_payload_bytes + HEADER_SIZE as u32,
            cfg.max_response_payload_bytes + HEADER_SIZE as u32,
        ) {
            Ok(ctx) => (session_id, cfg, Some(ctx), true),
            Err(_) => {
                cfg.supported_profiles &= !(PROFILE_SHM_HYBRID | PROFILE_SHM_FUTEX);
                cfg.preferred_profiles &= !(PROFILE_SHM_HYBRID | PROFILE_SHM_FUTEX);
                (session_id, cfg.clone(), None, cfg.supported_profiles != 0)
            }
        }
    }

    #[cfg(target_os = "linux")]
    fn finalize_unix_shm(
        &self,
        session: &crate::transport::posix::UdsSession,
        mut shm: Option<ShmContext>,
    ) -> Option<ShmContext> {
        let profile = session.selected_profile;
        if profile != PROFILE_SHM_HYBRID && profile != PROFILE_SHM_FUTEX {
            if let Some(ref mut ctx) = shm {
                ctx.destroy();
            }
            return None;
        }
        shm
    }

    #[cfg(windows)]
    fn prepare_windows_accept(&mut self) -> (u64, ServerConfig, Option<PreparedWinShm>, bool) {
        let session_id = self.next_session_id;
        self.next_session_id += 1;

        let mut cfg = self.server_config.clone();
        cfg.max_request_payload_bytes = self.learned_request_payload_bytes.load(Ordering::Acquire);
        cfg.max_response_payload_bytes =
            self.learned_response_payload_bytes.load(Ordering::Acquire);

        let shm_profiles =
            cfg.supported_profiles & (WIN_SHM_PROFILE_HYBRID | WIN_SHM_PROFILE_BUSYWAIT);
        if shm_profiles == 0 {
            return (session_id, cfg, None, true);
        }

        let mut prepared = PreparedWinShm::default();
        for profile in [WIN_SHM_PROFILE_HYBRID, WIN_SHM_PROFILE_BUSYWAIT] {
            if cfg.supported_profiles & profile == 0 {
                continue;
            }

            match WinShmContext::server_create(
                &self.run_dir,
                &self.service_name,
                self.server_config.auth_token,
                session_id,
                profile,
                cfg.max_request_payload_bytes + HEADER_SIZE as u32,
                cfg.max_response_payload_bytes + HEADER_SIZE as u32,
            ) {
                Ok(ctx) => prepared.insert(profile, ctx),
                Err(_) => {
                    cfg.supported_profiles &= !profile;
                    cfg.preferred_profiles &= !profile;
                }
            }
        }

        if cfg.supported_profiles == 0 {
            prepared.destroy_all();
            return (session_id, cfg, None, false);
        }

        if prepared.is_empty() {
            return (session_id, cfg, None, true);
        }

        (session_id, cfg, Some(prepared), true)
    }

    #[cfg(windows)]
    fn finalize_windows_shm(
        &self,
        session: &NpSession,
        mut prepared: Option<PreparedWinShm>,
    ) -> Option<WinShmContext> {
        let profile = session.selected_profile;
        if profile != WIN_SHM_PROFILE_HYBRID && profile != WIN_SHM_PROFILE_BUSYWAIT {
            if let Some(ref mut prepared) = prepared {
                prepared.destroy_all();
            }
            return None;
        }
        let mut prepared = prepared?;
        let selected = prepared.take(profile);
        prepared.destroy_all();
        selected
    }
}

#[cfg(windows)]
#[derive(Default)]
struct PreparedWinShm {
    hybrid: Option<WinShmContext>,
    busywait: Option<WinShmContext>,
}

#[cfg(windows)]
impl PreparedWinShm {
    fn insert(&mut self, profile: u32, ctx: WinShmContext) {
        if profile == WIN_SHM_PROFILE_HYBRID {
            self.hybrid = Some(ctx);
        } else if profile == WIN_SHM_PROFILE_BUSYWAIT {
            self.busywait = Some(ctx);
        }
    }

    fn take(&mut self, profile: u32) -> Option<WinShmContext> {
        if profile == WIN_SHM_PROFILE_HYBRID {
            self.hybrid.take()
        } else if profile == WIN_SHM_PROFILE_BUSYWAIT {
            self.busywait.take()
        } else {
            None
        }
    }

    fn destroy_all(&mut self) {
        if let Some(mut ctx) = self.hybrid.take() {
            ctx.destroy();
        }
        if let Some(mut ctx) = self.busywait.take() {
            ctx.destroy();
        }
    }

    fn is_empty(&self) -> bool {
        self.hybrid.is_none() && self.busywait.is_none()
    }
}
