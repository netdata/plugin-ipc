//! L2 cross-language interop binary.
//!
//! Usage:
//!   interop_service server <run_dir> <service_name>
//!     Starts a managed server with a cgroups handler (3 items),
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

    use netipc::protocol::{PROFILE_BASELINE, PROFILE_SHM_HYBRID};
    use netipc::service::cgroups::{CgroupsClient, Handlers, ManagedServer};
    use netipc::transport::posix::{ClientConfig, ServerConfig};

    const AUTH_TOKEN: u64 = 0xDEADBEEFCAFEBABE;
    const RESPONSE_BUF_SIZE: usize = 65536;

    /// NIPC_PROFILE env var: "shm" enables SHM_HYBRID|BASELINE, default BASELINE only.
    fn detect_profiles() -> u32 {
        match std::env::var("NIPC_PROFILE").as_deref() {
            Ok("shm") => PROFILE_SHM_HYBRID | PROFILE_BASELINE,
            _ => PROFILE_BASELINE,
        }
    }

    fn server_handlers() -> Handlers {
        Handlers {
            on_increment: Some(std::sync::Arc::new(|v| Some(v + 1))),
            on_string_reverse: Some(std::sync::Arc::new(|s| Some(s.chars().rev().collect()))),
            on_snapshot: Some(std::sync::Arc::new(|request, builder| {
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
        }
    }

    fn server_config() -> ServerConfig {
        let profiles = detect_profiles();
        ServerConfig {
            supported_profiles: profiles,
            preferred_profiles: profiles,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 16,
            max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
            max_response_batch_items: 16,
            auth_token: AUTH_TOKEN,
            backlog: 4,
            ..ServerConfig::default()
        }
    }

    fn client_config() -> ClientConfig {
        let profiles = detect_profiles();
        ClientConfig {
            supported_profiles: profiles,
            preferred_profiles: profiles,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 16,
            max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
            max_response_batch_items: 16,
            auth_token: AUTH_TOKEN,
            ..ClientConfig::default()
        }
    }

    fn run_server(run_dir: &str, service: &str) -> Result<(), Box<dyn std::error::Error>> {
        let mut server = ManagedServer::new(run_dir, service, server_config(), server_handlers());

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

    fn run_client(run_dir: &str, service: &str) -> Result<(), Box<dyn std::error::Error>> {
        let mut client = CgroupsClient::new(run_dir, service, client_config());
        client.refresh();

        if !client.ready() {
            return Err("client not ready".into());
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
                "Usage: {} <server|client> <run_dir> <service_name>",
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
