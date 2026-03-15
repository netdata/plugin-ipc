//! L2 cross-language interop binary (Windows).
//!
//! Usage:
//!   interop_service_win server <run_dir> <service_name>
//!   interop_service_win client <run_dir> <service_name>

#[cfg(windows)]
use netipc::protocol::{
    CgroupsBuilder, CgroupsRequest, PROFILE_BASELINE,
    METHOD_CGROUPS_SNAPSHOT,
};
#[cfg(windows)]
use netipc::service::cgroups::{CgroupsClient, CgroupsServer};
#[cfg(windows)]
use netipc::transport::windows::{ClientConfig, ServerConfig};

#[cfg(windows)]
const AUTH_TOKEN: u64 = 0xDEADBEEFCAFEBABE;
#[cfg(windows)]
const RESPONSE_BUF_SIZE: usize = 65536;

#[cfg(windows)]
fn test_handler(method_code: u16, request_payload: &[u8]) -> Option<Vec<u8>> {
    if method_code != METHOD_CGROUPS_SNAPSHOT {
        return None;
    }
    if CgroupsRequest::decode(request_payload).is_err() {
        return None;
    }

    let mut buf = vec![0u8; RESPONSE_BUF_SIZE];
    let mut builder = CgroupsBuilder::new(&mut buf, 3, 1, 42);

    let items = [
        (1001u32, 0u32, 1u32, b"docker-abc123" as &[u8], b"/sys/fs/cgroup/docker/abc123" as &[u8]),
        (2002, 0, 1, b"k8s-pod-xyz", b"/sys/fs/cgroup/kubepods/xyz"),
        (3003, 0, 0, b"systemd-user", b"/sys/fs/cgroup/user.slice/user-1000"),
    ];

    for (hash, options, enabled, name, path) in &items {
        if builder.add(*hash, *options, *enabled, name, path).is_err() {
            return None;
        }
    }

    let total = builder.finish();
    buf.truncate(total);
    Some(buf)
}

#[cfg(windows)]
fn run_server(run_dir: &str, service: &str) -> i32 {
    let config = ServerConfig {
        supported_profiles: PROFILE_BASELINE,
        max_request_payload_bytes: 4096,
        max_request_batch_items: 1,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        max_response_batch_items: 1,
        auth_token: AUTH_TOKEN,
        ..ServerConfig::default()
    };

    let mut server = CgroupsServer::new(
        run_dir,
        service,
        config,
        RESPONSE_BUF_SIZE,
        Box::new(test_handler),
    );

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
    let config = ClientConfig {
        supported_profiles: PROFILE_BASELINE,
        max_request_payload_bytes: 4096,
        max_request_batch_items: 1,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        max_response_batch_items: 1,
        auth_token: AUTH_TOKEN,
        ..ClientConfig::default()
    };

    let mut client = CgroupsClient::new(run_dir, service, config);
    client.refresh();

    if !client.ready() {
        eprintln!("client: not ready");
        return 1;
    }

    let mut resp_buf = vec![0u8; RESPONSE_BUF_SIZE];
    match client.call_snapshot(&mut resp_buf) {
        Ok(view) => {
            let mut ok = true;
            if view.item_count != 3 {
                eprintln!("client: expected 3 items, got {}", view.item_count);
                ok = false;
            }
            if view.systemd_enabled != 1 {
                eprintln!("client: expected systemd_enabled=1, got {}", view.systemd_enabled);
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

            client.close();

            if ok {
                println!("PASS");
                0
            } else {
                println!("FAIL");
                1
            }
        }
        Err(e) => {
            eprintln!("client: call failed: {:?}", e);
            client.close();
            println!("FAIL");
            1
        }
    }
}

fn main() {
    #[cfg(windows)]
    {
        let args: Vec<String> = std::env::args().collect();
        if args.len() != 4 {
            eprintln!("Usage: {} <server|client> <run_dir> <service_name>", args[0]);
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
