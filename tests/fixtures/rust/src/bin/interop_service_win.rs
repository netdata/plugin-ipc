//! L2 cross-language interop binary (Windows) for the cgroups-snapshot
//! service kind.
//!
//! Usage:
//!   interop_service_win server <run_dir> <service_name>
//!   interop_service_win client <run_dir> <service_name>

#[cfg(windows)]
use netipc::protocol::{
    APPS_CGROUP_HOST_ROOT, APPS_CGROUP_KNOWN, APPS_CGROUP_UNKNOWN_RETRY_LATER, CGROUP_LOOKUP_KNOWN,
    CGROUP_LOOKUP_OVERSIZED_ITEM, CGROUP_LOOKUP_UNKNOWN_PERMANENT,
    CGROUP_LOOKUP_UNKNOWN_RETRY_LATER, NIPC_UID_UNSET, ORCHESTRATOR_DOCKER, ORCHESTRATOR_K8S,
    PID_LOOKUP_KNOWN, PID_LOOKUP_OVERSIZED_ITEM, PID_LOOKUP_UNKNOWN, PROFILE_BASELINE,
    PROFILE_SHM_HYBRID,
};
#[cfg(windows)]
use netipc::service::cgroups::{CgroupsClient, ClientConfig, Handler, ManagedServer, ServerConfig};
#[cfg(windows)]
use netipc::service::{apps_lookup, cgroups_lookup};
#[cfg(windows)]
use std::thread;
#[cfg(windows)]
use std::time::Duration;

#[cfg(windows)]
const AUTH_TOKEN: u64 = 0xDEADBEEFCAFEBABE;
#[cfg(windows)]
const RESPONSE_BUF_SIZE: usize = 65536;
#[cfg(windows)]
const LOOKUP_SCALE_ITEMS_DEFAULT: usize = 8192;
#[cfg(windows)]
const LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES: u32 = 8192;
#[cfg(windows)]
const LOOKUP_SCALE_CALL_TIMEOUT_MS: u32 = 120_000;
#[cfg(windows)]
const LOOKUP_MIXED_ITEMS: usize = 5;

/// NIPC_PROFILE env var: "shm" enables SHM_HYBRID|BASELINE, default BASELINE only.
#[cfg(windows)]
fn detect_profiles() -> u32 {
    match std::env::var("NIPC_PROFILE").as_deref() {
        Ok("shm") => PROFILE_SHM_HYBRID | PROFILE_BASELINE,
        _ => PROFILE_BASELINE,
    }
}

#[cfg(windows)]
fn lookup_item_count() -> usize {
    std::env::var("NIPC_LOOKUP_SCALE_ITEMS")
        .ok()
        .and_then(|value| value.parse::<usize>().ok())
        .filter(|value| *value > 0 && *value <= 65_536)
        .unwrap_or(LOOKUP_SCALE_ITEMS_DEFAULT)
}

#[cfg(windows)]
fn lookup_count_u32(value: usize) -> u32 {
    value as u32
}

#[cfg(windows)]
fn apps_lookup_server_config() -> apps_lookup::ServerConfig {
    let profiles = detect_profiles();
    apps_lookup::ServerConfig {
        supported_profiles: profiles,
        preferred_profiles: profiles,
        max_request_payload_bytes: LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES,
        max_request_batch_items: 16,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        auth_token: AUTH_TOKEN,
    }
}

#[cfg(windows)]
fn apps_lookup_client_config(item_count: usize) -> apps_lookup::ClientConfig {
    let profiles = detect_profiles();
    apps_lookup::ClientConfig {
        supported_profiles: profiles,
        preferred_profiles: profiles,
        max_request_payload_bytes: LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES,
        max_request_batch_items: 16,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        auth_token: AUTH_TOKEN,
        max_logical_lookup_items: lookup_count_u32(item_count),
        max_logical_lookup_subcalls: 4096,
        max_logical_lookup_response_bytes: 64 * 1024 * 1024,
    }
}

#[cfg(windows)]
fn cgroups_lookup_server_config() -> cgroups_lookup::ServerConfig {
    let profiles = detect_profiles();
    cgroups_lookup::ServerConfig {
        supported_profiles: profiles,
        preferred_profiles: profiles,
        max_request_payload_bytes: LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES,
        max_request_batch_items: 16,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        auth_token: AUTH_TOKEN,
    }
}

#[cfg(windows)]
fn cgroups_lookup_client_config(item_count: usize) -> cgroups_lookup::ClientConfig {
    let profiles = detect_profiles();
    cgroups_lookup::ClientConfig {
        supported_profiles: profiles,
        preferred_profiles: profiles,
        max_request_payload_bytes: LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES,
        max_request_batch_items: 16,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        auth_token: AUTH_TOKEN,
        max_logical_lookup_items: lookup_count_u32(item_count),
        max_logical_lookup_subcalls: 4096,
        max_logical_lookup_response_bytes: 64 * 1024 * 1024,
    }
}

#[cfg(windows)]
fn apps_lookup_handler() -> apps_lookup::Handler {
    apps_lookup::Handler {
        handle: Some(std::sync::Arc::new(|request, builder| {
            builder.set_generation(9);
            for i in 0..request.item_count {
                let Ok(pid) = request.item(i) else {
                    return false;
                };
                if builder
                    .add(
                        PID_LOOKUP_KNOWN,
                        APPS_CGROUP_KNOWN,
                        ORCHESTRATOR_DOCKER,
                        pid,
                        1,
                        1000,
                        42,
                        b"ok",
                        b"/ok",
                        b"name",
                        &[],
                    )
                    .is_err()
                {
                    return false;
                }
            }
            true
        })),
    }
}

#[cfg(windows)]
fn cgroups_lookup_handler() -> cgroups_lookup::Handler {
    cgroups_lookup::Handler {
        handle: Some(std::sync::Arc::new(|request, builder| {
            builder.set_generation(7);
            for i in 0..request.item_count {
                let Ok(path) = request.item(i) else {
                    return false;
                };
                if builder
                    .add(
                        CGROUP_LOOKUP_KNOWN,
                        ORCHESTRATOR_K8S,
                        path.as_bytes(),
                        b"ok",
                        &[],
                    )
                    .is_err()
                {
                    return false;
                }
            }
            true
        })),
    }
}

#[cfg(windows)]
fn apps_lookup_mixed_handler() -> apps_lookup::Handler {
    apps_lookup::Handler {
        handle: Some(std::sync::Arc::new(|request, builder| {
            builder.set_generation(19);
            for i in 0..request.item_count {
                let Ok(pid) = request.item(i) else {
                    return false;
                };
                let result = match pid {
                    1001 => builder.add(
                        PID_LOOKUP_KNOWN,
                        APPS_CGROUP_KNOWN,
                        ORCHESTRATOR_DOCKER,
                        pid,
                        1,
                        1000,
                        42,
                        b"known",
                        b"/cg/known",
                        b"pod-a",
                        &[(b"role".as_slice(), b"api".as_slice())],
                    ),
                    1002 => builder.add(
                        PID_LOOKUP_KNOWN,
                        APPS_CGROUP_HOST_ROOT,
                        0,
                        pid,
                        1,
                        1001,
                        43,
                        b"host",
                        b"",
                        b"",
                        &[],
                    ),
                    1003 => builder.add(
                        PID_LOOKUP_UNKNOWN,
                        0,
                        0,
                        pid,
                        0,
                        NIPC_UID_UNSET,
                        0,
                        b"",
                        b"",
                        b"",
                        &[],
                    ),
                    1004 => builder.add(
                        PID_LOOKUP_OVERSIZED_ITEM,
                        0,
                        0,
                        pid,
                        0,
                        NIPC_UID_UNSET,
                        0,
                        b"",
                        b"",
                        b"",
                        &[],
                    ),
                    _ => builder.add(
                        PID_LOOKUP_KNOWN,
                        APPS_CGROUP_UNKNOWN_RETRY_LATER,
                        0,
                        pid,
                        1,
                        1002,
                        44,
                        b"retry",
                        b"",
                        b"",
                        &[],
                    ),
                };
                if result.is_err() {
                    return false;
                }
            }
            true
        })),
    }
}

#[cfg(windows)]
fn cgroups_lookup_mixed_handler() -> cgroups_lookup::Handler {
    cgroups_lookup::Handler {
        handle: Some(std::sync::Arc::new(|request, builder| {
            builder.set_generation(17);
            for i in 0..request.item_count {
                let Ok(path) = request.item(i) else {
                    return false;
                };
                let result = match path.as_bytes() {
                    b"/known" => builder.add(
                        CGROUP_LOOKUP_KNOWN,
                        ORCHESTRATOR_K8S,
                        path.as_bytes(),
                        b"pod-a",
                        &[(b"role".as_slice(), b"db".as_slice())],
                    ),
                    b"/retry" => builder.add(
                        CGROUP_LOOKUP_UNKNOWN_RETRY_LATER,
                        0,
                        path.as_bytes(),
                        b"",
                        &[],
                    ),
                    b"/permanent" => builder.add(
                        CGROUP_LOOKUP_UNKNOWN_PERMANENT,
                        0,
                        path.as_bytes(),
                        b"",
                        &[],
                    ),
                    b"/oversized" => {
                        builder.add(CGROUP_LOOKUP_OVERSIZED_ITEM, 0, path.as_bytes(), b"", &[])
                    }
                    _ => builder.add(
                        CGROUP_LOOKUP_KNOWN,
                        ORCHESTRATOR_DOCKER,
                        path.as_bytes(),
                        b"pod-b",
                        &[],
                    ),
                };
                if result.is_err() {
                    return false;
                }
            }
            true
        })),
    }
}

#[cfg(windows)]
fn server_handler() -> Handler {
    Handler {
        handle: Some(std::sync::Arc::new(|request, builder| {
            if request.layout_version != 1 || request.flags != 0 {
                return false;
            }

            builder.set_header(1, 42);
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
        })),
        snapshot_max_items: 3,
        ..Handler::default()
    }
}

#[cfg(windows)]
fn run_server(run_dir: &str, service: &str) -> i32 {
    let profiles = detect_profiles();
    let config = ServerConfig {
        supported_profiles: profiles,
        max_request_batch_items: 16,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        auth_token: AUTH_TOKEN,
        ..ServerConfig::default()
    };

    let mut server = ManagedServer::new(run_dir, service, config, server_handler());
    let stop_flag = server.running_flag();
    let wake_run_dir = run_dir.to_string();
    let wake_service = service.to_string();
    let wake_config = ClientConfig {
        supported_profiles: profiles,
        max_request_batch_items: 16,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        auth_token: AUTH_TOKEN,
        ..ClientConfig::default()
    };

    println!("READY");

    // Auto-stop after 10 seconds
    let sf = stop_flag.clone();
    std::thread::spawn(move || {
        std::thread::sleep(std::time::Duration::from_secs(10));
        sf.store(false, std::sync::atomic::Ordering::Release);
        let mut wake = CgroupsClient::new(&wake_run_dir, &wake_service, wake_config);
        let _ = wake.refresh();
    });

    let _ = server.run();
    0
}

#[cfg(windows)]
fn run_apps_server(run_dir: &str, service: &str) -> i32 {
    let mut server = apps_lookup::ManagedServer::with_workers(
        run_dir,
        service,
        apps_lookup_server_config(),
        apps_lookup_handler(),
        8,
    );
    let stop_flag = server.running_flag();
    let wake_run_dir = run_dir.to_string();
    let wake_service = service.to_string();
    let wake_config = apps_lookup_client_config(1);

    println!("READY");

    let sf = stop_flag.clone();
    std::thread::spawn(move || {
        std::thread::sleep(std::time::Duration::from_secs(120));
        sf.store(false, std::sync::atomic::Ordering::Release);
        let mut wake =
            apps_lookup::AppsLookupClient::new(&wake_run_dir, &wake_service, wake_config);
        let _ = wake.refresh();
    });

    let _ = server.run();
    0
}

#[cfg(windows)]
fn run_cgroups_lookup_server(run_dir: &str, service: &str) -> i32 {
    let mut server = cgroups_lookup::ManagedServer::with_workers(
        run_dir,
        service,
        cgroups_lookup_server_config(),
        cgroups_lookup_handler(),
        8,
    );
    let stop_flag = server.running_flag();
    let wake_run_dir = run_dir.to_string();
    let wake_service = service.to_string();
    let wake_config = cgroups_lookup_client_config(1);

    println!("READY");

    let sf = stop_flag.clone();
    std::thread::spawn(move || {
        std::thread::sleep(std::time::Duration::from_secs(120));
        sf.store(false, std::sync::atomic::Ordering::Release);
        let mut wake =
            cgroups_lookup::CgroupsLookupClient::new(&wake_run_dir, &wake_service, wake_config);
        let _ = wake.refresh();
    });

    let _ = server.run();
    0
}

#[cfg(windows)]
fn run_apps_mixed_server(run_dir: &str, service: &str) -> i32 {
    let mut server = apps_lookup::ManagedServer::with_workers(
        run_dir,
        service,
        apps_lookup_server_config(),
        apps_lookup_mixed_handler(),
        2,
    );
    let stop_flag = server.running_flag();
    let wake_run_dir = run_dir.to_string();
    let wake_service = service.to_string();
    let wake_config = apps_lookup_client_config(1);

    println!("READY");

    let sf = stop_flag.clone();
    std::thread::spawn(move || {
        std::thread::sleep(std::time::Duration::from_secs(120));
        sf.store(false, std::sync::atomic::Ordering::Release);
        let mut wake =
            apps_lookup::AppsLookupClient::new(&wake_run_dir, &wake_service, wake_config);
        let _ = wake.refresh();
    });

    let _ = server.run();
    0
}

#[cfg(windows)]
fn run_cgroups_mixed_server(run_dir: &str, service: &str) -> i32 {
    let mut server = cgroups_lookup::ManagedServer::with_workers(
        run_dir,
        service,
        cgroups_lookup_server_config(),
        cgroups_lookup_mixed_handler(),
        2,
    );
    let stop_flag = server.running_flag();
    let wake_run_dir = run_dir.to_string();
    let wake_service = service.to_string();
    let wake_config = cgroups_lookup_client_config(1);

    println!("READY");

    let sf = stop_flag.clone();
    std::thread::spawn(move || {
        std::thread::sleep(std::time::Duration::from_secs(120));
        sf.store(false, std::sync::atomic::Ordering::Release);
        let mut wake =
            cgroups_lookup::CgroupsLookupClient::new(&wake_run_dir, &wake_service, wake_config);
        let _ = wake.refresh();
    });

    let _ = server.run();
    0
}

#[cfg(windows)]
fn large_lookup_pids(item_count: usize) -> Vec<u32> {
    (0..item_count).map(|i| 100000u32 + i as u32).collect()
}

#[cfg(windows)]
fn large_lookup_paths(item_count: usize) -> Vec<Vec<u8>> {
    (0..item_count)
        .map(|i| format!("/cg/{i:05}").into_bytes())
        .collect()
}

#[cfg(windows)]
fn wait_apps_client_ready(client: &mut apps_lookup::AppsLookupClient) -> bool {
    for _ in 0..200 {
        client.refresh();
        if client.ready() {
            return true;
        }
        thread::sleep(Duration::from_millis(10));
    }
    false
}

#[cfg(windows)]
fn wait_cgroups_lookup_client_ready(client: &mut cgroups_lookup::CgroupsLookupClient) -> bool {
    for _ in 0..200 {
        client.refresh();
        if client.ready() {
            return true;
        }
        thread::sleep(Duration::from_millis(10));
    }
    false
}

#[cfg(windows)]
fn run_apps_client(run_dir: &str, service: &str) -> i32 {
    let item_count = lookup_item_count();
    let pids = large_lookup_pids(item_count);
    let mut client =
        apps_lookup::AppsLookupClient::new(run_dir, service, apps_lookup_client_config(item_count));
    client.set_call_timeout(LOOKUP_SCALE_CALL_TIMEOUT_MS);

    if !wait_apps_client_ready(&mut client) {
        eprintln!("apps client: not ready");
        return 1;
    }

    let result = (|| -> Result<(), String> {
        let view = client
            .call(&pids)
            .map_err(|err| format!("apps client: call failed: {err:?}"))?;
        if view.item_count != lookup_count_u32(pids.len()) || view.generation != 9 {
            return Err(format!(
                "apps client: bad header count={} generation={}",
                view.item_count, view.generation
            ));
        }
        for (i, expected) in pids.iter().enumerate() {
            let item = view
                .item(lookup_count_u32(i))
                .map_err(|err| format!("apps client: item {i} decode failed: {err:?}"))?;
            if item.status != PID_LOOKUP_KNOWN
                || item.pid != *expected
                || item.comm.as_bytes() != b"ok"
                || item.cgroup_path.as_bytes() != b"/ok"
            {
                return Err(format!("apps client: bad item {i}"));
            }
        }
        Ok(())
    })();

    client.close();
    match result {
        Ok(()) => {
            println!("PASS");
            0
        }
        Err(err) => {
            eprintln!("{err}");
            println!("FAIL");
            1
        }
    }
}

#[cfg(windows)]
fn run_cgroups_lookup_client(run_dir: &str, service: &str) -> i32 {
    let item_count = lookup_item_count();
    let paths = large_lookup_paths(item_count);
    let path_refs: Vec<&[u8]> = paths.iter().map(Vec::as_slice).collect();
    let mut client = cgroups_lookup::CgroupsLookupClient::new(
        run_dir,
        service,
        cgroups_lookup_client_config(item_count),
    );
    client.set_call_timeout(LOOKUP_SCALE_CALL_TIMEOUT_MS);

    if !wait_cgroups_lookup_client_ready(&mut client) {
        eprintln!("cgroups lookup client: not ready");
        return 1;
    }

    let result = (|| -> Result<(), String> {
        let view = client
            .call(&path_refs)
            .map_err(|err| format!("cgroups lookup client: call failed: {err:?}"))?;
        if view.item_count != lookup_count_u32(paths.len()) || view.generation != 7 {
            return Err(format!(
                "cgroups lookup client: bad header count={} generation={}",
                view.item_count, view.generation
            ));
        }
        for (i, expected) in paths.iter().enumerate() {
            let item = view
                .item(lookup_count_u32(i))
                .map_err(|err| format!("cgroups lookup client: item {i} decode failed: {err:?}"))?;
            if item.status != CGROUP_LOOKUP_KNOWN
                || item.path.as_bytes() != expected.as_slice()
                || item.name.as_bytes() != b"ok"
            {
                return Err(format!("cgroups lookup client: bad item {i}"));
            }
        }
        Ok(())
    })();

    client.close();
    match result {
        Ok(()) => {
            println!("PASS");
            0
        }
        Err(err) => {
            eprintln!("{err}");
            println!("FAIL");
            1
        }
    }
}

#[cfg(windows)]
fn run_apps_mixed_client(run_dir: &str, service: &str) -> i32 {
    let pids = [1001, 1002, 1003, 1004, 1005];
    let mut client = apps_lookup::AppsLookupClient::new(
        run_dir,
        service,
        apps_lookup_client_config(LOOKUP_MIXED_ITEMS),
    );
    client.set_call_timeout(LOOKUP_SCALE_CALL_TIMEOUT_MS);

    if !wait_apps_client_ready(&mut client) {
        eprintln!("apps mixed client: not ready");
        return 1;
    }

    let result = (|| -> Result<(), String> {
        let view = client
            .call(&pids)
            .map_err(|err| format!("apps mixed client: call failed: {err:?}"))?;
        if view.item_count != LOOKUP_MIXED_ITEMS as u32 || view.generation != 19 {
            return Err(format!(
                "apps mixed client: bad header count={} generation={}",
                view.item_count, view.generation
            ));
        }

        let item0 = view
            .item(0)
            .map_err(|err| format!("apps mixed client: item 0 decode failed: {err:?}"))?;
        if item0.status != PID_LOOKUP_KNOWN
            || item0.cgroup_status != APPS_CGROUP_KNOWN
            || item0.pid != 1001
            || item0.comm.as_bytes() != b"known"
            || item0.cgroup_path.as_bytes() != b"/cg/known"
            || item0.label_count != 1
        {
            return Err("apps mixed client: bad item 0".to_string());
        }
        let label = item0
            .label(0)
            .map_err(|err| format!("apps mixed client: item 0 label decode failed: {err:?}"))?;
        if label.key.as_bytes() != b"role" || label.value.as_bytes() != b"api" {
            return Err("apps mixed client: bad item 0 label".to_string());
        }

        let checks = [
            (
                1,
                1002,
                PID_LOOKUP_KNOWN,
                APPS_CGROUP_HOST_ROOT,
                b"host".as_slice(),
            ),
            (2, 1003, PID_LOOKUP_UNKNOWN, 0, b"".as_slice()),
            (3, 1004, PID_LOOKUP_OVERSIZED_ITEM, 0, b"".as_slice()),
            (
                4,
                1005,
                PID_LOOKUP_KNOWN,
                APPS_CGROUP_UNKNOWN_RETRY_LATER,
                b"retry".as_slice(),
            ),
        ];
        for (index, pid, status, cgroup_status, comm) in checks {
            let item = view
                .item(index)
                .map_err(|err| format!("apps mixed client: item {index} decode failed: {err:?}"))?;
            if item.pid != pid
                || item.status != status
                || item.cgroup_status != cgroup_status
                || item.comm.as_bytes() != comm
            {
                return Err(format!("apps mixed client: bad item {index}"));
            }
        }
        Ok(())
    })();

    client.close();
    match result {
        Ok(()) => {
            println!("PASS");
            0
        }
        Err(err) => {
            eprintln!("{err}");
            println!("FAIL");
            1
        }
    }
}

#[cfg(windows)]
fn run_cgroups_mixed_client(run_dir: &str, service: &str) -> i32 {
    let paths: [&[u8]; LOOKUP_MIXED_ITEMS] = [
        b"/known",
        b"/retry",
        b"/permanent",
        b"/oversized",
        b"/known2",
    ];
    let mut client = cgroups_lookup::CgroupsLookupClient::new(
        run_dir,
        service,
        cgroups_lookup_client_config(LOOKUP_MIXED_ITEMS),
    );
    client.set_call_timeout(LOOKUP_SCALE_CALL_TIMEOUT_MS);

    if !wait_cgroups_lookup_client_ready(&mut client) {
        eprintln!("cgroups mixed client: not ready");
        return 1;
    }

    let result = (|| -> Result<(), String> {
        let view = client
            .call(&paths)
            .map_err(|err| format!("cgroups mixed client: call failed: {err:?}"))?;
        if view.item_count != LOOKUP_MIXED_ITEMS as u32 || view.generation != 17 {
            return Err(format!(
                "cgroups mixed client: bad header count={} generation={}",
                view.item_count, view.generation
            ));
        }

        let item0 = view
            .item(0)
            .map_err(|err| format!("cgroups mixed client: item 0 decode failed: {err:?}"))?;
        if item0.status != CGROUP_LOOKUP_KNOWN
            || item0.path.as_bytes() != b"/known"
            || item0.name.as_bytes() != b"pod-a"
            || item0.label_count != 1
        {
            return Err("cgroups mixed client: bad item 0".to_string());
        }
        let label = item0
            .label(0)
            .map_err(|err| format!("cgroups mixed client: item 0 label decode failed: {err:?}"))?;
        if label.key.as_bytes() != b"role" || label.value.as_bytes() != b"db" {
            return Err("cgroups mixed client: bad item 0 label".to_string());
        }

        let checks = [
            (
                1,
                b"/retry".as_slice(),
                CGROUP_LOOKUP_UNKNOWN_RETRY_LATER,
                b"".as_slice(),
            ),
            (
                2,
                b"/permanent".as_slice(),
                CGROUP_LOOKUP_UNKNOWN_PERMANENT,
                b"".as_slice(),
            ),
            (
                3,
                b"/oversized".as_slice(),
                CGROUP_LOOKUP_OVERSIZED_ITEM,
                b"".as_slice(),
            ),
            (
                4,
                b"/known2".as_slice(),
                CGROUP_LOOKUP_KNOWN,
                b"pod-b".as_slice(),
            ),
        ];
        for (index, path, status, name) in checks {
            let item = view.item(index).map_err(|err| {
                format!("cgroups mixed client: item {index} decode failed: {err:?}")
            })?;
            if item.status != status || item.path.as_bytes() != path || item.name.as_bytes() != name
            {
                return Err(format!("cgroups mixed client: bad item {index}"));
            }
        }
        Ok(())
    })();

    client.close();
    match result {
        Ok(()) => {
            println!("PASS");
            0
        }
        Err(err) => {
            eprintln!("{err}");
            println!("FAIL");
            1
        }
    }
}

#[cfg(windows)]
fn run_client(run_dir: &str, service: &str) -> i32 {
    let profiles = detect_profiles();
    let config = ClientConfig {
        supported_profiles: profiles,
        max_request_batch_items: 16,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        auth_token: AUTH_TOKEN,
        ..ClientConfig::default()
    };

    let mut client = CgroupsClient::new(run_dir, service, config);
    for _ in 0..200 {
        client.refresh();
        if client.ready() {
            break;
        }
        thread::sleep(Duration::from_millis(10));
    }

    if !client.ready() {
        eprintln!("client: not ready");
        return 1;
    }

    let mut ok = true;

    // --- Test CGROUPS_SNAPSHOT: 3 items ---
    match client.call_snapshot() {
        Ok(view) => {
            if view.item_count != 3 {
                eprintln!("client: expected 3 items, got {}", view.item_count);
                ok = false;
            }
            if view.systemd_enabled != 1 {
                eprintln!(
                    "client: expected systemd_enabled=1, got {}",
                    view.systemd_enabled
                );
                ok = false;
            }
            if view.generation != 42 {
                eprintln!("client: expected generation=42, got {}", view.generation);
                ok = false;
            }

            if let Ok(item0) = view.item(0) {
                if item0.hash != 1001 {
                    eprintln!("client: item 0 hash: got {}", item0.hash);
                    ok = false;
                }
                if item0.name.as_bytes() != b"docker-abc123" {
                    eprintln!("client: item 0 name mismatch");
                    ok = false;
                }
            } else {
                eprintln!("client: item 0 decode failed");
                ok = false;
            }
        }
        Err(e) => {
            eprintln!("client: cgroups call failed: {e:?}");
            ok = false;
        }
    }

    client.close();

    if ok {
        println!("PASS");
        0
    } else {
        println!("FAIL");
        1
    }
}

fn main() {
    #[cfg(windows)]
    {
        let args: Vec<String> = std::env::args().collect();
        if args.len() != 4 {
            eprintln!(
                "Usage: {} <server|client|apps-server|apps-client|cgroups-server|cgroups-client|apps-mixed-server|apps-mixed-client|cgroups-mixed-server|cgroups-mixed-client> <run_dir> <service_name>",
                args[0]
            );
            std::process::exit(1);
        }

        let mode = &args[1];
        let run_dir = &args[2];
        let service = &args[3];

        let rc = match mode.as_str() {
            "server" => run_server(run_dir, service),
            "client" => run_client(run_dir, service),
            "apps-server" => run_apps_server(run_dir, service),
            "apps-client" => run_apps_client(run_dir, service),
            "cgroups-server" => run_cgroups_lookup_server(run_dir, service),
            "cgroups-client" => run_cgroups_lookup_client(run_dir, service),
            "apps-mixed-server" => run_apps_mixed_server(run_dir, service),
            "apps-mixed-client" => run_apps_mixed_client(run_dir, service),
            "cgroups-mixed-server" => run_cgroups_mixed_server(run_dir, service),
            "cgroups-mixed-client" => run_cgroups_mixed_client(run_dir, service),
            _ => {
                eprintln!("Unknown mode: {}", mode);
                1
            }
        };
        std::process::exit(rc);
    }

    #[cfg(not(windows))]
    {
        eprintln!("Windows-only binary");
        std::process::exit(1);
    }
}
