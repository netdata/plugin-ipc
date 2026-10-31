//! Robustness reproduction tests (SOW-0040), Windows.
//!
//! Each test asserts the correct behavior and runs every blocking step on a
//! helper thread under a watchdog, so a regression fails instead of hanging.
//! Bounds are deliberately lenient so they do not dictate a fix design.
//!
//! Server shutdown follows this module's `TestServer::stop` pattern: clear
//! the running flag, then make a wake-up connection, because `run()` holds
//! `&mut self` and `stop()` cannot be called from another thread meanwhile.

use super::*;
use crate::protocol::{string_reverse_encode, STRING_REVERSE_HDR_SIZE};
use std::io::{BufRead, BufReader};
use std::process::{Child, Command, Stdio};

const HELPER_ENV: &str = "NETIPC_ROBUSTNESS_HELPER";
const HELPER_TEST: &str = "service::raw::windows_tests::robustness::robustness_helper_process";
const GENERIC_READ: Dword = 0x8000_0000;
const GENERIC_WRITE: Dword = 0x4000_0000;
const OPEN_EXISTING: Dword = 3;
const FILE_FLAG_FIRST_PIPE_INSTANCE: Dword = 0x0008_0000;

unsafe extern "system" {
    fn CreateFileW(
        name: *const u16,
        access: Dword,
        share: Dword,
        security: *const core::ffi::c_void,
        disposition: Dword,
        flags: Dword,
        template: RawHandle,
    ) -> RawHandle;
}

/// Run `f` on a thread; return its result, or None if it did not finish in time.
fn within<T: Send + 'static>(
    limit: Duration,
    f: impl FnOnce() -> T + Send + 'static,
) -> (Option<T>, thread::JoinHandle<()>) {
    let (tx, rx) = mpsc::channel();
    let handle = thread::spawn(move || {
        let _ = tx.send(f());
    });
    (rx.recv_timeout(limit).ok(), handle)
}

fn served_within(service: &str, config: ClientConfig, limit: Duration) -> Result<(), String> {
    let deadline = Instant::now() + limit;
    let mut last = String::from("never connected");
    while Instant::now() < deadline {
        let mut client = increment_client(service, config.clone());
        client.refresh();
        if client.ready() {
            match client.call_increment_with_timeout(1, 1000) {
                Ok(2) => {
                    client.close();
                    return Ok(());
                }
                Ok(v) => last = format!("unexpected value {v}"),
                Err(e) => last = format!("call error {e:?}"),
            }
        } else {
            last = format!("state {:?}", client.state);
        }
        client.close();
        thread::sleep(Duration::from_millis(50));
    }
    Err(last)
}

struct RobustServer {
    service: String,
    wake: ClientConfig,
    running: Arc<AtomicBool>,
    thread: thread::JoinHandle<()>,
}

fn start_server(
    service: &str,
    config: ServerConfig,
    wake: ClientConfig,
    method: u16,
    handler: DispatchHandler,
    workers: usize,
) -> RobustServer {
    ensure_run_dir();
    let mut server = ManagedServer::with_workers(
        TEST_RUN_DIR,
        service,
        config,
        method,
        Some(handler),
        workers,
    );
    let running = server.running_flag();
    let thread = thread::spawn(move || {
        let _ = server.run();
    });
    thread::sleep(Duration::from_millis(100));
    RobustServer {
        service: service.to_string(),
        wake,
        running,
        thread,
    }
}

impl RobustServer {
    /// Request shutdown and report whether `run()` returned within `limit`.
    /// On failure the server thread is left behind; callers release the
    /// blocker and call `finish`.
    fn stop_within(self, limit: Duration) -> (bool, thread::JoinHandle<()>) {
        self.running.store(false, Ordering::Release);
        let (svc, wake, server_thread) = (self.service, self.wake, self.thread);
        let (done, waiter) = within(limit, move || {
            let _ = NpSession::connect(TEST_RUN_DIR, &svc, &wake);
            let _ = server_thread.join();
        });
        (done.is_some(), waiter)
    }
}

fn open_pipe_silently(service: &str) -> RawHandle {
    let name = build_pipe_name(TEST_RUN_DIR, service).expect("pipe name");
    for _ in 0..100 {
        let h = unsafe {
            CreateFileW(
                name.as_ptr(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                ptr::null(),
                OPEN_EXISTING,
                0,
                0,
            )
        };
        if h != INVALID_HANDLE_VALUE {
            return h;
        }
        thread::sleep(Duration::from_millis(20));
    }
    panic!("could not open pipe for {service}");
}

fn spawn_helper(mode: &str, service: &str) -> Child {
    let mut child = Command::new(std::env::current_exe().expect("current_exe"))
        .args([HELPER_TEST, "--exact", "--nocapture", "--test-threads=1"])
        .env(HELPER_ENV, format!("{mode}:{service}"))
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .expect("spawn helper");
    let stdout = child.stdout.take().expect("helper stdout");
    let (ready, _h) = within(Duration::from_secs(15), move || {
        BufReader::new(stdout)
            .lines()
            .map_while(Result::ok)
            .any(|l| l.contains("HELPER_READY"))
    });
    if ready != Some(true) {
        let _ = child.kill();
        let _ = child.wait();
        panic!("helper {mode} did not become ready");
    }
    child
}

fn slow_increment_dispatch(delay: Duration) -> DispatchHandler {
    increment_dispatch(Arc::new(move |v| {
        thread::sleep(delay);
        Some(v + 1)
    }))
}

/// Child-process entry point. Inert unless spawned by `spawn_helper`.
#[test]
fn robustness_helper_process() {
    let Ok(spec) = std::env::var(HELPER_ENV) else {
        return;
    };
    let (mode, service) = spec.split_once(':').expect("helper spec");
    match mode {
        "shm_client" => {
            let mut client = increment_client(service, shm_client_config());
            connect_ready(&mut client);
            assert_eq!(client.call_increment_with_timeout(1, 2000), Ok(2));
            println!("HELPER_READY");
            thread::sleep(Duration::from_secs(600));
        }
        "shm_server" => {
            let _server = start_server(
                service,
                shm_server_config(),
                shm_client_config(),
                METHOD_INCREMENT,
                slow_increment_dispatch(Duration::from_secs(60)),
                2,
            );
            println!("HELPER_READY");
            thread::sleep(Duration::from_secs(600));
        }
        other => panic!("unknown helper mode {other}"),
    }
}

// K2: SHM server must release a session once its client crashes.
#[test]
fn test_robust_win_shm_session_released_after_client_crash() {
    let svc = unique_service("rs_wrob_shm_crash");
    let server = start_server(
        &svc,
        shm_server_config(),
        shm_client_config(),
        METHOD_INCREMENT,
        increment_dispatch_handler(),
        2,
    );

    for _ in 0..2 {
        let mut child = spawn_helper("shm_client", &svc);
        let _ = child.kill(); // TerminateProcess
        let _ = child.wait();
    }
    let served = served_within(&svc, shm_client_config(), Duration::from_secs(8));

    let (_, waiter) = server.stop_within(Duration::from_secs(5));
    let _ = waiter.join();
    assert!(
        served.is_ok(),
        "after 2 crashed SHM clients on 2 workers, a new client must be served: {served:?}"
    );
}

// K3: a client that opens the pipe but never sends HELLO.
#[test]
fn test_robust_win_silent_client_does_not_block_other_clients() {
    let svc = unique_service("rs_wrob_silent");
    let server = start_server(
        &svc,
        server_config(),
        client_config(),
        METHOD_INCREMENT,
        increment_dispatch_handler(),
        4,
    );

    let silent = open_pipe_silently(&svc);
    thread::sleep(Duration::from_millis(100));
    let svc2 = svc.clone();
    let (served, worker) = within(Duration::from_secs(5), move || {
        served_within(&svc2, client_config(), Duration::from_secs(5))
    });

    unsafe { CloseHandle(silent) };
    let _ = worker.join();
    let (_, waiter) = server.stop_within(Duration::from_secs(5));
    let _ = waiter.join();
    assert!(
        matches!(served, Some(Ok(()))),
        "a legitimate client must be served within 5s while a silent client is connected: {served:?}"
    );
}

/// A silent client holds a worker only until the server handshake deadline.
#[test]
fn test_robust_win_silent_client_released_at_handshake_deadline() {
    let svc = unique_service("rs_wrob_silent_deadline");
    let server = start_server(
        &svc,
        server_config(),
        client_config(),
        METHOD_INCREMENT,
        increment_dispatch_handler(),
        1,
    );

    let silent = open_pipe_silently(&svc);
    let limit =
        Duration::from_millis(u64::from(crate::protocol::SERVER_HANDSHAKE_TIMEOUT_MS) + 4000);
    let served = served_within(&svc, client_config(), limit);

    unsafe { CloseHandle(silent) };
    let (_, waiter) = server.stop_within(Duration::from_secs(5));
    let _ = waiter.join();
    assert!(
        served.is_ok(),
        "the only worker must be freed once the silent client misses the handshake deadline: {served:?}"
    );
}

#[test]
fn test_robust_win_silent_client_does_not_block_shutdown() {
    let svc = unique_service("rs_wrob_silent_stop");
    let server = start_server(
        &svc,
        server_config(),
        client_config(),
        METHOD_INCREMENT,
        increment_dispatch_handler(),
        4,
    );

    let silent = open_pipe_silently(&svc);
    thread::sleep(Duration::from_millis(100));
    let (stopped, waiter) = server.stop_within(Duration::from_secs(3));

    unsafe { CloseHandle(silent) };
    let _ = waiter.join();
    assert!(
        stopped,
        "server run() must return within 3s of shutdown while a silent client is connected"
    );
}

// K4: a client call must be bounded even when the server never answers HELLO.
#[test]
fn test_robust_win_client_bounded_when_server_never_sends_hello_ack() {
    let svc = unique_service("rs_wrob_nohelloack");
    let name = build_pipe_name(TEST_RUN_DIR, &svc).expect("pipe name");
    let pipe = unsafe {
        CreateNamedPipeW(
            name.as_ptr(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            4,
            RAW_PIPE_PACKET_SIZE,
            RAW_PIPE_PACKET_SIZE,
            0,
            ptr::null(),
        )
    };
    assert_ne!(pipe, INVALID_HANDLE_VALUE, "fake server pipe");

    let svc2 = svc.clone();
    let (result, worker) = within(Duration::from_secs(6), move || {
        let mut client = increment_client(&svc2, client_config());
        client.set_call_timeout(1000);
        client.refresh();
        let r = client.call_increment_with_timeout(1, 1000);
        client.close();
        r
    });

    unsafe { CloseHandle(pipe) };
    let _ = worker.join();
    assert!(
        result.is_some(),
        "refresh + call with a 1s timeout must return within 6s when the server never sends HELLO_ACK"
    );
}

// N1: the configured call timeout must be honored on the named-pipe path.
#[test]
fn test_robust_win_pipe_call_honors_timeout_longer_than_100ms() {
    let svc = unique_service("rs_wrob_slow_handler");
    let server = start_server(
        &svc,
        server_config(),
        client_config(),
        METHOD_INCREMENT,
        slow_increment_dispatch(Duration::from_millis(300)),
        2,
    );

    let mut client = increment_client(&svc, client_config());
    connect_ready(&mut client);
    let start = Instant::now();
    let result = client.call_increment_with_timeout(41, 2000);
    let elapsed = start.elapsed();
    client.close();

    let (_, waiter) = server.stop_within(Duration::from_secs(5));
    let _ = waiter.join();
    assert_eq!(
        result,
        Ok(42),
        "a 300ms handler must succeed with a 2000ms timeout (failed after {elapsed:?})"
    );
}

// N2: shutdown must not wait forever on a worker blocked writing a response
// to a client that stopped reading.
#[test]
fn test_robust_win_shutdown_not_blocked_by_non_reading_client() {
    const BIG: u32 = 8 * 1024 * 1024;
    let svc = unique_service("rs_wrob_noread");
    let mut scfg = server_config();
    scfg.max_request_payload_bytes = BIG;
    scfg.max_response_payload_bytes = BIG;
    let mut ccfg = client_config();
    ccfg.max_request_payload_bytes = BIG;
    ccfg.max_response_payload_bytes = BIG;
    let handler = string_reverse_dispatch(Arc::new(|s: &str| Some(s.chars().rev().collect())));
    let server = start_server(&svc, scfg, ccfg.clone(), METHOD_STRING_REVERSE, handler, 2);

    let mut session = NpSession::connect(TEST_RUN_DIR, &svc, &ccfg).expect("connect");
    let text = "x".repeat(4 * 1024 * 1024);
    let mut payload = vec![0u8; STRING_REVERSE_HDR_SIZE + text.len() + 1];
    let n = string_reverse_encode(text.as_bytes(), &mut payload);
    assert!(n > 0, "encode request");
    let mut hdr = Header {
        kind: KIND_REQUEST,
        code: METHOD_STRING_REVERSE,
        item_count: 1,
        message_id: 1,
        ..Header::default()
    };
    session.send(&mut hdr, &payload[..n]).expect("send request");
    // Never read the response; give the worker time to block in WriteFile.
    thread::sleep(Duration::from_millis(500));

    let (stopped, waiter) = server.stop_within(Duration::from_secs(3));
    drop(session);
    let _ = waiter.join();
    assert!(
        stopped,
        "server run() must return within 3s of shutdown while a worker is blocked writing"
    );
}

// N3: stop() issued before run() must not be lost.
#[test]
fn test_robust_win_stop_before_run_is_honored() {
    let svc = unique_service("rs_wrob_stop_first");
    ensure_run_dir();
    let mut server = ManagedServer::with_workers(
        TEST_RUN_DIR,
        &svc,
        server_config(),
        METHOD_INCREMENT,
        Some(increment_dispatch_handler()),
        2,
    );
    server.stop();
    let flag = server.running_flag();
    let (returned, worker) = within(Duration::from_secs(2), move || {
        let _ = server.run();
    });

    if returned.is_none() {
        flag.store(false, Ordering::Release);
        let _ = NpSession::connect(TEST_RUN_DIR, &svc, &client_config());
    }
    let _ = worker.join();
    assert!(
        returned.is_some(),
        "run() after stop() must return promptly instead of serving forever"
    );
}

// N5: an SHM client must notice a crashed server instead of waiting out its
// whole timeout.
#[test]
fn test_robust_win_shm_client_notices_crashed_server() {
    let svc = unique_service("rs_wrob_dead_server");
    let mut child = spawn_helper("shm_server", &svc);

    let svc2 = svc.clone();
    let (call_tx, call_rx) = mpsc::channel();
    let caller = thread::spawn(move || {
        let mut client = increment_client(&svc2, shm_client_config());
        connect_ready(&mut client);
        let _ = call_tx.send(());
        let start = Instant::now();
        let r = client.call_increment_with_timeout(1, 10_000);
        let elapsed = start.elapsed();
        client.close();
        (r, elapsed)
    });
    call_rx
        .recv_timeout(Duration::from_secs(10))
        .expect("client connected");
    thread::sleep(Duration::from_millis(500)); // request parked in the stalled handler
    let _ = child.kill();
    let _ = child.wait();

    let (result, elapsed) = caller.join().expect("caller");
    assert!(
        result.is_err() && elapsed < Duration::from_millis(3500),
        "call must fail within 3s of the server crash (timeout is 10s): {result:?} after {elapsed:?}"
    );
    assert_eq!(
        result,
        Err(NipcError::Disconnected),
        "call must report the server as disconnected"
    );
}

// A server at worker capacity completes the handshake, then closes the
// session and destroys the prepared WinSHM objects. The client must not spend
// its attach retry budget (5 s) on objects that will never appear, and must
// keep WinSHM enabled for its next connect.
#[test]
fn test_robust_win_shm_attach_stops_when_server_closes_session() {
    let svc = unique_service("rs_wrob_attach_refused");
    let server = start_server(
        &svc,
        shm_server_config(),
        shm_client_config(),
        METHOD_INCREMENT,
        increment_dispatch_handler(),
        1,
    );

    let mut holder = RawClient::new_increment(TEST_RUN_DIR, &svc, shm_client_config());
    for _ in 0..200 {
        holder.refresh();
        if holder.ready() {
            break;
        }
        thread::sleep(Duration::from_millis(10));
    }
    let holder_ready = holder.ready();

    let mut refused = RawClient::new_increment(TEST_RUN_DIR, &svc, shm_client_config());
    let start = Instant::now();
    refused.refresh();
    let elapsed = start.elapsed();

    holder.close();
    let mut ready = false;
    for _ in 0..300 {
        refused.refresh();
        if refused.ready() {
            ready = true;
            break;
        }
        thread::sleep(Duration::from_millis(10));
    }
    let uses_shm = refused.shm.is_some();
    refused.close();
    let (_, waiter) = server.stop_within(Duration::from_secs(5));
    let _ = waiter.join();

    assert!(holder_ready, "first client must hold the only worker");
    assert!(
        elapsed < Duration::from_secs(1),
        "refused connect must return within 1s (attach retry budget is 5s): {elapsed:?}"
    );
    assert!(ready, "refused client must connect once the worker is free");
    assert!(uses_shm, "refused client must still use WinSHM");
}
