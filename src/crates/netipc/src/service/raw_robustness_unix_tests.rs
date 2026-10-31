//! Robustness reproduction tests (SOW-0040).
//!
//! Each test asserts the correct behavior and runs every blocking step on a
//! helper thread under a watchdog, so a regression fails instead of hanging.
//! Bounds are deliberately lenient so they do not dictate a fix design.

use super::*;
use crate::protocol::{string_reverse_encode, METHOD_STRING_REVERSE, STRING_REVERSE_HDR_SIZE};
use std::io::{BufRead, BufReader};
use std::process::{Child, Command, Stdio};

const HELPER_ENV: &str = "NETIPC_ROBUSTNESS_HELPER";
const HELPER_TEST: &str = "service::raw::tests::robustness::robustness_helper_process";

/// Run `f` on a thread; return its result, or None if it did not finish in time.
/// On timeout the thread is left running; callers release whatever blocks it.
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

/// Connect and complete one increment call, retrying until `limit` elapses.
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

fn shm_files(service: &str) -> Vec<String> {
    let prefix = format!("{service}-");
    std::fs::read_dir(TEST_RUN_DIR)
        .map(|rd| {
            rd.filter_map(|e| e.ok())
                .map(|e| e.file_name().to_string_lossy().into_owned())
                .filter(|n| n.starts_with(&prefix) && n.ends_with(".ipcshm"))
                .collect()
        })
        .unwrap_or_default()
}

fn wait_no_shm_files(service: &str, limit: Duration) -> Vec<String> {
    let deadline = Instant::now() + limit;
    loop {
        let files = shm_files(service);
        if files.is_empty() || Instant::now() >= deadline {
            return files;
        }
        thread::sleep(Duration::from_millis(50));
    }
}

/// Connect a raw SOCK_SEQPACKET socket to the service without sending HELLO.
fn silent_connect(service: &str) -> RawFd {
    let path = socket_path(service);
    let bytes = path.as_os_str().as_bytes();
    let fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET, 0) };
    assert!(fd >= 0, "socket: {}", std::io::Error::last_os_error());
    let mut addr: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    addr.sun_family = libc::AF_UNIX as libc::sa_family_t;
    for (i, b) in bytes.iter().enumerate() {
        addr.sun_path[i] = *b as libc::c_char;
    }
    let rc = unsafe {
        libc::connect(
            fd,
            &addr as *const libc::sockaddr_un as *const libc::sockaddr,
            std::mem::size_of::<libc::sockaddr_un>() as libc::socklen_t,
        )
    };
    assert_eq!(rc, 0, "connect: {}", std::io::Error::last_os_error());
    fd
}

/// Start a managed server on its own thread. Returns the running flag used to
/// stop it (on POSIX, `stop()` only clears this flag) and the thread handle.
fn start_server(
    service: &str,
    config: ServerConfig,
    method: u16,
    handler: DispatchHandler,
    workers: usize,
) -> (Arc<AtomicBool>, thread::JoinHandle<()>) {
    ensure_run_dir();
    cleanup_all(service);
    let mut server = ManagedServer::with_workers(
        TEST_RUN_DIR,
        service,
        config,
        method,
        Some(handler),
        workers,
    );
    let running = server.running_flag();
    let handle = thread::spawn(move || {
        let _ = server.run();
    });
    wait_for_listener_bind(service);
    (running, handle)
}

fn stop(running: &AtomicBool) {
    running.store(false, Ordering::Release);
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
    let (ready, _h) = within(Duration::from_secs(10), move || {
        BufReader::new(stdout)
            .lines()
            .map_while(Result::ok)
            .any(|l| l.contains("HELPER_READY"))
    });
    assert_eq!(ready, Some(true), "helper {mode} did not become ready");
    child
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
        "shm_server" | "uds_server" => {
            let handler = increment_dispatch(Arc::new(|v| {
                thread::sleep(Duration::from_secs(60));
                Some(v + 1)
            }));
            let config = if mode == "shm_server" {
                shm_server_config()
            } else {
                server_config()
            };
            let _server = start_server(service, config, METHOD_INCREMENT, handler, 2);
            println!("HELPER_READY");
            thread::sleep(Duration::from_secs(600));
        }
        other => panic!("unknown helper mode {other}"),
    }
}

// ---------------------------------------------------------------------------
// K1: SHM server must release a session once its client leaves.
// ---------------------------------------------------------------------------

#[test]
fn test_robust_shm_sessions_released_after_clean_close() {
    let svc = unique_service("rs_rob_shm_clean");
    let (server, handle) = start_server(
        &svc,
        shm_server_config(),
        METHOD_INCREMENT,
        increment_dispatch_handler(),
        2,
    );

    let mut failures = Vec::new();
    for i in 0..5 {
        if let Err(e) = served_within(&svc, shm_client_config(), Duration::from_secs(3)) {
            failures.push(format!("client {i}: {e}"));
        }
    }
    let leftover = wait_no_shm_files(&svc, Duration::from_secs(2));

    stop(&server);
    let _ = handle.join();
    cleanup_all(&svc);
    assert!(
        failures.is_empty(),
        "with 2 workers, sequential SHM clients must all be served: {failures:?}"
    );
    assert!(leftover.is_empty(), "SHM regions leaked: {leftover:?}");
}

#[test]
fn test_robust_shm_sessions_released_after_client_crash() {
    let svc = unique_service("rs_rob_shm_crash");
    let (server, handle) = start_server(
        &svc,
        shm_server_config(),
        METHOD_INCREMENT,
        increment_dispatch_handler(),
        2,
    );

    for _ in 0..2 {
        let mut child = spawn_helper("shm_client", &svc);
        let _ = child.kill(); // SIGKILL
        let _ = child.wait();
    }
    let served = served_within(&svc, shm_client_config(), Duration::from_secs(3));
    let leftover = wait_no_shm_files(&svc, Duration::from_secs(2));

    stop(&server);
    let _ = handle.join();
    cleanup_all(&svc);
    assert!(
        served.is_ok(),
        "after 2 crashed SHM clients on 2 workers, a new client must be served: {served:?}"
    );
    assert!(leftover.is_empty(), "SHM regions leaked: {leftover:?}");
}

// ---------------------------------------------------------------------------
// K3: a client that connects but never sends HELLO must not block others or
// shutdown.
// ---------------------------------------------------------------------------

#[test]
fn test_robust_silent_client_does_not_block_other_clients() {
    let svc = unique_service("rs_rob_silent");
    let (server, handle) = start_server(
        &svc,
        server_config(),
        METHOD_INCREMENT,
        increment_dispatch_handler(),
        4,
    );

    let silent = silent_connect(&svc);
    thread::sleep(Duration::from_millis(100));
    let svc2 = svc.clone();
    let (served, worker) = within(Duration::from_secs(5), move || {
        served_within(&svc2, client_config(), Duration::from_secs(5))
    });

    unsafe { libc::close(silent) };
    let _ = worker.join();
    stop(&server);
    let _ = handle.join();
    cleanup_all(&svc);
    assert!(
        matches!(served, Some(Ok(()))),
        "a legitimate client must be served within 5s while a silent client is connected: {served:?}"
    );
}

/// A silent client holds a worker only until the server handshake deadline.
#[test]
fn test_robust_silent_client_released_at_handshake_deadline() {
    let svc = unique_service("rs_rob_silent_deadline");
    let (server, handle) = start_server(
        &svc,
        server_config(),
        METHOD_INCREMENT,
        increment_dispatch_handler(),
        1,
    );

    let silent = silent_connect(&svc);
    let limit =
        Duration::from_millis(u64::from(crate::protocol::SERVER_HANDSHAKE_TIMEOUT_MS) + 4000);
    let served = served_within(&svc, client_config(), limit);

    let mut ch = 0u8;
    let n = unsafe { libc::recv(silent, (&mut ch as *mut u8).cast(), 1, libc::MSG_DONTWAIT) };
    unsafe { libc::close(silent) };
    stop(&server);
    let _ = handle.join();
    cleanup_all(&svc);
    assert!(
        served.is_ok(),
        "the only worker must be freed once the silent client misses the handshake deadline: {served:?}"
    );
    assert_eq!(
        n, 0,
        "server must close the silent connection at the deadline"
    );
}

#[test]
fn test_robust_silent_client_does_not_block_shutdown() {
    let svc = unique_service("rs_rob_silent_stop");
    let (server, handle) = start_server(
        &svc,
        server_config(),
        METHOD_INCREMENT,
        increment_dispatch_handler(),
        4,
    );

    let silent = silent_connect(&svc);
    thread::sleep(Duration::from_millis(100));
    stop(&server);
    let (joined, waiter) = within(Duration::from_secs(3), move || handle.join().is_ok());

    unsafe { libc::close(silent) };
    let _ = waiter.join();
    cleanup_all(&svc);
    assert!(
        joined.is_some(),
        "server run() must return within 3s of stop() while a silent client is connected"
    );
}

// ---------------------------------------------------------------------------
// K4: a client call must be bounded even when the server never answers HELLO.
// ---------------------------------------------------------------------------

#[test]
fn test_robust_client_bounded_when_server_never_accepts() {
    let svc = unique_service("rs_rob_noaccept");
    ensure_run_dir();
    cleanup_all(&svc);
    let listener = raw_listener_fd_for_service(&svc);

    let svc2 = svc.clone();
    let start = Instant::now();
    let (result, worker) = within(Duration::from_secs(6), move || {
        let mut client = increment_client(&svc2, client_config());
        client.set_call_timeout(1000);
        client.refresh();
        let r = client.call_increment_with_timeout(1, 1000);
        client.close();
        r
    });
    let elapsed = start.elapsed();

    unsafe { libc::close(listener) };
    let _ = worker.join();
    cleanup_all(&svc);
    assert!(
        result.is_some(),
        "refresh + call with a 1s timeout must return within 6s against a server that never \
         accepts (still blocked after {elapsed:?})"
    );
}

#[test]
fn test_robust_client_bounded_when_server_never_sends_hello_ack() {
    let svc = unique_service("rs_rob_nohelloack");
    ensure_run_dir();
    cleanup_all(&svc);
    let listener = raw_listener_fd_for_service(&svc);
    let acceptor = thread::spawn(move || {
        let fd = unsafe { libc::accept(listener, std::ptr::null_mut(), std::ptr::null_mut()) };
        // Hold the session open without replying until the listener goes away.
        thread::sleep(Duration::from_secs(8));
        if fd >= 0 {
            unsafe { libc::close(fd) };
        }
        unsafe { libc::close(listener) };
    });

    let svc2 = svc.clone();
    let (result, worker) = within(Duration::from_secs(6), move || {
        let mut client = increment_client(&svc2, client_config());
        client.set_call_timeout(1000);
        client.refresh();
        let r = client.call_increment_with_timeout(1, 1000);
        client.close();
        r
    });

    let _ = acceptor.join();
    let _ = worker.join();
    cleanup_all(&svc);
    assert!(
        result.is_some(),
        "refresh + call with a 1s timeout must return within 6s when the server accepts but \
         never sends HELLO_ACK"
    );
}

// ---------------------------------------------------------------------------
// N1: the configured call timeout must be honored on the UDS path.
// ---------------------------------------------------------------------------

#[test]
fn test_robust_uds_call_honors_timeout_longer_than_100ms() {
    let svc = unique_service("rs_rob_slow_handler");
    let handler = increment_dispatch(Arc::new(|v| {
        thread::sleep(Duration::from_millis(300));
        Some(v + 1)
    }));
    let (server, handle) = start_server(&svc, server_config(), METHOD_INCREMENT, handler, 2);

    let mut client = increment_client(&svc, client_config());
    connect_ready(&mut client);
    let start = Instant::now();
    let result = client.call_increment_with_timeout(41, 2000);
    let elapsed = start.elapsed();
    client.close();

    stop(&server);
    let _ = handle.join();
    cleanup_all(&svc);
    assert_eq!(
        result,
        Ok(42),
        "a 300ms handler must succeed with a 2000ms timeout (failed after {elapsed:?})"
    );
}

// ---------------------------------------------------------------------------
// N2: shutdown must not wait forever on a worker blocked sending a response
// to a client that stopped reading.
// ---------------------------------------------------------------------------

#[test]
fn test_robust_shutdown_not_blocked_by_non_reading_client() {
    const BIG: u32 = 8 * 1024 * 1024;
    let svc = unique_service("rs_rob_noread");
    let mut scfg = server_config();
    scfg.max_request_payload_bytes = BIG;
    scfg.max_response_payload_bytes = BIG;
    // Explicit packet size isolates this test from the default-size issue
    // covered by test_robust_default_packet_size_carries_large_message.
    scfg.packet_size = 65536;
    let (server, handle) = start_server(
        &svc,
        scfg,
        METHOD_STRING_REVERSE,
        string_reverse_dispatch_handler(),
        2,
    );

    let mut ccfg = client_config();
    ccfg.max_request_payload_bytes = BIG;
    ccfg.max_response_payload_bytes = BIG;
    ccfg.packet_size = 65536;
    let mut session = UdsSession::connect(TEST_RUN_DIR, &svc, &ccfg).expect("connect");
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
    // Never read the response; give the worker time to block in send.
    thread::sleep(Duration::from_millis(500));

    stop(&server);
    let (joined, waiter) = within(Duration::from_secs(3), move || handle.join().is_ok());

    drop(session);
    let _ = waiter.join();
    cleanup_all(&svc);
    assert!(
        joined.is_some(),
        "server run() must return within 3s of stop() while a worker is blocked sending"
    );
}

// ---------------------------------------------------------------------------
// N3: stop() issued before run() must not be lost.
// ---------------------------------------------------------------------------

#[test]
fn test_robust_stop_before_run_is_honored() {
    let svc = unique_service("rs_rob_stop_first");
    ensure_run_dir();
    cleanup_all(&svc);
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
        stop(&flag);
    }
    let _ = worker.join();
    cleanup_all(&svc);
    assert!(
        returned.is_some(),
        "run() after stop() must return promptly instead of serving forever"
    );
}

// ---------------------------------------------------------------------------
// N11: with the auto-detected packet size, a message larger than SO_SNDBUF
// must still be delivered (chunked).
// ---------------------------------------------------------------------------

#[test]
fn test_robust_default_packet_size_carries_large_message() {
    let result = large_round_trip(0);
    assert!(
        result.is_ok(),
        "a 512 KiB request must round-trip with default packet size: {result:?}"
    );
}

/// Control: the same message with an explicit 64 KiB packet size passes,
/// isolating the failure above to the auto-detected packet size.
#[test]
fn test_robust_explicit_packet_size_carries_large_message() {
    let result = large_round_trip(65536);
    assert!(
        result.is_ok(),
        "a 512 KiB request must round-trip with a 64 KiB packet size: {result:?}"
    );
}

fn large_round_trip(packet_size: u32) -> Result<(), NipcError> {
    const BIG: u32 = 1024 * 1024;
    let svc = unique_service("rs_rob_bigmsg");
    let mut scfg = server_config();
    scfg.max_request_payload_bytes = BIG;
    scfg.max_response_payload_bytes = BIG;
    scfg.packet_size = packet_size;
    let (server, handle) = start_server(
        &svc,
        scfg,
        METHOD_STRING_REVERSE,
        string_reverse_dispatch_handler(),
        2,
    );

    let mut ccfg = client_config();
    ccfg.max_request_payload_bytes = BIG;
    ccfg.max_response_payload_bytes = BIG;
    ccfg.packet_size = packet_size;
    let mut client = string_reverse_client(&svc, ccfg);
    connect_ready(&mut client);
    let text = "y".repeat(512 * 1024);
    let result = client
        .call_string_reverse_with_timeout(&text, 5000)
        .map(|v| v.as_str().len());
    client.close();

    stop(&server);
    let _ = handle.join();
    cleanup_all(&svc);
    match result {
        Ok(n) if n == text.len() => Ok(()),
        Ok(_) => Err(NipcError::Truncated),
        Err(e) => Err(e),
    }
}

// ---------------------------------------------------------------------------
// N5: an SHM client must notice a crashed server instead of waiting out its
// whole timeout.
// ---------------------------------------------------------------------------

#[test]
fn test_robust_shm_client_notices_crashed_server() {
    client_notices_crashed_server(true);
}

#[test]
fn test_robust_uds_client_reports_crashed_server_as_disconnected() {
    client_notices_crashed_server(false);
}

fn client_notices_crashed_server(shm: bool) {
    let svc = unique_service(if shm {
        "rs_rob_dead_server"
    } else {
        "rs_rob_dead_uds_server"
    });
    ensure_run_dir();
    cleanup_all(&svc);
    let mut child = spawn_helper(if shm { "shm_server" } else { "uds_server" }, &svc);

    let svc2 = svc.clone();
    let (call_tx, call_rx) = mpsc::channel();
    let caller = thread::spawn(move || {
        let config = if shm {
            shm_client_config()
        } else {
            client_config()
        };
        let mut client = increment_client(&svc2, config);
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
    cleanup_all(&svc);
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
// session and removes the prepared SHM region. The client must not spend its
// attach retry budget (5 s) on a region that will never appear, and must keep
// SHM enabled for its next connect.
#[test]
fn test_robust_shm_attach_stops_when_server_closes_session() {
    let svc = unique_service("rs_rob_attach_refused");
    let handler = increment_dispatch(Arc::new(|v| Some(v + 1)));
    let (running, handle) = start_server(&svc, shm_server_config(), METHOD_INCREMENT, handler, 1);

    let mut holder = increment_client(&svc, shm_client_config());
    connect_ready(&mut holder);

    let mut refused = increment_client(&svc, shm_client_config());
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
    stop(&running);
    let _ = handle.join();
    cleanup_all(&svc);

    assert!(
        elapsed < Duration::from_secs(1),
        "refused connect must return within 1s (attach retry budget is 5s): {elapsed:?}"
    );
    assert!(ready, "refused client must connect once the worker is free");
    assert!(uses_shm, "refused client must still use SHM");
}
