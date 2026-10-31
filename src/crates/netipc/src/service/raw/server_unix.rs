use super::common::{wait_for_worker_slot, SERVER_POLL_TIMEOUT_MS, SERVER_STOP_GRACE_MS};
use super::server::{ManagedServer, ServerConfig};
use super::server_session_unix::{handle_session_threaded, poll_fd};
use crate::protocol::{NipcError, SERVER_HANDSHAKE_TIMEOUT_MS};
#[cfg(target_os = "linux")]
use crate::protocol::{HEADER_SIZE, PROFILE_SHM_FUTEX, PROFILE_SHM_HYBRID};
use crate::transport::posix::{PendingConnection, UdsListener, UdsSession};
#[cfg(target_os = "linux")]
use crate::transport::shm::ShmContext;
use std::os::unix::io::RawFd;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// Session socket of a running worker, published so a stopping server can
/// shut it down. The worker clears it before closing the socket, so a
/// shutdown never reaches a reused descriptor.
#[derive(Default)]
struct WorkerSocket(Mutex<Option<RawFd>>);

struct Worker {
    thread: std::thread::JoinHandle<()>,
    socket: Arc<WorkerSocket>,
}

impl ManagedServer {
    /// Run the acceptor loop. Blocking. Accepts clients and runs each
    /// connection, handshake included, in its own thread (up to
    /// worker_count); a connection over the limit is closed before
    /// HELLO_ACK.
    ///
    /// Returns when `stop()` is called or on fatal error. A stop before
    /// run makes run return at once; the server is single-use.
    #[cfg(unix)]
    pub fn run(&mut self) -> Result<(), NipcError> {
        // A stop before run must win; the server is single-use.
        if self.stop_requested.load(Ordering::Acquire) {
            return Ok(());
        }

        #[cfg(target_os = "linux")]
        crate::transport::shm::cleanup_stale(&self.run_dir, &self.service_name);

        let listener = UdsListener::bind(
            &self.run_dir,
            &self.service_name,
            self.server_config.clone(),
        )
        .map_err(|_| NipcError::BadLayout)?;

        self.running.store(true, Ordering::Release);
        if self.stop_requested.load(Ordering::Acquire) {
            self.running.store(false, Ordering::Release);
        }

        let mut workers: Vec<Worker> = Vec::new();

        while self.running.load(Ordering::Acquire) {
            let ready = poll_fd(listener.fd(), SERVER_POLL_TIMEOUT_MS as i32);
            if ready < 0 {
                break;
            }
            if ready == 0 {
                workers.retain(|w| !w.thread.is_finished());
                continue;
            }

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

            // A connection holds its slot from accept, handshake included.
            // Refuse before HELLO_ACK so the client's connect fails instead of
            // a later call.
            let worker_count = self.worker_count;
            if !wait_for_worker_slot(&self.running, || {
                workers.retain(|w| !w.thread.is_finished());
                workers.len() < worker_count
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
            };
            let socket = Arc::new(WorkerSocket::default());
            let worker_socket = socket.clone();
            let expected_method_code = self.expected_method_code;
            let handler = self.handler.clone();
            let running = self.running.clone();
            let stop_requested = self.stop_requested.clone();
            let learned_request_payload_bytes = self.learned_request_payload_bytes.clone();
            let learned_response_payload_bytes = self.learned_response_payload_bytes.clone();
            let request_payload_growth_ceiling = self.request_payload_growth_ceiling;
            let response_payload_growth_ceiling = self.response_payload_growth_ceiling;

            let thread = std::thread::spawn(move || {
                let Some((mut session, shm)) = accept.handshake(
                    pending,
                    &stop_requested,
                    &learned_request_payload_bytes,
                    &learned_response_payload_bytes,
                ) else {
                    return;
                };
                *worker_socket.0.lock().unwrap() = Some(session.fd());

                handle_session_threaded(
                    &mut session,
                    shm,
                    expected_method_code,
                    handler,
                    running,
                    learned_request_payload_bytes,
                    learned_response_payload_bytes,
                    request_payload_growth_ceiling,
                    response_payload_growth_ceiling,
                );

                let mut published = worker_socket.0.lock().unwrap();
                *published = None;
                drop(session);
            });
            workers.push(Worker { thread, socket });
        }

        self.stop_requested.store(true, Ordering::Release);
        self.running.store(false, Ordering::Release);
        stop_workers(workers);

        Ok(())
    }
}

/// Give workers a grace to finish their current message, then shut down the
/// sockets of the ones still blocked in send/recv, and join them all.
fn stop_workers(workers: Vec<Worker>) {
    let deadline = Instant::now() + Duration::from_millis(SERVER_STOP_GRACE_MS);
    while Instant::now() < deadline && workers.iter().any(|w| !w.thread.is_finished()) {
        std::thread::sleep(Duration::from_millis(5));
    }

    for w in &workers {
        if w.thread.is_finished() {
            continue;
        }
        if let Some(fd) = *w.socket.0.lock().unwrap() {
            unsafe {
                libc::shutdown(fd, libc::SHUT_RDWR);
            }
        }
    }

    for w in workers {
        let _ = w.thread.join();
    }
}

/// What a worker needs to run the server side of one handshake.
struct AcceptContext {
    run_dir: String,
    service_name: String,
    base_config: ServerConfig,
    session_id: u64,
}

#[cfg(target_os = "linux")]
type WorkerShm = Option<ShmContext>;
#[cfg(not(target_os = "linux"))]
type WorkerShm = Option<()>;

impl AcceptContext {
    /// Prepare the per-session config and SHM, then handshake. Returns `None`
    /// if the session must not start.
    fn handshake(
        self,
        pending: PendingConnection,
        stop_requested: &AtomicBool,
        learned_request_payload_bytes: &AtomicU32,
        learned_response_payload_bytes: &AtomicU32,
    ) -> Option<(UdsSession, WorkerShm)> {
        let mut cfg = self.base_config.clone();
        cfg.max_request_payload_bytes = learned_request_payload_bytes.load(Ordering::Acquire);
        cfg.max_response_payload_bytes = learned_response_payload_bytes.load(Ordering::Acquire);

        #[cfg(target_os = "linux")]
        let precreated_shm = self.prepare_shm(&mut cfg)?;
        #[cfg(not(target_os = "linux"))]
        let precreated_shm: WorkerShm = None;

        let session = match pending.handshake(
            self.session_id,
            &cfg,
            SERVER_HANDSHAKE_TIMEOUT_MS,
            Some(stop_requested),
        ) {
            Ok(session) => session,
            Err(_) => {
                #[cfg(target_os = "linux")]
                if let Some(mut shm) = precreated_shm {
                    shm.destroy();
                }
                return None;
            }
        };

        #[cfg(target_os = "linux")]
        {
            let profile = session.selected_profile;
            let wants_shm = profile == PROFILE_SHM_HYBRID || profile == PROFILE_SHM_FUTEX;
            match (wants_shm, precreated_shm) {
                (true, Some(shm)) => Some((session, Some(shm))),
                // SHM profile guarantee: only negotiate SHM for sessions that
                // already have a prepared per-session SHM region.
                (true, None) => None,
                (false, shm) => {
                    if let Some(mut shm) = shm {
                        shm.destroy();
                    }
                    Some((session, None))
                }
            }
        }
        #[cfg(not(target_os = "linux"))]
        Some((session, precreated_shm))
    }

    /// Create the session's SHM region before HELLO is read, or drop the SHM
    /// profiles if that fails. Returns `None` if no profile is left.
    #[cfg(target_os = "linux")]
    fn prepare_shm(&self, cfg: &mut ServerConfig) -> Option<Option<ShmContext>> {
        let shm_profiles = cfg.supported_profiles & (PROFILE_SHM_HYBRID | PROFILE_SHM_FUTEX);
        if shm_profiles == 0 {
            return Some(None);
        }

        match ShmContext::server_create(
            &self.run_dir,
            &self.service_name,
            self.session_id,
            cfg.max_request_payload_bytes + HEADER_SIZE as u32,
            cfg.max_response_payload_bytes + HEADER_SIZE as u32,
        ) {
            Ok(ctx) => Some(Some(ctx)),
            Err(_) => {
                cfg.supported_profiles &= !(PROFILE_SHM_HYBRID | PROFILE_SHM_FUTEX);
                cfg.preferred_profiles &= !(PROFILE_SHM_HYBRID | PROFILE_SHM_FUTEX);
                (cfg.supported_profiles != 0).then_some(None)
            }
        }
    }
}
