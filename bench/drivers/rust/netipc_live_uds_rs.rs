use netipc::transport::posix::{
    UdsSeqpacketClient, UdsSeqpacketConfig, UdsSeqpacketServer, PROFILE_SHM_HYBRID,
};
use netipc::{IncrementRequest, IncrementResponse, STATUS_OK};
use std::env;
use std::io;
use std::process;
use std::thread;
use std::time::{Duration, Instant};

fn usage(argv0: &str) {
    eprintln!("usage:");
    eprintln!("  {argv0} server-once <run_dir> <service>");
    eprintln!("  {argv0} client-once <run_dir> <service> <value>");
    eprintln!("  {argv0} server-loop <run_dir> <service> <max_requests|0>");
    eprintln!("  {argv0} client-bench <run_dir> <service> <duration_sec> <target_rps>");
}

fn parse_u64(v: &str) -> u64 {
    v.parse::<u64>().unwrap_or_else(|_| {
        eprintln!("invalid u64: {v}");
        process::exit(2);
    })
}

fn parse_i32(v: &str) -> i32 {
    v.parse::<i32>().unwrap_or_else(|_| {
        eprintln!("invalid i32: {v}");
        process::exit(2);
    })
}

fn parse_env_u32(name: &str) -> Option<u32> {
    let raw = env::var(name).ok()?;
    if raw.trim().is_empty() {
        return None;
    }
    raw.parse::<u32>().ok()
}

fn parse_env_u64(name: &str) -> Option<u64> {
    let raw = env::var(name).ok()?;
    if raw.trim().is_empty() {
        return None;
    }
    raw.parse::<u64>().ok()
}

fn uds_config(run_dir: &str, service: &str) -> UdsSeqpacketConfig {
    let mut config = UdsSeqpacketConfig::new(run_dir, service);
    if let Some(supported) = parse_env_u32("NETIPC_SUPPORTED_PROFILES") {
        config.supported_profiles = supported;
    }
    if let Some(preferred) = parse_env_u32("NETIPC_PREFERRED_PROFILES") {
        config.preferred_profiles = preferred;
    }
    if let Some(auth_token) = parse_env_u64("NETIPC_AUTH_TOKEN") {
        config.auth_token = auth_token;
    }
    config
}

fn protocol_error(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

fn percentile_micros(lat_ns: &[i64], pct: f64) -> f64 {
    if lat_ns.is_empty() {
        return 0.0;
    }
    if pct <= 0.0 {
        return lat_ns[0] as f64 / 1000.0;
    }
    if pct >= 100.0 {
        return lat_ns[lat_ns.len() - 1] as f64 / 1000.0;
    }

    let rank = ((pct / 100.0) * (lat_ns.len() as f64 - 1.0)) as usize;
    lat_ns[rank] as f64 / 1000.0
}

fn self_cpu_seconds() -> f64 {
    unsafe {
        let mut ru: libc::rusage = std::mem::zeroed();
        if libc::getrusage(libc::RUSAGE_SELF, &mut ru as *mut libc::rusage) != 0 {
            return 0.0;
        }

        ru.ru_utime.tv_sec as f64
            + ru.ru_utime.tv_usec as f64 / 1e6
            + ru.ru_stime.tv_sec as f64
            + ru.ru_stime.tv_usec as f64 / 1e6
    }
}

fn sleep_until(start: Instant, target_ns: u64) {
    loop {
        let now_ns = start.elapsed().as_nanos() as u64;
        if now_ns >= target_ns {
            return;
        }

        let diff = target_ns - now_ns;
        let sleep_ns = if diff > 2_000_000 {
            diff - 200_000
        } else if diff > 200_000 {
            diff - 50_000
        } else {
            diff
        };
        thread::sleep(Duration::from_nanos(sleep_ns));
    }
}

fn server_once(run_dir: &str, service: &str) -> io::Result<()> {
    let mut server = UdsSeqpacketServer::bind(&uds_config(run_dir, service))?;
    server.accept(Some(Duration::from_secs(10)))?;
    let profile = server.negotiated_profile();
    let (request_id, request) = server.receive_increment(Some(Duration::from_secs(10)))?;
    let response = IncrementResponse {
        status: STATUS_OK,
        value: request.value + 1,
    };
    server.send_increment(request_id, &response, Some(Duration::from_secs(10)))?;

    println!(
        "RUST-UDS-SERVER request_id={request_id} value={} response={} profile={profile}",
        request.value, response.value
    );
    Ok(())
}

fn client_once(run_dir: &str, service: &str, value: u64) -> io::Result<()> {
    let mut client =
        UdsSeqpacketClient::connect(&uds_config(run_dir, service), Some(Duration::from_secs(10)))?;
    let profile = client.negotiated_profile();
    let response =
        client.call_increment(&IncrementRequest { value }, Some(Duration::from_secs(10)))?;

    if response.status != STATUS_OK {
        return Err(protocol_error("server returned non-OK status"));
    }
    if response.value != value + 1 {
        return Err(protocol_error("unexpected response value"));
    }

    println!(
        "RUST-UDS-CLIENT request={value} response={} profile={profile}",
        response.value
    );
    Ok(())
}

fn server_loop(run_dir: &str, service: &str, max_requests: u64) -> io::Result<()> {
    let mut server = UdsSeqpacketServer::bind(&uds_config(run_dir, service))?;
    server.accept(Some(Duration::from_secs(10)))?;

    let mut handled = 0u64;
    while max_requests == 0 || handled < max_requests {
        let (request_id, request) = server.receive_increment(Some(Duration::from_secs(10)))?;
        let response = IncrementResponse {
            status: STATUS_OK,
            value: request.value + 1,
        };
        server.send_increment(request_id, &response, Some(Duration::from_secs(10)))?;
        handled += 1;
    }

    Ok(())
}

fn client_bench(
    run_dir: &str,
    service: &str,
    duration_sec: i32,
    target_rps: i32,
) -> io::Result<()> {
    if duration_sec <= 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "duration_sec must be > 0",
        ));
    }
    if target_rps < 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "target_rps must be >= 0",
        ));
    }

    let mut client =
        UdsSeqpacketClient::connect(&uds_config(run_dir, service), Some(Duration::from_secs(10)))?;
    let profile = client.negotiated_profile();

    let start = Instant::now();
    let end_at = start + Duration::from_secs(duration_sec as u64);
    let cpu_start = self_cpu_seconds();

    let mut lat_ns: Vec<i64> = Vec::with_capacity(1 << 20);
    let mut counter = 1u64;
    let mut requests = 0i64;
    let mut responses = 0i64;
    let mut mismatches = 0i64;

    let interval_ns: u64 = if target_rps > 0 {
        let value = 1_000_000_000u64 / target_rps as u64;
        if value == 0 {
            1
        } else {
            value
        }
    } else {
        0
    };
    let mut next_send_ns = 0u64;

    while Instant::now() < end_at {
        if interval_ns > 0 {
            sleep_until(start, next_send_ns);
            next_send_ns = next_send_ns.saturating_add(interval_ns);
        }

        let send_start = Instant::now();
        requests += 1;

        let response = client.call_increment(
            &IncrementRequest { value: counter },
            Some(Duration::from_secs(10)),
        )?;
        if response.status != STATUS_OK || response.value != counter + 1 {
            mismatches += 1;
        }

        counter = response.value;
        responses += 1;
        lat_ns.push(send_start.elapsed().as_nanos() as i64);
    }

    let elapsed_sec = start.elapsed().as_secs_f64().max(1e-9);
    let cpu_cores = (self_cpu_seconds() - cpu_start) / elapsed_sec;
    let throughput = responses as f64 / elapsed_sec;

    lat_ns.sort_unstable();
    let p50 = percentile_micros(&lat_ns, 50.0);
    let p95 = percentile_micros(&lat_ns, 95.0);
    let p99 = percentile_micros(&lat_ns, 99.0);

    println!(
        "mode,duration_sec,target_rps,requests,responses,mismatches,throughput_rps,p50_us,p95_us,p99_us,client_cpu_cores"
    );
    let mode = if profile == PROFILE_SHM_HYBRID {
        "rust-shm-hybrid"
    } else {
        "rust-uds"
    };
    println!(
        "{},{},{},{},{},{},{:.2},{:.2},{:.2},{:.2},{:.3}",
        mode,
        duration_sec,
        target_rps,
        requests,
        responses,
        mismatches,
        throughput,
        p50,
        p95,
        p99,
        cpu_cores
    );

    Ok(())
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        usage(&args[0]);
        process::exit(2);
    }

    let result = match args[1].as_str() {
        "server-once" => {
            if args.len() != 4 {
                usage(&args[0]);
                process::exit(2);
            }
            server_once(&args[2], &args[3])
        }
        "client-once" => {
            if args.len() != 5 {
                usage(&args[0]);
                process::exit(2);
            }
            client_once(&args[2], &args[3], parse_u64(&args[4]))
        }
        "server-loop" => {
            if args.len() != 5 {
                usage(&args[0]);
                process::exit(2);
            }
            server_loop(&args[2], &args[3], parse_u64(&args[4]))
        }
        "client-bench" => {
            if args.len() != 6 {
                usage(&args[0]);
                process::exit(2);
            }
            client_bench(&args[2], &args[3], parse_i32(&args[4]), parse_i32(&args[5]))
        }
        _ => {
            usage(&args[0]);
            process::exit(2);
        }
    };

    if let Err(err) = result {
        eprintln!("netipc_live_uds_rs failed: {err}");
        process::exit(1);
    }
}
