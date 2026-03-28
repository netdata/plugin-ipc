//! L3 cross-language cache interop binary (Windows).
//!
//! Usage:
//!   interop_cache_win server <run_dir> <service_name>
//!   interop_cache_win client <run_dir> <service_name>

#[cfg(windows)]
use netipc::protocol::{PROFILE_BASELINE, PROFILE_SHM_HYBRID};
#[cfg(windows)]
use netipc::service::cgroups::{
    CgroupsCache, CgroupsClient, ClientConfig, Handler, ManagedServer, ServerConfig,
};

#[cfg(windows)]
const AUTH_TOKEN: u64 = 0xDEADBEEFCAFEBABE;
#[cfg(windows)]
const RESPONSE_BUF_SIZE: usize = 65536;

/// NIPC_PROFILE env var: "shm" enables SHM_HYBRID|BASELINE, default BASELINE only.
#[cfg(windows)]
fn detect_profiles() -> u32 {
    match std::env::var("NIPC_PROFILE").as_deref() {
        Ok("shm") => PROFILE_SHM_HYBRID | PROFILE_BASELINE,
        _ => PROFILE_BASELINE,
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
        max_request_payload_bytes: 4096,
        max_request_batch_items: 1,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        max_response_batch_items: 1,
        auth_token: AUTH_TOKEN,
        ..ServerConfig::default()
    };

    let mut server = ManagedServer::new(run_dir, service, config, server_handler());
    let stop_flag = server.running_flag();
    let wake_run_dir = run_dir.to_string();
    let wake_service = service.to_string();
    let wake_config = ClientConfig {
        supported_profiles: profiles,
        max_request_payload_bytes: 4096,
        max_request_batch_items: 1,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        max_response_batch_items: 1,
        auth_token: AUTH_TOKEN,
        ..ClientConfig::default()
    };

    println!("READY");

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
fn run_client(run_dir: &str, service: &str) -> i32 {
    let profiles = detect_profiles();
    let config = ClientConfig {
        supported_profiles: profiles,
        max_request_payload_bytes: 4096,
        max_request_batch_items: 1,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        max_response_batch_items: 1,
        auth_token: AUTH_TOKEN,
        ..ClientConfig::default()
    };

    let mut cache = CgroupsCache::new(run_dir, service, config);

    let updated = cache.refresh();
    if !updated || !cache.ready() {
        eprintln!("client: cache not ready after refresh");
        cache.close();
        println!("FAIL");
        return 1;
    }

    let mut ok = true;

    let status = cache.status();
    if status.item_count != 3 {
        eprintln!("client: expected 3 items, got {}", status.item_count);
        ok = false;
    }
    if status.systemd_enabled != 1 {
        eprintln!(
            "client: expected systemd_enabled=1, got {}",
            status.systemd_enabled
        );
        ok = false;
    }
    if status.generation != 42 {
        eprintln!("client: expected generation=42, got {}", status.generation);
        ok = false;
    }

    match cache.lookup(1001, "docker-abc123") {
        Some(item) => {
            if item.hash != 1001 {
                eprintln!("client: item hash: got {}", item.hash);
                ok = false;
            }
            if item.name != "docker-abc123" {
                eprintln!("client: item name mismatch");
                ok = false;
            }
            if item.path != "/sys/fs/cgroup/docker/abc123" {
                eprintln!("client: item path mismatch");
                ok = false;
            }
        }
        None => {
            eprintln!("client: item 1001 not found");
            ok = false;
        }
    }

    if cache.lookup(9999, "nonexistent").is_some() {
        eprintln!("client: nonexistent item should be None");
        ok = false;
    }

    cache.close();

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
                "Usage: {} <server|client> <run_dir> <service_name>",
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
