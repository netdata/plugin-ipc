use netipc::transport::posix::{ShmClient, ShmConfig, ShmServer};
use netipc::{IncrementRequest, IncrementResponse, STATUS_OK};
use std::env;
use std::io;
use std::process;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

static BENCH_STOP_REQUESTED: AtomicBool = AtomicBool::new(false);

struct BenchResult {
    duration_sec: i32,
    target_rps: i32,
    requests: i64,
    responses: i64,
    mismatches: i64,
    throughput_rps: f64,
    p50_us: f64,
    p95_us: f64,
    p99_us: f64,
    client_cpu_cores: f64,
}

fn usage(argv0: &str) {
    eprintln!("usage:");
    eprintln!("  {argv0} server-once <run_dir> <service>");
    eprintln!("  {argv0} client-once <run_dir> <service> <value>");
    eprintln!("  {argv0} server-loop <run_dir> <service> <max_requests|0>");
    eprintln!("  {argv0} server-bench <run_dir> <service> <max_requests|0>");
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

fn adaptive_sleep_ns(remaining_ns: u64) -> u64 {
    if remaining_ns > 5_000_000 {
        return remaining_ns - 1_000_000;
    }
    if remaining_ns > 500_000 {
        return remaining_ns / 2;
    }
    if remaining_ns > 50_000 {
        return remaining_ns / 4;
    }
    remaining_ns
}

fn wait_for_benchmark_slot(
    start: Instant,
    end_at: Instant,
    target_rps: i32,
    requests_sent: u64,
) -> bool {
    if target_rps <= 0 {
        return Instant::now() < end_at;
    }

    let rate = target_rps as u64;
    loop {
        let now = Instant::now();
        if now >= end_at {
            return false;
        }

        let elapsed_ns = start.elapsed().as_nanos() as u64;
        let target_completed = elapsed_ns.saturating_mul(rate) / 1_000_000_000;
        if requests_sent <= target_completed {
            return true;
        }

        let target_elapsed_ns = requests_sent.saturating_mul(1_000_000_000) / rate;
        if target_elapsed_ns <= elapsed_ns {
            return true;
        }

        std::thread::sleep(Duration::from_nanos(adaptive_sleep_ns(
            target_elapsed_ns - elapsed_ns,
        )));
    }
}

extern "C" fn handle_bench_stop_signal(_: libc::c_int) {
    BENCH_STOP_REQUESTED.store(true, Ordering::Relaxed);
}

fn install_benchmark_stop_handlers() -> io::Result<()> {
    BENCH_STOP_REQUESTED.store(false, Ordering::Relaxed);

    unsafe {
        if libc::signal(libc::SIGTERM, handle_bench_stop_signal as libc::sighandler_t)
            == libc::SIG_ERR
        {
            return Err(io::Error::last_os_error());
        }
        if libc::signal(libc::SIGINT, handle_bench_stop_signal as libc::sighandler_t)
            == libc::SIG_ERR
        {
            return Err(io::Error::last_os_error());
        }
    }

    Ok(())
}

fn server_once(run_dir: &str, service: &str) -> io::Result<()> {
    let mut server = ShmServer::create(&ShmConfig::new(run_dir, service))?;
    let (request_id, request) = server.receive_increment(Some(Duration::from_secs(10)))?;
    let response = IncrementResponse {
        status: STATUS_OK,
        value: request.value + 1,
    };
    server.send_increment(request_id, &response)?;

    println!(
        "RUST-SHM-SERVER request_id={request_id} value={} response={}",
        request.value, response.value
    );
    Ok(())
}

fn client_once(run_dir: &str, service: &str, value: u64) -> io::Result<()> {
    let mut client = ShmClient::connect(&ShmConfig::new(run_dir, service))?;
    let response =
        client.call_increment(&IncrementRequest { value }, Some(Duration::from_secs(10)))?;

    if response.status != STATUS_OK {
        return Err(protocol_error("server returned non-OK status"));
    }
    if response.value != value + 1 {
        return Err(protocol_error("unexpected response value"));
    }

    println!("RUST-SHM-CLIENT request={value} response={}", response.value);
    Ok(())
}

fn server_loop(run_dir: &str, service: &str, max_requests: u64) -> io::Result<()> {
    let _ = server_loop_internal(run_dir, service, max_requests, Duration::from_secs(10), false)?;
    Ok(())
}

fn server_loop_internal(
    run_dir: &str,
    service: &str,
    max_requests: u64,
    timeout: Duration,
    allow_signal_stop: bool,
) -> io::Result<u64> {
    let mut server = ShmServer::create(&ShmConfig::new(run_dir, service))?;
    let mut handled = 0u64;

    while max_requests == 0 || handled < max_requests {
        if allow_signal_stop && BENCH_STOP_REQUESTED.load(Ordering::Relaxed) {
            break;
        }

        let (request_id, request) = match server.receive_increment(Some(timeout)) {
            Ok(value) => value,
            Err(err) if err.kind() == io::ErrorKind::TimedOut => {
                if allow_signal_stop && BENCH_STOP_REQUESTED.load(Ordering::Relaxed) {
                    break;
                }
                continue;
            }
            Err(err) => return Err(err),
        };

        let response = IncrementResponse {
            status: STATUS_OK,
            value: request.value + 1,
        };
        server.send_increment(request_id, &response)?;
        handled += 1;
    }

    Ok(handled)
}

fn client_bench_capture(
    run_dir: &str,
    service: &str,
    duration_sec: i32,
    target_rps: i32,
) -> io::Result<BenchResult> {
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

    let mut client = ShmClient::connect(&ShmConfig::new(run_dir, service))?;

    let start = Instant::now();
    let end_at = start + Duration::from_secs(duration_sec as u64);
    let cpu_start = self_cpu_seconds();

    let mut lat_ns: Vec<i64> = Vec::with_capacity(1 << 20);
    let mut counter = 1u64;
    let mut requests = 0i64;
    let mut responses = 0i64;
    let mismatches = 0i64;

    while wait_for_benchmark_slot(start, end_at, target_rps, requests as u64) {
        if Instant::now() >= end_at {
            break;
        }

        let send_start = Instant::now();
        requests += 1;

        let response = client.call_increment(
            &IncrementRequest { value: counter },
            Some(Duration::from_secs(10)),
        )?;
        if response.status != STATUS_OK {
            return Err(protocol_error("server returned non-OK status during benchmark"));
        }
        if response.value != counter + 1 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "benchmark counter mismatch: got={} expected={}",
                    response.value,
                    counter + 1
                ),
            ));
        }

        counter = response.value;
        responses += 1;
        lat_ns.push(send_start.elapsed().as_nanos() as i64);
    }

    let elapsed_sec = start.elapsed().as_secs_f64().max(1e-9);
    let cpu_cores = (self_cpu_seconds() - cpu_start) / elapsed_sec;
    let throughput = responses as f64 / elapsed_sec;

    if responses != requests {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "benchmark request/response mismatch: requests={} responses={}",
                requests, responses
            ),
        ));
    }
    if counter != responses as u64 + 1 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "benchmark final counter mismatch: counter={} expected={}",
                counter,
                responses as u64 + 1
            ),
        ));
    }

    lat_ns.sort_unstable();
    let p50 = percentile_micros(&lat_ns, 50.0);
    let p95 = percentile_micros(&lat_ns, 95.0);
    let p99 = percentile_micros(&lat_ns, 99.0);

    Ok(BenchResult {
        duration_sec,
        target_rps,
        requests,
        responses,
        mismatches,
        throughput_rps: throughput,
        p50_us: p50,
        p95_us: p95,
        p99_us: p99,
        client_cpu_cores: cpu_cores,
    })
}

fn print_bench_header() {
    println!(
        "mode,duration_sec,target_rps,requests,responses,mismatches,throughput_rps,p50_us,p95_us,p99_us,client_cpu_cores,server_cpu_cores,total_cpu_cores"
    );
}

fn print_bench_row(mode: &str, result: &BenchResult, server_cpu_cores: f64) {
    println!(
        "{},{},{},{},{},{},{:.2},{:.2},{:.2},{:.2},{:.3},{:.3},{:.3}",
        mode,
        result.duration_sec,
        result.target_rps,
        result.requests,
        result.responses,
        result.mismatches,
        result.throughput_rps,
        result.p50_us,
        result.p95_us,
        result.p99_us,
        result.client_cpu_cores,
        server_cpu_cores,
        result.client_cpu_cores + server_cpu_cores
    );
}

fn print_server_bench_row(
    mode: &str,
    handled_requests: u64,
    elapsed_sec: f64,
    server_cpu_cores: f64,
) {
    println!(
        "{}-server,{},{:.6},{:.3}",
        mode, handled_requests, elapsed_sec, server_cpu_cores
    );
}

fn client_bench(
    run_dir: &str,
    service: &str,
    duration_sec: i32,
    target_rps: i32,
) -> io::Result<()> {
    let result = client_bench_capture(run_dir, service, duration_sec, target_rps)?;
    print_bench_header();
    print_bench_row("rust-shm-hybrid", &result, 0.0);
    Ok(())
}

fn server_bench(run_dir: &str, service: &str, max_requests: u64) -> io::Result<()> {
    install_benchmark_stop_handlers()?;
    let start = Instant::now();
    let cpu_start = self_cpu_seconds();
    let handled_requests =
        server_loop_internal(run_dir, service, max_requests, Duration::from_millis(100), true)?;
    let elapsed_sec = start.elapsed().as_secs_f64().max(1e-9);
    let server_cpu_cores = (self_cpu_seconds() - cpu_start) / elapsed_sec;
    print_server_bench_row("rust-shm-hybrid", handled_requests, elapsed_sec, server_cpu_cores);
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
        "server-bench" => {
            if args.len() != 5 {
                usage(&args[0]);
                process::exit(2);
            }
            server_bench(&args[2], &args[3], parse_u64(&args[4]))
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
        eprintln!("netipc_live_rs failed: {err}");
        process::exit(1);
    }
}
