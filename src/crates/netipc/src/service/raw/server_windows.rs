use super::common::wait_for_worker_slot;
#[cfg(test)]
use super::server::TestHooks;
use super::server::{ManagedServer, ServerConfig};
use super::server_session_windows::handle_session_win_threaded;
use crate::protocol::{NipcError, HEADER_SIZE};
use crate::transport::win_shm::{
    WinShmContext, PROFILE_BUSYWAIT as WIN_SHM_PROFILE_BUSYWAIT,
    PROFILE_HYBRID as WIN_SHM_PROFILE_HYBRID,
};
use crate::transport::windows::{NpListener, NpSession, PendingPipe};
use std::os::windows::io::AsRawHandle;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

unsafe extern "system" {
    fn CancelSynchronousIo(thread: isize) -> i32;
}

impl ManagedServer {
    /// Windows: run the acceptor loop over Named Pipes.
    pub fn run(&mut self) -> Result<(), NipcError> {
        // A stop before run must win; the server is single-use.
        if self.stop_requested.load(Ordering::Acquire) {
            return Ok(());
        }

        let mut listener = NpListener::bind(
            &self.run_dir,
            &self.service_name,
            self.server_config.clone(),
        )
        .map_err(|_| NipcError::BadLayout)?;

        *self.listener_waker.lock().unwrap() = Some(listener.waker());

        self.running.store(true, Ordering::Release);
        if self.stop_requested.load(Ordering::Acquire) {
            self.running.store(false, Ordering::Release);
        }
        self.listening.set();

        let mut session_threads: Vec<JoinHandle<()>> = Vec::new();

        while self.running.load(Ordering::Acquire) {
            // The handshake runs on the worker, so a client that never sends
            // HELLO cannot stall this loop.
            let pending = match listener.accept_pending() {
                Ok(p) => p,
                Err(_) => {
                    if !self.running.load(Ordering::Acquire) {
                        break;
                    }
                    std::thread::sleep(std::time::Duration::from_millis(10));
                    continue;
                }
            };

            // Stop wakes ConnectNamedPipe with a throwaway connection.
            if !self.running.load(Ordering::Acquire) {
                break;
            }

            // A connection holds its slot from accept, handshake included.
            // Refuse before HELLO_ACK so the client's connect fails instead of
            // a later call.
            let worker_count = self.worker_count;
            if !wait_for_worker_slot(&self.running, self.timings.slot_wait(), || {
                session_threads.retain(|t| !t.is_finished());
                session_threads.len() < worker_count
            }) {
                drop(pending);
                continue;
            }

            let session_id = self.next_session_id;
            self.next_session_id += 1;
            let accept = AcceptContext {
                run_dir: self.run_dir.clone(),
                service_name: self.service_name.clone(),
                base_config: self.server_config.clone(),
                session_id,
                handshake_ms: self.timings.handshake_ms,
            };
            let expected_method_code = self.expected_method_code;
            let handler = self.handler.clone();
            let running = self.running.clone();
            let poll_ms = self.timings.poll_ms;
            #[cfg(test)]
            let hooks = self.test_hooks.clone();
            let stop_requested = self.stop_requested.clone();
            let learned_request_payload_bytes = self.learned_request_payload_bytes.clone();
            let learned_response_payload_bytes = self.learned_response_payload_bytes.clone();
            let request_payload_growth_ceiling = self.request_payload_growth_ceiling;
            let response_payload_growth_ceiling = self.response_payload_growth_ceiling;
            let t = std::thread::spawn(move || {
                #[cfg(test)]
                let _exit = SessionExitNotice(hooks.session_exited.clone());
                #[cfg(test)]
                TestHooks::notify(&hooks.handshake_started);
                let Some((session, shm)) = accept.handshake(
                    pending,
                    &stop_requested,
                    &learned_request_payload_bytes,
                    &learned_response_payload_bytes,
                ) else {
                    return;
                };
                handle_session_win_threaded(
                    session,
                    shm,
                    expected_method_code,
                    handler,
                    running,
                    poll_ms,
                    learned_request_payload_bytes,
                    learned_response_payload_bytes,
                    request_payload_growth_ceiling,
                    response_payload_growth_ceiling,
                );
            });
            session_threads.push(t);
        }

        *self.listener_waker.lock().unwrap() = None;
        self.stop_requested.store(true, Ordering::Release);
        self.running.store(false, Ordering::Release);
        drop(listener);
        stop_workers(session_threads, self.timings.stop_grace);

        Ok(())
    }
}

/// Give workers a grace to finish their current message, then cancel the
/// pipe I/O of the ones still blocked until they exit, and join them all. A
/// single cancel can land before a thread enters its next blocking call, so
/// it repeats.
fn stop_workers(workers: Vec<JoinHandle<()>>, grace: Duration) {
    let deadline = Instant::now() + grace;
    while Instant::now() < deadline && workers.iter().any(|t| !t.is_finished()) {
        std::thread::sleep(Duration::from_millis(5));
    }

    while workers.iter().any(|t| !t.is_finished()) {
        for t in workers.iter().filter(|t| !t.is_finished()) {
            // The JoinHandle keeps the thread handle valid until join.
            unsafe {
                CancelSynchronousIo(t.as_raw_handle() as isize);
            }
        }
        std::thread::sleep(Duration::from_millis(10));
    }

    for t in workers {
        let _ = t.join();
    }
}

/// What a worker needs to run the server side of one handshake.
struct AcceptContext {
    run_dir: String,
    service_name: String,
    base_config: ServerConfig,
    session_id: u64,
    handshake_ms: u32,
}

/// Reports a session worker's exit, after its pipe closed, however the
/// worker returns.
#[cfg(test)]
struct SessionExitNotice(Option<std::sync::mpsc::Sender<()>>);

#[cfg(test)]
impl Drop for SessionExitNotice {
    fn drop(&mut self) {
        TestHooks::notify(&self.0);
    }
}

impl AcceptContext {
    /// Prepare the per-session config and SHM, then handshake. Returns `None`
    /// if the session must not start.
    fn handshake(
        self,
        pending: PendingPipe,
        stop_requested: &AtomicBool,
        learned_request_payload_bytes: &AtomicU32,
        learned_response_payload_bytes: &AtomicU32,
    ) -> Option<(NpSession, Option<WinShmContext>)> {
        let mut cfg = self.base_config.clone();
        cfg.max_request_payload_bytes = learned_request_payload_bytes.load(Ordering::Acquire);
        cfg.max_response_payload_bytes = learned_response_payload_bytes.load(Ordering::Acquire);

        let mut prepared = self.prepare_shm(&mut cfg)?;

        let session = match pending.handshake(
            self.session_id,
            &cfg,
            self.handshake_ms,
            Some(stop_requested),
        ) {
            Ok(session) => session,
            Err(_) => {
                prepared.destroy_all();
                return None;
            }
        };

        let profile = session.selected_profile;
        if profile != WIN_SHM_PROFILE_HYBRID && profile != WIN_SHM_PROFILE_BUSYWAIT {
            prepared.destroy_all();
            return Some((session, None));
        }
        // Keep the negotiated context and destroy every unused prepared
        // context. SHM profile guarantee: no prepared context, no session.
        let selected = prepared.take(profile);
        prepared.destroy_all();
        selected.map(|shm| (session, Some(shm)))
    }

    /// Create the session's SHM objects before HELLO is read, dropping each
    /// profile whose objects fail. Returns `None` if no profile is left.
    fn prepare_shm(&self, cfg: &mut ServerConfig) -> Option<PreparedWinShm> {
        let mut prepared = PreparedWinShm::default();
        let shm_profiles =
            cfg.supported_profiles & (WIN_SHM_PROFILE_HYBRID | WIN_SHM_PROFILE_BUSYWAIT);
        if shm_profiles == 0 {
            return Some(prepared);
        }

        for profile in [WIN_SHM_PROFILE_HYBRID, WIN_SHM_PROFILE_BUSYWAIT] {
            if cfg.supported_profiles & profile == 0 {
                continue;
            }

            match WinShmContext::server_create(
                &self.run_dir,
                &self.service_name,
                self.base_config.auth_token,
                self.session_id,
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
            return None;
        }

        Some(prepared)
    }
}

#[derive(Default)]
struct PreparedWinShm {
    hybrid: Option<WinShmContext>,
    busywait: Option<WinShmContext>,
}

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
}
