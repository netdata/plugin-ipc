use super::*;
use crate::protocol::{
    CgroupsBuilder, CgroupsRequest, Header, KIND_REQUEST, KIND_RESPONSE, METHOD_CGROUPS_SNAPSHOT,
    PROFILE_BASELINE, PROFILE_SHM_HYBRID, STATUS_OK,
};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

const TEST_RUN_DIR: &str = r"C:\Temp\nipc_svc_rust_test";
const AUTH_TOKEN: u64 = 0xDEADBEEFCAFEBABE;
const RESPONSE_BUF_SIZE: usize = 65536;

fn ensure_run_dir() {
    let _ = std::fs::create_dir_all(TEST_RUN_DIR);
}

fn cleanup_all(_service: &str) {}

fn server_config() -> ServerConfig {
    ServerConfig {
        supported_profiles: PROFILE_BASELINE,
        max_request_payload_bytes: 4096,
        max_request_batch_items: 1,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        max_response_batch_items: 1,
        auth_token: AUTH_TOKEN,
        ..ServerConfig::default()
    }
}

fn client_config() -> ClientConfig {
    ClientConfig {
        supported_profiles: PROFILE_BASELINE,
        max_request_payload_bytes: 4096,
        max_request_batch_items: 1,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        max_response_batch_items: 1,
        auth_token: AUTH_TOKEN,
        ..ClientConfig::default()
    }
}

fn shm_server_config() -> ServerConfig {
    ServerConfig {
        supported_profiles: PROFILE_SHM_HYBRID | PROFILE_BASELINE,
        max_request_payload_bytes: 4096,
        max_request_batch_items: 1,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        max_response_batch_items: 1,
        auth_token: AUTH_TOKEN,
        ..ServerConfig::default()
    }
}

fn shm_client_config() -> ClientConfig {
    ClientConfig {
        supported_profiles: PROFILE_SHM_HYBRID | PROFILE_BASELINE,
        max_request_payload_bytes: 4096,
        max_request_batch_items: 1,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        max_response_batch_items: 1,
        auth_token: AUTH_TOKEN,
        ..ClientConfig::default()
    }
}

fn batch_server_config() -> ServerConfig {
    ServerConfig {
        supported_profiles: PROFILE_BASELINE,
        max_request_payload_bytes: 4096,
        max_request_batch_items: 16,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        max_response_batch_items: 16,
        auth_token: AUTH_TOKEN,
        ..ServerConfig::default()
    }
}

fn batch_client_config() -> ClientConfig {
    ClientConfig {
        supported_profiles: PROFILE_BASELINE,
        max_request_payload_bytes: 4096,
        max_request_batch_items: 16,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        max_response_batch_items: 16,
        auth_token: AUTH_TOKEN,
        ..ClientConfig::default()
    }
}

fn fill_test_cgroups_snapshot(builder: &mut CgroupsBuilder<'_>) -> bool {
    let items = [
        (
            1001u32,
            0u32,
            1u32,
            b"docker-abc123" as &[u8],
            b"/sys/fs/cgroup/docker/abc123" as &[u8],
        ),
        (2002, 0, 1, b"k8s-pod-xyz", b"/sys/fs/cgroup/kubepods/xyz"),
        (
            3003,
            0,
            0,
            b"systemd-user",
            b"/sys/fs/cgroup/user.slice/user-1000",
        ),
    ];

    for (hash, options, enabled, name, path) in &items {
        if builder.add(*hash, *options, *enabled, name, path).is_err() {
            return false;
        }
    }

    true
}

fn test_cgroups_handlers() -> Handlers {
    Handlers {
        on_snapshot: Some(Arc::new(|req, builder| {
            if req.layout_version != 1 || req.flags != 0 {
                return false;
            }
            builder.set_header(1, 42);
            fill_test_cgroups_snapshot(builder)
        })),
        snapshot_max_items: 3,
        ..Handlers::default()
    }
}

fn pingpong_handlers() -> Handlers {
    Handlers {
        on_increment: Some(Arc::new(|value| Some(value + 1))),
        on_string_reverse: Some(Arc::new(|s| Some(s.chars().rev().collect()))),
        ..Handlers::default()
    }
}

fn connect_ready(client: &mut CgroupsClient) {
    for _ in 0..200 {
        client.refresh();
        if client.ready() {
            return;
        }
        thread::sleep(Duration::from_millis(10));
    }

    panic!("client did not reach READY state");
}

struct TestServer {
    service: String,
    wake_config: ClientConfig,
    stop_flag: Arc<AtomicBool>,
    thread: Option<thread::JoinHandle<()>>,
}

impl TestServer {
    fn start(service: &str, handlers: Handlers) -> Self {
        Self::start_with(service, server_config(), client_config(), handlers, 8)
    }

    fn start_shm(service: &str, handlers: Handlers) -> Self {
        Self::start_with(
            service,
            shm_server_config(),
            shm_client_config(),
            handlers,
            8,
        )
    }

    fn start_batch(service: &str, handlers: Handlers) -> Self {
        Self::start_with(
            service,
            batch_server_config(),
            batch_client_config(),
            handlers,
            8,
        )
    }

    fn start_with(
        service: &str,
        config: ServerConfig,
        wake_config: ClientConfig,
        handlers: Handlers,
        worker_count: usize,
    ) -> Self {
        ensure_run_dir();
        cleanup_all(service);

        let svc = service.to_string();
        let stop_cfg = wake_config.clone();
        let mut server =
            ManagedServer::with_workers(TEST_RUN_DIR, &svc, config, handlers, worker_count);
        let stop_flag = server.running_flag();

        let thread = thread::spawn(move || {
            let _ = server.run();
        });
        thread::sleep(Duration::from_millis(50));

        TestServer {
            service: svc,
            wake_config: stop_cfg,
            stop_flag,
            thread: Some(thread),
        }
    }

    fn stop(&mut self) {
        self.stop_flag.store(false, Ordering::Release);

        // Wake a blocking ConnectNamedPipe() so the accept loop can observe
        // the stop flag and exit.
        let _ = NpSession::connect(TEST_RUN_DIR, &self.service, &self.wake_config);

        if let Some(handle) = self.thread.take() {
            let _ = handle.join();
        }

        cleanup_all(&self.service);
    }
}

impl Drop for TestServer {
    fn drop(&mut self) {
        self.stop();
    }
}

#[test]
fn test_client_lifecycle_windows() {
    let svc = "rs_win_svc_lifecycle";
    ensure_run_dir();
    cleanup_all(svc);

    let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
    assert_eq!(client.state, ClientState::Disconnected);
    assert!(!client.ready());

    let changed = client.refresh();
    assert!(changed);
    assert_eq!(client.state, ClientState::NotFound);

    let mut server = TestServer::start(svc, test_cgroups_handlers());

    connect_ready(&mut client);
    assert_eq!(client.state, ClientState::Ready);
    assert!(client.ready());
    assert_eq!(client.status().connect_count, 1);

    client.close();
    assert_eq!(client.state, ClientState::Disconnected);

    server.stop();
}

#[test]
fn test_cgroups_call_windows_baseline() {
    let svc = "rs_win_svc_cgroups";
    let mut server = TestServer::start(svc, test_cgroups_handlers());

    let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
    connect_ready(&mut client);

    let view = client.call_snapshot().expect("snapshot");
    assert_eq!(view.item_count, 3);
    assert_eq!(view.systemd_enabled, 1);
    assert_eq!(view.generation, 42);

    let item0 = view.item(0).expect("item 0");
    assert_eq!(item0.hash, 1001);
    assert_eq!(item0.name.as_bytes(), b"docker-abc123");

    client.close();
    server.stop();
}

#[test]
fn test_cgroups_call_windows_shm() {
    let svc = "rs_win_svc_cgroups_shm";
    let mut server = TestServer::start_shm(svc, test_cgroups_handlers());

    let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, shm_client_config());
    connect_ready(&mut client);

    assert!(client.shm.is_some(), "expected Win SHM to be negotiated");
    assert_eq!(
        client.session.as_ref().map(|s| s.selected_profile),
        Some(PROFILE_SHM_HYBRID)
    );

    let view = client.call_snapshot().expect("snapshot");
    assert_eq!(view.item_count, 3);
    assert_eq!(view.generation, 42);

    client.close();
    server.stop();
}

#[test]
#[ignore = "Windows managed-server shutdown/reconnect still needs a dedicated investigation"]
fn test_retry_on_failure_windows() {
    let svc = "rs_win_svc_retry";
    let mut server1 = TestServer::start(svc, test_cgroups_handlers());

    let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
    connect_ready(&mut client);

    let view = client.call_snapshot().expect("first call");
    assert_eq!(view.item_count, 3);

    server1.stop();
    thread::sleep(Duration::from_millis(50));

    let mut server2 = TestServer::start(svc, test_cgroups_handlers());

    let view2 = client.call_snapshot().expect("retry call");
    assert_eq!(view2.item_count, 3);
    assert!(client.status().reconnect_count >= 1);

    client.close();
    server2.stop();
}

#[test]
fn test_non_request_terminates_session_windows() {
    let svc = "rs_win_svc_nonreq";
    let mut server = TestServer::start(svc, test_cgroups_handlers());

    let mut session = NpSession::connect(TEST_RUN_DIR, svc, &client_config()).expect("connect");

    let mut hdr = Header {
        kind: KIND_RESPONSE,
        code: METHOD_CGROUPS_SNAPSHOT,
        flags: 0,
        item_count: 0,
        message_id: 1,
        transport_status: STATUS_OK,
        ..Header::default()
    };
    let send_result = session.send(&mut hdr, &[]);
    if send_result.is_ok() {
        thread::sleep(Duration::from_millis(100));
        let mut recv_buf = vec![0u8; 4096];
        let mut hdr2 = Header {
            kind: KIND_REQUEST,
            code: METHOD_CGROUPS_SNAPSHOT,
            flags: 0,
            item_count: 1,
            message_id: 2,
            transport_status: STATUS_OK,
            ..Header::default()
        };
        let req = CgroupsRequest {
            layout_version: 1,
            flags: 0,
        };
        let mut req_buf = [0u8; 4];
        req.encode(&mut req_buf);
        let _ = session.send(&mut hdr2, &req_buf);
        let recv = session.receive(&mut recv_buf);
        assert!(
            recv.is_err(),
            "server should terminate the offending session"
        );
    }

    drop(session);

    let mut verify = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
    connect_ready(&mut verify);
    let view = verify.call_snapshot().expect("normal call");
    assert_eq!(view.item_count, 3);

    verify.close();
    server.stop();
}

#[test]
fn test_cache_full_round_trip_windows() {
    let svc = "rs_win_cache_roundtrip";
    let mut server = TestServer::start(svc, test_cgroups_handlers());

    let mut cache = CgroupsCache::new(TEST_RUN_DIR, svc, client_config());
    assert!(!cache.ready());

    thread::sleep(Duration::from_millis(2));

    let updated = cache.refresh();
    assert!(updated);
    assert!(cache.ready());

    let item = cache.lookup(1001, "docker-abc123").expect("lookup");
    assert_eq!(item.hash, 1001);
    assert_eq!(item.path, "/sys/fs/cgroup/docker/abc123");

    let status = cache.status();
    assert!(status.populated);
    assert_eq!(status.item_count, 3);
    assert_eq!(status.generation, 42);
    assert_eq!(status.connection_state, ClientState::Ready);

    cache.close();
    server.stop();
}

#[test]
fn test_increment_ping_pong_windows() {
    let svc = "rs_win_pp_increment";
    let mut server = TestServer::start(svc, pingpong_handlers());

    let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());
    connect_ready(&mut client);

    let mut value = 0u64;
    for _ in 0..10 {
        value = client.call_increment(value).expect("increment");
    }
    assert_eq!(value, 10);

    client.close();
    server.stop();
}

#[test]
fn test_increment_batch_windows() {
    let svc = "rs_win_pp_batch";
    let mut server = TestServer::start_batch(svc, pingpong_handlers());

    let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, batch_client_config());
    connect_ready(&mut client);

    let values = vec![10u64, 20, 30, 40];
    let results = client.call_increment_batch(&values).expect("batch call");
    assert_eq!(results, vec![11, 21, 31, 41]);

    let single = client
        .call_increment_batch(&[99])
        .expect("single-item batch");
    assert_eq!(single, vec![100]);

    client.close();
    server.stop();
}

#[test]
fn test_client_auth_failure_windows() {
    let svc = "rs_win_svc_authfail";
    let mut server = TestServer::start(svc, test_cgroups_handlers());

    let mut bad_cfg = client_config();
    bad_cfg.auth_token = 0xBAD_BAD_BAD;

    let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, bad_cfg);
    client.refresh();
    assert_eq!(client.state, ClientState::AuthFailed);
    assert!(!client.ready());

    client.refresh();
    assert_eq!(client.state, ClientState::AuthFailed);

    client.close();
    server.stop();
}

#[test]
fn test_client_incompatible_windows() {
    let svc = "rs_win_svc_incompat";
    let mut server = TestServer::start(svc, test_cgroups_handlers());

    let mut bad_cfg = client_config();
    bad_cfg.supported_profiles = 0x80000000;

    let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, bad_cfg);
    client.refresh();
    assert_eq!(client.state, ClientState::Incompatible);
    assert!(!client.ready());

    client.refresh();
    assert_eq!(client.state, ClientState::Incompatible);

    client.close();
    server.stop();
}

#[test]
fn test_call_when_not_ready_windows() {
    let svc = "rs_win_svc_noready";
    let mut client = CgroupsClient::new(TEST_RUN_DIR, svc, client_config());

    assert_eq!(client.state, ClientState::Disconnected);
    assert!(client.call_snapshot().is_err());
    assert!(client.call_increment(42).is_err());
    assert!(client.call_string_reverse("test").is_err());
    assert_eq!(client.status().error_count, 3);

    client.close();
}

#[test]
fn test_server_worker_count_clamped_windows() {
    let svc = "rs_win_svc_w0";
    ensure_run_dir();
    cleanup_all(svc);

    let server =
        ManagedServer::with_workers(TEST_RUN_DIR, svc, server_config(), Handlers::default(), 0);
    assert_eq!(server.worker_count, 1);
}

#[test]
fn test_server_stop_flag_windows() {
    let svc = "rs_win_svc_stopflag";
    ensure_run_dir();
    cleanup_all(svc);

    let server = ManagedServer::new(TEST_RUN_DIR, svc, server_config(), Handlers::default());
    let flag = server.running_flag();
    assert!(!flag.load(Ordering::Acquire));

    server.stop();
    assert!(!flag.load(Ordering::Acquire));
}
