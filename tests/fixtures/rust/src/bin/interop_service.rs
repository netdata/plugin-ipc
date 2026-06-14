//! L2 cross-language interop binary for the cgroups-snapshot service kind.
//!
//! Usage:
//!   interop_service server <run_dir> <service_name>
//!     Starts a managed server for cgroups-snapshot only (3 items),
//!     prints READY, handles 1 client session, then exits.
//!
//!   interop_service client <run_dir> <service_name>
//!     Connects, calls snapshot, verifies 3 items, prints PASS/FAIL.

#[cfg(windows)]
fn main() {
    eprintln!("interop_service is only supported on POSIX platforms");
    std::process::exit(1);
}

#[cfg(not(windows))]
mod posix_only {

    use netipc::protocol::{
        APPS_CGROUP_HOST_ROOT, APPS_CGROUP_KNOWN, APPS_CGROUP_UNKNOWN_RETRY_LATER,
        CGROUP_LOOKUP_KNOWN, CGROUP_LOOKUP_OVERSIZED_ITEM, CGROUP_LOOKUP_UNKNOWN_PERMANENT,
        CGROUP_LOOKUP_UNKNOWN_RETRY_LATER, NIPC_UID_UNSET, ORCHESTRATOR_DOCKER, ORCHESTRATOR_K8S,
        PID_LOOKUP_KNOWN, PID_LOOKUP_OVERSIZED_ITEM, PID_LOOKUP_UNKNOWN, PROFILE_BASELINE,
        PROFILE_SHM_HYBRID,
    };
    use netipc::service::apps_lookup::{
        AppsLookupClient, Handler as AppsLookupHandler, ManagedServer as AppsLookupServer,
    };
    use netipc::service::cgroups::{
        CgroupsClient, ClientConfig, Handler, ManagedServer, ServerConfig,
    };
    use netipc::service::cgroups_lookup::{
        CgroupsLookupClient, Handler as CgroupsLookupHandler, ManagedServer as CgroupsLookupServer,
    };

    const AUTH_TOKEN: u64 = 0xDEADBEEFCAFEBABE;
    const RESPONSE_BUF_SIZE: usize = 65536;
    const LOOKUP_SCALE_ITEMS_DEFAULT: usize = 8192;
    const LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES: u32 = 8192;
    const LOOKUP_SCALE_CALL_TIMEOUT_MS: u32 = 120_000;
    const LOOKUP_MIXED_ITEMS: usize = 5;

    /// NIPC_PROFILE env var: "shm" enables SHM_HYBRID|BASELINE, default BASELINE only.
    fn detect_profiles() -> u32 {
        match std::env::var("NIPC_PROFILE").as_deref() {
            Ok("shm") => PROFILE_SHM_HYBRID | PROFILE_BASELINE,
            _ => PROFILE_BASELINE,
        }
    }

    fn server_handler() -> Handler {
        Handler {
            handle: Some(std::sync::Arc::new(|request, builder| {
                if request.layout_version != 1 || request.flags != 0 {
                    return false;
                }

                builder.set_header(1, 42);
                let items: &[(u32, u32, u32, &[u8], &[u8])] = &[
                    (
                        1001,
                        0,
                        1,
                        b"docker-abc123",
                        b"/sys/fs/cgroup/docker/abc123",
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

                for &(hash, options, enabled, name, path) in items {
                    if builder.add(hash, options, enabled, name, path).is_err() {
                        return false;
                    }
                }
                true
            })),
            snapshot_max_items: 3,
            ..Handler::default()
        }
    }

    fn server_config() -> ServerConfig {
        let profiles = detect_profiles();
        ServerConfig {
            supported_profiles: profiles,
            preferred_profiles: profiles,
            max_request_batch_items: 16,
            max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
            auth_token: AUTH_TOKEN,
            ..ServerConfig::default()
        }
    }

    fn client_config() -> ClientConfig {
        let profiles = detect_profiles();
        ClientConfig {
            supported_profiles: profiles,
            preferred_profiles: profiles,
            max_request_batch_items: 16,
            max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
            auth_token: AUTH_TOKEN,
            ..ClientConfig::default()
        }
    }

    fn lookup_item_count() -> usize {
        std::env::var("NIPC_LOOKUP_SCALE_ITEMS")
            .ok()
            .and_then(|value| value.parse::<usize>().ok())
            .filter(|value| *value > 0 && *value <= 65_536)
            .unwrap_or(LOOKUP_SCALE_ITEMS_DEFAULT)
    }

    fn lookup_count_u32(value: usize) -> u32 {
        value as u32
    }

    fn apps_lookup_server_config() -> netipc::service::apps_lookup::ServerConfig {
        let profiles = detect_profiles();
        netipc::service::apps_lookup::ServerConfig {
            supported_profiles: profiles,
            preferred_profiles: profiles,
            max_request_payload_bytes: LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES,
            max_request_batch_items: 16,
            max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
            auth_token: AUTH_TOKEN,
        }
    }

    fn apps_lookup_client_config(item_count: usize) -> netipc::service::apps_lookup::ClientConfig {
        let profiles = detect_profiles();
        netipc::service::apps_lookup::ClientConfig {
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

    fn cgroups_lookup_server_config() -> netipc::service::cgroups_lookup::ServerConfig {
        let profiles = detect_profiles();
        netipc::service::cgroups_lookup::ServerConfig {
            supported_profiles: profiles,
            preferred_profiles: profiles,
            max_request_payload_bytes: LOOKUP_SCALE_REQUEST_PAYLOAD_BYTES,
            max_request_batch_items: 16,
            max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
            auth_token: AUTH_TOKEN,
        }
    }

    fn cgroups_lookup_client_config(
        item_count: usize,
    ) -> netipc::service::cgroups_lookup::ClientConfig {
        let profiles = detect_profiles();
        netipc::service::cgroups_lookup::ClientConfig {
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

    fn apps_lookup_handler() -> AppsLookupHandler {
        AppsLookupHandler {
            handle: Some(std::sync::Arc::new(|request, builder| {
                builder.set_generation(9);
                for i in 0..request.item_count {
                    let pid = match request.item(i) {
                        Ok(pid) => pid,
                        Err(_) => return false,
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

    fn cgroups_lookup_handler() -> CgroupsLookupHandler {
        CgroupsLookupHandler {
            handle: Some(std::sync::Arc::new(|request, builder| {
                builder.set_generation(7);
                for i in 0..request.item_count {
                    let path = match request.item(i) {
                        Ok(path) => path,
                        Err(_) => return false,
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

    fn apps_lookup_mixed_handler() -> AppsLookupHandler {
        AppsLookupHandler {
            handle: Some(std::sync::Arc::new(|request, builder| {
                builder.set_generation(19);
                for i in 0..request.item_count {
                    let pid = match request.item(i) {
                        Ok(pid) => pid,
                        Err(_) => return false,
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

    fn cgroups_lookup_mixed_handler() -> CgroupsLookupHandler {
        CgroupsLookupHandler {
            handle: Some(std::sync::Arc::new(|request, builder| {
                builder.set_generation(17);
                for i in 0..request.item_count {
                    let path = match request.item(i) {
                        Ok(path) => path,
                        Err(_) => return false,
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

    fn run_server(run_dir: &str, service: &str) -> Result<(), Box<dyn std::error::Error>> {
        let mut server = ManagedServer::new(run_dir, service, server_config(), server_handler());

        // Signal readiness
        println!("READY");

        // Stop after 10s timeout (interop test should finish sooner)
        let stop_flag = server.running_flag();
        std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_secs(10));
            stop_flag.store(false, std::sync::atomic::Ordering::Release);
        });

        server.run()?;
        Ok(())
    }

    fn run_apps_server(run_dir: &str, service: &str) -> Result<(), Box<dyn std::error::Error>> {
        let mut server = AppsLookupServer::with_workers(
            run_dir,
            service,
            apps_lookup_server_config(),
            apps_lookup_handler(),
            8,
        );

        println!("READY");

        let stop_flag = server.running_flag();
        std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_secs(10));
            stop_flag.store(false, std::sync::atomic::Ordering::Release);
        });

        server.run()?;
        Ok(())
    }

    fn run_cgroups_lookup_server(
        run_dir: &str,
        service: &str,
    ) -> Result<(), Box<dyn std::error::Error>> {
        let mut server = CgroupsLookupServer::with_workers(
            run_dir,
            service,
            cgroups_lookup_server_config(),
            cgroups_lookup_handler(),
            8,
        );

        println!("READY");

        let stop_flag = server.running_flag();
        std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_secs(10));
            stop_flag.store(false, std::sync::atomic::Ordering::Release);
        });

        server.run()?;
        Ok(())
    }

    fn run_apps_mixed_server(
        run_dir: &str,
        service: &str,
    ) -> Result<(), Box<dyn std::error::Error>> {
        let mut server = AppsLookupServer::with_workers(
            run_dir,
            service,
            apps_lookup_server_config(),
            apps_lookup_mixed_handler(),
            2,
        );

        println!("READY");

        let stop_flag = server.running_flag();
        std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_secs(10));
            stop_flag.store(false, std::sync::atomic::Ordering::Release);
        });

        server.run()?;
        Ok(())
    }

    fn run_cgroups_mixed_server(
        run_dir: &str,
        service: &str,
    ) -> Result<(), Box<dyn std::error::Error>> {
        let mut server = CgroupsLookupServer::with_workers(
            run_dir,
            service,
            cgroups_lookup_server_config(),
            cgroups_lookup_mixed_handler(),
            2,
        );

        println!("READY");

        let stop_flag = server.running_flag();
        std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_secs(10));
            stop_flag.store(false, std::sync::atomic::Ordering::Release);
        });

        server.run()?;
        Ok(())
    }

    fn large_lookup_pids(item_count: usize) -> Vec<u32> {
        (0..item_count).map(|i| 100000u32 + i as u32).collect()
    }

    fn large_lookup_paths(item_count: usize) -> Vec<Vec<u8>> {
        (0..item_count)
            .map(|i| format!("/cg/{i:05}").into_bytes())
            .collect()
    }

    fn wait_apps_client_ready(client: &mut AppsLookupClient) -> bool {
        for _ in 0..200 {
            client.refresh();
            if client.ready() {
                return true;
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        false
    }

    fn wait_cgroups_lookup_client_ready(client: &mut CgroupsLookupClient) -> bool {
        for _ in 0..200 {
            client.refresh();
            if client.ready() {
                return true;
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        false
    }

    fn run_apps_client(run_dir: &str, service: &str) -> Result<(), Box<dyn std::error::Error>> {
        let item_count = lookup_item_count();
        let pids = large_lookup_pids(item_count);
        let mut client =
            AppsLookupClient::new(run_dir, service, apps_lookup_client_config(item_count));
        client.set_call_timeout(LOOKUP_SCALE_CALL_TIMEOUT_MS);

        if !wait_apps_client_ready(&mut client) {
            return Err("apps client not ready".into());
        }

        let view = client.call(&pids)?;
        if view.item_count != lookup_count_u32(pids.len()) || view.generation != 9 {
            return Err(format!(
                "apps bad header count={} generation={}",
                view.item_count, view.generation
            )
            .into());
        }
        for (i, expected) in pids.iter().enumerate() {
            let item = view.item(lookup_count_u32(i))?;
            if item.status != PID_LOOKUP_KNOWN
                || item.pid != *expected
                || item.comm.as_bytes() != b"ok"
                || item.cgroup_path.as_bytes() != b"/ok"
            {
                return Err(format!("apps bad item {i}").into());
            }
        }

        client.close();
        println!("PASS");
        Ok(())
    }

    fn run_cgroups_lookup_client(
        run_dir: &str,
        service: &str,
    ) -> Result<(), Box<dyn std::error::Error>> {
        let item_count = lookup_item_count();
        let paths = large_lookup_paths(item_count);
        let path_refs: Vec<&[u8]> = paths.iter().map(Vec::as_slice).collect();
        let mut client =
            CgroupsLookupClient::new(run_dir, service, cgroups_lookup_client_config(item_count));
        client.set_call_timeout(LOOKUP_SCALE_CALL_TIMEOUT_MS);

        if !wait_cgroups_lookup_client_ready(&mut client) {
            return Err("cgroups lookup client not ready".into());
        }

        let view = client.call(&path_refs)?;
        if view.item_count != lookup_count_u32(paths.len()) || view.generation != 7 {
            return Err(format!(
                "cgroups lookup bad header count={} generation={}",
                view.item_count, view.generation
            )
            .into());
        }
        for (i, expected) in paths.iter().enumerate() {
            let item = view.item(lookup_count_u32(i))?;
            if item.status != CGROUP_LOOKUP_KNOWN
                || item.path.as_bytes() != expected.as_slice()
                || item.name.as_bytes() != b"ok"
            {
                return Err(format!("cgroups lookup bad item {i}").into());
            }
        }

        client.close();
        println!("PASS");
        Ok(())
    }

    fn run_apps_mixed_client(
        run_dir: &str,
        service: &str,
    ) -> Result<(), Box<dyn std::error::Error>> {
        let pids = [1001, 1002, 1003, 1004, 1005];
        let mut client = AppsLookupClient::new(
            run_dir,
            service,
            apps_lookup_client_config(LOOKUP_MIXED_ITEMS),
        );
        client.set_call_timeout(LOOKUP_SCALE_CALL_TIMEOUT_MS);

        if !wait_apps_client_ready(&mut client) {
            return Err("apps mixed client not ready".into());
        }

        let view = client.call(&pids)?;
        if view.item_count != LOOKUP_MIXED_ITEMS as u32 || view.generation != 19 {
            return Err(format!(
                "apps mixed bad header count={} generation={}",
                view.item_count, view.generation
            )
            .into());
        }

        let item0 = view.item(0)?;
        if item0.status != PID_LOOKUP_KNOWN
            || item0.cgroup_status != APPS_CGROUP_KNOWN
            || item0.pid != 1001
            || item0.comm.as_bytes() != b"known"
            || item0.cgroup_path.as_bytes() != b"/cg/known"
            || item0.label_count != 1
        {
            return Err("apps mixed bad item 0".into());
        }
        let label = item0.label(0)?;
        if label.key.as_bytes() != b"role" || label.value.as_bytes() != b"api" {
            return Err("apps mixed bad item 0 label".into());
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
            let item = view.item(index)?;
            if item.pid != pid
                || item.status != status
                || item.cgroup_status != cgroup_status
                || item.comm.as_bytes() != comm
            {
                return Err(format!("apps mixed bad item {index}").into());
            }
        }

        client.close();
        println!("PASS");
        Ok(())
    }

    fn run_cgroups_mixed_client(
        run_dir: &str,
        service: &str,
    ) -> Result<(), Box<dyn std::error::Error>> {
        let paths: [&[u8]; LOOKUP_MIXED_ITEMS] = [
            b"/known",
            b"/retry",
            b"/permanent",
            b"/oversized",
            b"/known2",
        ];
        let mut client = CgroupsLookupClient::new(
            run_dir,
            service,
            cgroups_lookup_client_config(LOOKUP_MIXED_ITEMS),
        );
        client.set_call_timeout(LOOKUP_SCALE_CALL_TIMEOUT_MS);

        if !wait_cgroups_lookup_client_ready(&mut client) {
            return Err("cgroups mixed client not ready".into());
        }

        let view = client.call(&paths)?;
        if view.item_count != LOOKUP_MIXED_ITEMS as u32 || view.generation != 17 {
            return Err(format!(
                "cgroups mixed bad header count={} generation={}",
                view.item_count, view.generation
            )
            .into());
        }

        let item0 = view.item(0)?;
        if item0.status != CGROUP_LOOKUP_KNOWN
            || item0.path.as_bytes() != b"/known"
            || item0.name.as_bytes() != b"pod-a"
            || item0.label_count != 1
        {
            return Err("cgroups mixed bad item 0".into());
        }
        let label = item0.label(0)?;
        if label.key.as_bytes() != b"role" || label.value.as_bytes() != b"db" {
            return Err("cgroups mixed bad item 0 label".into());
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
            let item = view.item(index)?;
            if item.status != status || item.path.as_bytes() != path || item.name.as_bytes() != name
            {
                return Err(format!("cgroups mixed bad item {index}").into());
            }
        }

        client.close();
        println!("PASS");
        Ok(())
    }

    fn run_client(run_dir: &str, service: &str) -> Result<(), Box<dyn std::error::Error>> {
        let mut client = CgroupsClient::new(run_dir, service, client_config());
        for _ in 0..200 {
            client.refresh();
            if client.ready() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }

        if !client.ready() {
            return Err("client not ready".into());
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

                match view.item(0) {
                    Ok(item0) => {
                        if item0.hash != 1001 {
                            eprintln!("client: item 0 hash: got {}", item0.hash);
                            ok = false;
                        }
                        if item0.name.as_bytes() != b"docker-abc123" {
                            eprintln!("client: item 0 name mismatch");
                            ok = false;
                        }
                        if item0.path.as_bytes() != b"/sys/fs/cgroup/docker/abc123" {
                            eprintln!("client: item 0 path mismatch");
                            ok = false;
                        }
                    }
                    Err(e) => {
                        eprintln!("client: item 0 decode failed: {e:?}");
                        ok = false;
                    }
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
            Ok(())
        } else {
            println!("FAIL");
            std::process::exit(1);
        }
    }

    pub(crate) fn main() {
        // Ignore SIGPIPE
        unsafe {
            libc::signal(libc::SIGPIPE, libc::SIG_IGN);
        }

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

        let _ = std::fs::create_dir_all(run_dir);

        let result = match mode.as_str() {
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
                eprintln!("Unknown mode: {mode}");
                std::process::exit(1);
            }
        };

        if let Err(e) = result {
            eprintln!("{mode}: {e}");
            std::process::exit(1);
        }
    }
}

#[cfg(not(windows))]
fn main() {
    posix_only::main();
}
