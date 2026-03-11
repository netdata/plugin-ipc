use netipc::transport::windows::{
    NamedPipeClient, NamedPipeConfig, NamedPipeServer, PROFILE_SHM_HYBRID,
    self_cpu_seconds,
};
use netipc::{IncrementRequest, IncrementResponse, STATUS_OK};
use std::env;
use std::io;
use std::process;
use std::thread;
use std::time::{Duration, Instant};

// ---------------------------------------------------------------------------
// RDTSC-based timing: avoids ~4.5us QPC overhead under Hyper-V
// ---------------------------------------------------------------------------

#[cfg(target_arch = "x86_64")]
#[inline(always)]
fn rdtsc_now() -> u64 {
    unsafe {
        core::arch::x86_64::_mm_lfence();
        core::arch::x86_64::_rdtsc()
    }
}

static mut TSC_NS_PER_TICK: f64 = 0.0;

fn calibrate_tsc() {
    // Warmup
    let _ = rdtsc_now();
    thread::sleep(Duration::from_millis(200));

    const CAL_PASSES: usize = 5;
    let mut samples = [0.0f64; CAL_PASSES];

    for sample in samples.iter_mut() {
        let tsc0 = rdtsc_now();
        let wall0 = Instant::now();
        thread::sleep(Duration::from_millis(200));
        let tsc1 = rdtsc_now();
        let wall_sec = wall0.elapsed().as_secs_f64();

        if wall_sec < 0.05 {
            // Bad sample, retry
            *sample = 0.0;
            continue;
        }
        *sample = (tsc1 - tsc0) as f64 / wall_sec;
    }

    samples.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let tsc_hz = samples[CAL_PASSES / 2];

    let ns_per_tick = 1e9 / tsc_hz;
    eprintln!("TSC calibrated: {:.2} GHz ({:.3} ns/tick)", tsc_hz / 1e9, ns_per_tick);
    unsafe { TSC_NS_PER_TICK = ns_per_tick; }
}

#[inline(always)]
fn tsc_delta_ns(delta: u64) -> f64 {
    delta as f64 * unsafe { TSC_NS_PER_TICK }
}

// ---------------------------------------------------------------------------

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
    if raw.trim().is_empty() { return None; }
    raw.parse::<u32>().ok()
}

fn parse_env_u64(name: &str) -> Option<u64> {
    let raw = env::var(name).ok()?;
    if raw.trim().is_empty() { return None; }
    raw.parse::<u64>().ok()
}

fn win_config(run_dir: &str, service: &str) -> NamedPipeConfig {
    let mut config = NamedPipeConfig::new(run_dir, service);
    if let Some(v) = parse_env_u32("NETIPC_SUPPORTED_PROFILES") {
        config.supported_profiles = v;
    }
    if let Some(v) = parse_env_u32("NETIPC_PREFERRED_PROFILES") {
        config.preferred_profiles = v;
    }
    if let Some(v) = parse_env_u64("NETIPC_AUTH_TOKEN") {
        config.auth_token = v;
    }
    if let Some(v) = parse_env_u32("NETIPC_SHM_SPIN_TRIES") {
        config.shm_spin_tries = v;
    }
    config
}

fn protocol_error(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

fn percentile_micros(lat_ns: &[f64], pct: f64) -> f64 {
    if lat_ns.is_empty() { return 0.0; }
    if pct <= 0.0 { return lat_ns[0] / 1000.0; }
    if pct >= 100.0 { return lat_ns[lat_ns.len() - 1] / 1000.0; }
    let rank = ((pct / 100.0) * (lat_ns.len() as f64 - 1.0)) as usize;
    lat_ns[rank] / 1000.0
}

fn sleep_until(start: Instant, target_ns: u64) {
    loop {
        let now_ns = start.elapsed().as_nanos() as u64;
        if now_ns >= target_ns { return; }
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
    let mut server = NamedPipeServer::bind(&win_config(run_dir, service))?;
    server.accept(Some(Duration::from_secs(10)))?;
    let profile = server.negotiated_profile();
    let (request_id, request) = server.receive_increment(Some(Duration::from_secs(10)))?;
    let response = IncrementResponse {
        status: STATUS_OK,
        value: request.value + 1,
    };
    server.send_increment(request_id, &response, Some(Duration::from_secs(10)))?;
    println!(
        "RUST-WIN-SERVER request_id={request_id} value={} response={} profile={profile}",
        request.value, response.value
    );
    Ok(())
}

fn client_once(run_dir: &str, service: &str, value: u64) -> io::Result<()> {
    let mut client =
        NamedPipeClient::connect(&win_config(run_dir, service), Some(Duration::from_secs(10)))?;
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
        "RUST-WIN-CLIENT request={value} response={} profile={profile}",
        response.value
    );
    Ok(())
}

fn server_loop(run_dir: &str, service: &str, max_requests: u64) -> io::Result<()> {
    let mut server = NamedPipeServer::bind(&win_config(run_dir, service))?;
    server.accept(Some(Duration::from_secs(10)))?;

    let cpu_start = self_cpu_seconds();
    let wall_start = Instant::now();

    let mut handled = 0u64;
    while max_requests == 0 || handled < max_requests {
        let (request_id, request) = match server.receive_increment(None) {
            Ok(v) => v,
            Err(e) if e.kind() == io::ErrorKind::BrokenPipe => break,
            Err(e) if e.kind() == io::ErrorKind::TimedOut => break,
            Err(e) => return Err(e),
        };
        let response = IncrementResponse {
            status: STATUS_OK,
            value: request.value + 1,
        };
        server.send_increment(request_id, &response, None)?;
        handled += 1;
    }

    let elapsed = wall_start.elapsed().as_secs_f64().max(1e-9);
    let cpu_cores = (self_cpu_seconds() - cpu_start) / elapsed;
    eprintln!("SERVER_CPU_CORES={cpu_cores:.3}");

    Ok(())
}

fn client_bench(
    run_dir: &str,
    service: &str,
    duration_sec: i32,
    target_rps: i32,
) -> io::Result<()> {
    if duration_sec <= 0 {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "duration_sec must be > 0"));
    }
    if target_rps < 0 {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "target_rps must be >= 0"));
    }

    calibrate_tsc();

    let mut client =
        NamedPipeClient::connect(&win_config(run_dir, service), Some(Duration::from_secs(10)))?;
    let profile = client.negotiated_profile();

    let start = Instant::now();
    let end_at = start + Duration::from_secs(duration_sec as u64);
    let cpu_start = self_cpu_seconds();

    // Maximum plausible latency in TSC ticks (~100ms).
    // Discard insane deltas caused by cross-core TSC offset differences.
    let tsc_hz = 1e9 / unsafe { TSC_NS_PER_TICK };
    let max_sane_tsc_delta = (0.1 * tsc_hz) as u64;

    let mut lat_ns: Vec<f64> = Vec::with_capacity(1 << 20);
    let mut counter = 1u64;
    let mut requests = 0u64;
    let mut responses = 0u64;
    let mut mismatches = 0u64;

    let interval_ns: u64 = if target_rps > 0 {
        let v = 1_000_000_000u64 / target_rps as u64;
        if v == 0 { 1 } else { v }
    } else {
        0
    };
    let mut next_send_ns = 0u64;

    const DEADLINE_CHECK_MASK: u64 = 1023;

    loop {
        // Check QPC deadline every 1024 iterations to amortize the ~4.5us cost
        if (requests & DEADLINE_CHECK_MASK) == 0 && requests > 0 {
            if Instant::now() >= end_at {
                break;
            }
        }

        if interval_ns > 0 {
            sleep_until(start, next_send_ns);
            next_send_ns = next_send_ns.saturating_add(interval_ns);
        }

        let send_start_tsc = rdtsc_now();
        requests += 1;

        let response = client.call_increment(
            &IncrementRequest { value: counter },
            None,
        )?;
        if response.status != STATUS_OK || response.value != counter + 1 {
            mismatches += 1;
        }

        counter = response.value;
        responses += 1;

        let tsc_delta = rdtsc_now() - send_start_tsc;
        // Discard insane deltas caused by cross-core TSC offset differences
        if tsc_delta < max_sane_tsc_delta {
            lat_ns.push(tsc_delta_ns(tsc_delta));
        }
    }

    let elapsed_sec = start.elapsed().as_secs_f64().max(1e-9);
    let cpu_cores = (self_cpu_seconds() - cpu_start) / elapsed_sec;
    let throughput = responses as f64 / elapsed_sec;

    lat_ns.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let p50 = percentile_micros(&lat_ns, 50.0);
    let p95 = percentile_micros(&lat_ns, 95.0);
    let p99 = percentile_micros(&lat_ns, 99.0);

    println!(
        "mode,duration_sec,target_rps,requests,responses,mismatches,throughput_rps,p50_us,p95_us,p99_us,client_cpu_cores"
    );
    let mode = if profile == PROFILE_SHM_HYBRID {
        "rust-shm-hybrid"
    } else {
        "rust-named-pipe"
    };
    println!(
        "{},{},{},{},{},{},{:.2},{:.2},{:.2},{:.2},{:.3}",
        mode, duration_sec, target_rps, requests, responses, mismatches,
        throughput, p50, p95, p99, cpu_cores
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
            if args.len() != 4 { usage(&args[0]); process::exit(2); }
            server_once(&args[2], &args[3])
        }
        "client-once" => {
            if args.len() != 5 { usage(&args[0]); process::exit(2); }
            client_once(&args[2], &args[3], parse_u64(&args[4]))
        }
        "server-loop" => {
            if args.len() != 5 { usage(&args[0]); process::exit(2); }
            server_loop(&args[2], &args[3], parse_u64(&args[4]))
        }
        "client-bench" => {
            if args.len() != 6 { usage(&args[0]); process::exit(2); }
            client_bench(&args[2], &args[3], parse_i32(&args[4]), parse_i32(&args[5]))
        }
        _ => {
            usage(&args[0]);
            process::exit(2);
        }
    };

    if let Err(err) = result {
        eprintln!("netipc_live_win_rs failed: {err}");
        process::exit(1);
    }
}
