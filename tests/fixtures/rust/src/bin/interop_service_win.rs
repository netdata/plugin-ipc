//! L2 cross-language interop binary (Windows).
//!
//! Usage:
//!   interop_service_win server <run_dir> <service_name>
//!   interop_service_win client <run_dir> <service_name>

#[cfg(windows)]
use netipc::protocol::{PROFILE_BASELINE, PROFILE_SHM_HYBRID};
#[cfg(windows)]
use netipc::service::cgroups::{CgroupsClient, Handlers, ManagedServer};
#[cfg(windows)]
use netipc::transport::windows::{ClientConfig, ServerConfig};
#[cfg(windows)]
use std::thread;
#[cfg(windows)]
use std::time::Duration;

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
fn server_handlers() -> Handlers {
    Handlers {
        on_increment: Some(std::sync::Arc::new(|v| Some(v + 1))),
        on_string_reverse: Some(std::sync::Arc::new(|s| Some(s.chars().rev().collect()))),
        on_snapshot: Some(std::sync::Arc::new(|request, builder| {
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
    }
}

#[cfg(windows)]
fn run_server(run_dir: &str, service: &str) -> i32 {
    let profiles = detect_profiles();
    let config = ServerConfig {
        supported_profiles: profiles,
        max_request_payload_bytes: 4096,
        max_request_batch_items: 16,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        max_response_batch_items: 16,
        auth_token: AUTH_TOKEN,
        ..ServerConfig::default()
    };

    let mut server = ManagedServer::new(run_dir, service, config, server_handlers());

    let stop_flag = server.running_flag();

    println!("READY");

    // Auto-stop after 10 seconds
    let sf = stop_flag.clone();
    std::thread::spawn(move || {
        std::thread::sleep(std::time::Duration::from_secs(10));
        sf.store(false, std::sync::atomic::Ordering::Release);
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
        max_request_batch_items: 16,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        max_response_batch_items: 16,
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

    // --- Test INCREMENT: 42 -> 43 ---
    match client.call_increment(42) {
        Ok(v) if v == 43 => {}
        Ok(v) => {
            eprintln!("client: increment expected 43, got {v}");
            ok = false;
        }
        Err(e) => {
            eprintln!("client: increment call failed: {e:?}");
            ok = false;
        }
    }

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

    // --- Test INCREMENT batch: [10,20,30] -> [11,21,31] ---
    match client.call_increment_batch(&[10, 20, 30]) {
        Ok(ref results) if results == &[11, 21, 31] => {}
        Ok(ref results) => {
            eprintln!("client: increment batch expected [11,21,31], got {results:?}");
            ok = false;
        }
        Err(e) => {
            eprintln!("client: increment batch call failed: {e:?}");
            ok = false;
        }
    }

    // --- Test STRING_REVERSE: "hello" -> "olleh" ---
    match client.call_string_reverse("hello") {
        Ok(ref s) if s.as_str() == "olleh" => {}
        Ok(ref s) => {
            eprintln!(
                "client: string_reverse expected \"olleh\", got \"{}\"",
                s.as_str()
            );
            ok = false;
        }
        Err(e) => {
            eprintln!("client: string_reverse call failed: {e:?}");
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
