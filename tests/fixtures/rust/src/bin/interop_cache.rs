//! L3 cross-language cache interop binary.
//!
//! Usage:
//!   interop_cache server <run_dir> <service_name>
//!     Starts a managed L2 server with a cgroups handler (3 items),
//!     prints READY, handles clients, exits after ~10s.
//!
//!   interop_cache client <run_dir> <service_name>
//!     Creates L3 cache, refreshes, verifies lookup, prints PASS/FAIL.

#[cfg(windows)]
fn main() {
    eprintln!("interop_cache is only supported on POSIX platforms");
    std::process::exit(1);
}

#[cfg(not(windows))]
mod posix_only {

    use netipc::protocol::{PROFILE_BASELINE, PROFILE_SHM_HYBRID};
    use netipc::service::cgroups::{
        CgroupsCache, ClientConfig, Handler, ManagedServer, ServerConfig,
    };

    const AUTH_TOKEN: u64 = 0xDEADBEEFCAFEBABE;
    const RESPONSE_BUF_SIZE: usize = 65536;

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
            max_request_payload_bytes: 4096,
            max_request_batch_items: 1,
            max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
            max_response_batch_items: 1,
            auth_token: AUTH_TOKEN,
            ..ServerConfig::default()
        }
    }

    fn client_config() -> ClientConfig {
        let profiles = detect_profiles();
        ClientConfig {
            supported_profiles: profiles,
            preferred_profiles: profiles,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 1,
            max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
            max_response_batch_items: 1,
            auth_token: AUTH_TOKEN,
            ..ClientConfig::default()
        }
    }

    fn run_server(run_dir: &str, service: &str) -> Result<(), Box<dyn std::error::Error>> {
        let mut server = ManagedServer::new(run_dir, service, server_config(), server_handler());

        println!("READY");

        let stop_flag = server.running_flag();
        std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_secs(10));
            stop_flag.store(false, std::sync::atomic::Ordering::Release);
        });

        server.run()?;
        Ok(())
    }

    fn run_client(run_dir: &str, service: &str) -> Result<(), Box<dyn std::error::Error>> {
        let mut cache = CgroupsCache::new(run_dir, service, client_config());

        // Refresh to populate cache
        let updated = cache.refresh();
        if !updated || !cache.ready() {
            return Err("cache not ready after refresh".into());
        }

        let mut ok = true;

        // Verify status
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

        // Verify lookups
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

        // Verify not-found
        if cache.lookup(9999, "nonexistent").is_some() {
            eprintln!("client: nonexistent item should be None");
            ok = false;
        }

        cache.close();

        if ok {
            println!("PASS");
            Ok(())
        } else {
            println!("FAIL");
            std::process::exit(1);
        }
    }

    pub(crate) fn main() {
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
