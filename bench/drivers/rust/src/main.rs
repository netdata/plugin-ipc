//! bench_posix - POSIX benchmark driver for netipc (Rust).
//!
//! Exercises the public L1/L2/L3 API surface. Measures throughput,
//! latency (p50/p95/p99), and CPU.
//!
//! Same subcommands and output format as the C driver.

use netipc::protocol::{
    self, CgroupsBuilder, CgroupsRequest, Header,
    HEADER_SIZE, KIND_REQUEST, MAGIC_MSG, METHOD_CGROUPS_SNAPSHOT,
    METHOD_INCREMENT, PROFILE_BASELINE, PROFILE_SHM_HYBRID, STATUS_OK, VERSION,
};
use netipc::service::cgroups::{
    CgroupsCacheItem, CgroupsClient, ManagedServer,
};
use netipc::transport::posix::{ClientConfig, ServerConfig, UdsSession};

#[cfg(target_os = "linux")]
use netipc::transport::shm::ShmContext;

use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

const AUTH_TOKEN: u64 = 0xBE4C400000C0FFEE;
const RESPONSE_BUF_SIZE: usize = 65536;
const MAX_LATENCY_SAMPLES: usize = 10_000_000;

const PROFILE_UDS: u32 = PROFILE_BASELINE;
const PROFILE_SHM: u32 = PROFILE_BASELINE | PROFILE_SHM_HYBRID;

// ---------------------------------------------------------------------------
//  Timing helpers
// ---------------------------------------------------------------------------

fn cpu_ns() -> u64 {
    let mut ts = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    unsafe {
        libc::clock_gettime(libc::CLOCK_PROCESS_CPUTIME_ID, &mut ts);
    }
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

// ---------------------------------------------------------------------------
//  Latency recorder
// ---------------------------------------------------------------------------

struct LatencyRecorder {
    samples: Vec<u64>, // nanoseconds
}

impl LatencyRecorder {
    fn new(cap: usize) -> Self {
        let cap = cap.min(MAX_LATENCY_SAMPLES);
        LatencyRecorder {
            samples: Vec::with_capacity(cap),
        }
    }

    fn record(&mut self, ns: u64) {
        if self.samples.len() < self.samples.capacity() {
            self.samples.push(ns);
        }
    }

    fn percentile(&mut self, pct: f64) -> u64 {
        if self.samples.is_empty() {
            return 0;
        }
        self.samples.sort_unstable();
        let idx = ((pct / 100.0) * (self.samples.len() - 1) as f64) as usize;
        let idx = idx.min(self.samples.len() - 1);
        self.samples[idx]
    }
}

// ---------------------------------------------------------------------------
//  Rate limiter (adaptive sleep, no busy-wait)
// ---------------------------------------------------------------------------

struct RateLimiter {
    interval: Option<Duration>,
    next: Instant,
}

impl RateLimiter {
    fn new(target_rps: u64) -> Self {
        RateLimiter {
            interval: if target_rps == 0 {
                None
            } else {
                Some(Duration::from_nanos(1_000_000_000 / target_rps))
            },
            next: Instant::now(),
        }
    }

    fn wait(&mut self) {
        if let Some(interval) = self.interval {
            let now = Instant::now();
            if now < self.next {
                std::thread::sleep(self.next - now);
            }
            self.next += interval;
        }
    }
}

// ---------------------------------------------------------------------------
//  Config helpers
// ---------------------------------------------------------------------------

fn server_config(profiles: u32) -> ServerConfig {
    ServerConfig {
        supported_profiles: profiles,
        preferred_profiles: profiles,
        max_request_payload_bytes: 4096,
        max_request_batch_items: 1,
        max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
        max_response_batch_items: 1,
        auth_token: AUTH_TOKEN,
        backlog: 4,
        ..ServerConfig::default()
    }
}

fn client_config(profiles: u32) -> ClientConfig {
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

// ---------------------------------------------------------------------------
//  Ping-pong handler (INCREMENT method)
// ---------------------------------------------------------------------------

fn ping_pong_handler(method_code: u16, payload: &[u8]) -> Option<Vec<u8>> {
    if method_code != METHOD_INCREMENT {
        return None;
    }
    if payload.len() < 8 {
        return None;
    }

    let mut counter = u64::from_ne_bytes(payload[..8].try_into().ok()?);
    counter += 1;
    Some(counter.to_ne_bytes().to_vec())
}

// ---------------------------------------------------------------------------
//  Snapshot handler (16 cgroup items)
// ---------------------------------------------------------------------------

fn snapshot_handler(method_code: u16, payload: &[u8]) -> Option<Vec<u8>> {
    if method_code != METHOD_CGROUPS_SNAPSHOT {
        return None;
    }
    CgroupsRequest::decode(payload).ok()?;

    // Thread-local generation counter
    thread_local! {
        static GEN: std::cell::Cell<u64> = const { std::cell::Cell::new(0) };
    }
    let gen = GEN.with(|g| {
        let v = g.get() + 1;
        g.set(v);
        v
    });

    let mut buf = vec![0u8; RESPONSE_BUF_SIZE];
    let mut builder = CgroupsBuilder::new(&mut buf, 16, 1, gen);

    for i in 0..16u32 {
        let name = format!("cgroup-{}", i);
        let path = format!("/sys/fs/cgroup/bench/cg-{}", i);
        if builder
            .add(1000 + i, 0, i % 2, name.as_bytes(), path.as_bytes())
            .is_err()
        {
            return None;
        }
    }

    let total = builder.finish();
    buf.truncate(total);
    Some(buf)
}

// ---------------------------------------------------------------------------
//  Server
// ---------------------------------------------------------------------------

fn run_server(
    run_dir: &str,
    service: &str,
    profiles: u32,
    duration_sec: u32,
    handler_type: &str,
) -> i32 {
    let handler: std::sync::Arc<dyn Fn(u16, &[u8]) -> Option<Vec<u8>> + Send + Sync> = match handler_type {
        "ping-pong" => std::sync::Arc::new(|code, payload| ping_pong_handler(code, payload)),
        "snapshot" => std::sync::Arc::new(|code, payload| snapshot_handler(code, payload)),
        _ => {
            eprintln!("Unknown handler type: {handler_type}");
            return 1;
        }
    };

    let mut server = ManagedServer::new(
        run_dir,
        service,
        server_config(profiles),
        RESPONSE_BUF_SIZE,
        handler,
    );

    println!("READY");

    let cpu_start = cpu_ns();

    let stop_flag = server.running_flag();
    let dur = duration_sec;
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_secs(dur as u64 + 3));
        stop_flag.store(false, Ordering::Release);
    });

    let _ = server.run();

    let cpu_end = cpu_ns();
    let cpu_sec = (cpu_end - cpu_start) as f64 / 1e9;

    println!("SERVER_CPU_SEC={:.6}", cpu_sec);

    0
}

// ---------------------------------------------------------------------------
//  Ping-pong client
// ---------------------------------------------------------------------------

fn run_ping_pong_client(
    run_dir: &str,
    service: &str,
    profiles: u32,
    duration_sec: u32,
    target_rps: u64,
    scenario: &str,
    server_lang: &str,
) -> i32 {
    // Direct L1 connection with retry (no CgroupsClient pre-connect)
    let mut session = None;
    for _ in 0..200 {
        match UdsSession::connect(run_dir, service, &client_config(profiles)) {
            Ok(s) => {
                session = Some(s);
                break;
            }
            Err(_) => std::thread::sleep(Duration::from_millis(10)),
        }
    }
    let mut session = match session {
        Some(s) => s,
        None => {
            eprintln!("client: connect failed after retries");
            return 1;
        }
    };

    let est_samples = if target_rps == 0 {
        5_000_000
    } else {
        (target_rps * duration_sec as u64) as usize
    };
    let mut lr = LatencyRecorder::new(est_samples);
    let mut rl = RateLimiter::new(target_rps);

    let mut counter: u64 = 0;
    let mut requests: u64 = 0;
    let mut errors: u64 = 0;

    let cpu_start = cpu_ns();
    let wall_start = Instant::now();
    let deadline = Duration::from_secs(duration_sec as u64);

    // SHM upgrade if negotiated
    #[cfg(target_os = "linux")]
    let mut shm: Option<ShmContext> = {
        let sp = session.selected_profile;
        if sp == PROFILE_SHM_HYBRID || sp == protocol::PROFILE_SHM_FUTEX {
            let mut shm_ctx = None;
            for _ in 0..200 {
                match ShmContext::client_attach(run_dir, service, session.session_id) {
                    Ok(ctx) => {
                        shm_ctx = Some(ctx);
                        break;
                    }
                    Err(_) => std::thread::sleep(Duration::from_millis(5)),
                }
            }
            shm_ctx
        } else {
            None
        }
    };

    #[cfg(not(target_os = "linux"))]
    let shm: Option<()> = None;

    while wall_start.elapsed() < deadline {
        rl.wait();

        let req_payload = counter.to_ne_bytes();

        let mut hdr = Header {
            kind: KIND_REQUEST,
            code: METHOD_INCREMENT,
            flags: 0,
            item_count: 1,
            message_id: counter + 1,
            transport_status: STATUS_OK,
            ..Header::default()
        };

        let t0 = Instant::now();

        #[cfg(target_os = "linux")]
        let send_ok = if let Some(ref mut shm_ctx) = shm {
            // SHM path
            let msg_len = HEADER_SIZE + 8;
            let mut msg = vec![0u8; msg_len];
            hdr.magic = MAGIC_MSG;
            hdr.version = VERSION;
            hdr.header_len = protocol::HEADER_LEN;
            hdr.payload_len = 8;
            hdr.encode(&mut msg[..HEADER_SIZE]);
            msg[HEADER_SIZE..].copy_from_slice(&req_payload);

            if shm_ctx.send(&msg).is_err() {
                errors += 1;
                continue;
            }

            let mut resp_shm_buf = vec![0u8; HEADER_SIZE + 64];
            match shm_ctx.receive(&mut resp_shm_buf, 30000) {
                Ok(resp_len) => {
                    if resp_len >= HEADER_SIZE + 8 {
                        let resp_val =
                            u64::from_ne_bytes(resp_shm_buf[HEADER_SIZE..HEADER_SIZE + 8].try_into().unwrap());
                        if resp_val != counter + 1 {
                            eprintln!(
                                "counter chain broken: expected {}, got {}",
                                counter + 1,
                                resp_val
                            );
                            errors += 1;
                        }
                    }
                    true
                }
                Err(_) => {
                    errors += 1;
                    false
                }
            }
        } else {
            // UDS path
            if session.send(&mut hdr, &req_payload).is_err() {
                errors += 1;
                continue;
            }
            let mut recv_buf = vec![0u8; 256];
            match session.receive(&mut recv_buf) {
                Ok((_resp_hdr, payload)) => {
                    if payload.len() >= 8 {
                        let resp_val = u64::from_ne_bytes(payload[..8].try_into().unwrap());
                        if resp_val != counter + 1 {
                            eprintln!(
                                "counter chain broken: expected {}, got {}",
                                counter + 1,
                                resp_val
                            );
                            errors += 1;
                        }
                    }
                    true
                }
                Err(_) => {
                    errors += 1;
                    false
                }
            }
        };

        #[cfg(not(target_os = "linux"))]
        let send_ok = {
            if session.send(&mut hdr, &req_payload).is_err() {
                errors += 1;
                continue;
            }
            let mut recv_buf = vec![0u8; 256];
            match session.receive(&mut recv_buf) {
                Ok((_resp_hdr, payload)) => {
                    if payload.len() >= 8 {
                        let resp_val = u64::from_ne_bytes(payload[..8].try_into().unwrap());
                        if resp_val != counter + 1 {
                            eprintln!(
                                "counter chain broken: expected {}, got {}",
                                counter + 1,
                                resp_val
                            );
                            errors += 1;
                        }
                    }
                    true
                }
                Err(_) => {
                    errors += 1;
                    false
                }
            }
        };

        let t1 = Instant::now();
        if send_ok {
            lr.record((t1 - t0).as_nanos() as u64);
        }

        counter += 1;
        requests += 1;
    }

    let wall_sec = wall_start.elapsed().as_secs_f64();
    let cpu_end = cpu_ns();
    let cpu_sec = (cpu_end - cpu_start) as f64 / 1e9;
    let throughput = requests as f64 / wall_sec;
    let cpu_pct = (cpu_sec / wall_sec) * 100.0;

    let p50 = lr.percentile(50.0) / 1000;
    let p95 = lr.percentile(95.0) / 1000;
    let p99 = lr.percentile(99.0) / 1000;

    println!(
        "{},rust,{},{:.0},{},{},{},{:.1},0.0,{:.1}",
        scenario, server_lang, throughput, p50, p95, p99, cpu_pct, cpu_pct
    );

    // Cleanup
    #[cfg(target_os = "linux")]
    {
        if let Some(mut shm_ctx) = shm {
            shm_ctx.close();
        }
    }
    drop(session);

    if errors > 0 {
        eprintln!("client: {} errors", errors);
    }

    if errors > 0 { 1 } else { 0 }
}

// ---------------------------------------------------------------------------
//  Snapshot client (L2 typed call)
// ---------------------------------------------------------------------------

fn run_snapshot_client(
    run_dir: &str,
    service: &str,
    profiles: u32,
    duration_sec: u32,
    target_rps: u64,
    scenario: &str,
    server_lang: &str,
) -> i32 {
    let mut client = CgroupsClient::new(run_dir, service, client_config(profiles));

    for _ in 0..200 {
        client.refresh();
        if client.ready() {
            break;
        }
        std::thread::sleep(Duration::from_millis(10));
    }

    if !client.ready() {
        eprintln!("client: not ready after retries");
        return 1;
    }

    let est_samples = if target_rps == 0 {
        5_000_000
    } else {
        (target_rps * duration_sec as u64) as usize
    };
    let mut lr = LatencyRecorder::new(est_samples);
    let mut rl = RateLimiter::new(target_rps);

    let mut requests: u64 = 0;
    let mut errors: u64 = 0;

    let cpu_start = cpu_ns();
    let wall_start = Instant::now();
    let deadline = Duration::from_secs(duration_sec as u64);

    let mut resp_buf = vec![0u8; RESPONSE_BUF_SIZE];

    while wall_start.elapsed() < deadline {
        rl.wait();

        let t0 = Instant::now();

        match client.call_snapshot(&mut resp_buf) {
            Ok(view) => {
                if view.item_count != 16 {
                    eprintln!("snapshot: expected 16 items, got {}", view.item_count);
                    errors += 1;
                }
                let t1 = Instant::now();
                lr.record((t1 - t0).as_nanos() as u64);
                requests += 1;
            }
            Err(_) => {
                errors += 1;
                client.refresh();
            }
        }
    }

    let wall_sec = wall_start.elapsed().as_secs_f64();
    let cpu_end = cpu_ns();
    let cpu_sec = (cpu_end - cpu_start) as f64 / 1e9;
    let throughput = requests as f64 / wall_sec;
    let cpu_pct = (cpu_sec / wall_sec) * 100.0;

    let p50 = lr.percentile(50.0) / 1000;
    let p95 = lr.percentile(95.0) / 1000;
    let p99 = lr.percentile(99.0) / 1000;

    println!(
        "{},rust,{},{:.0},{},{},{},{:.1},0.0,{:.1}",
        scenario, server_lang, throughput, p50, p95, p99, cpu_pct, cpu_pct
    );

    client.close();

    if errors > 0 {
        eprintln!("client: {} errors", errors);
    }

    if errors > 0 { 1 } else { 0 }
}

// ---------------------------------------------------------------------------
//  Lookup benchmark (L3 cache, no transport)
// ---------------------------------------------------------------------------

fn run_lookup_bench(duration_sec: u32) -> i32 {
    // Build a synthetic cache with 16 items
    let items: Vec<CgroupsCacheItem> = (0..16)
        .map(|i| CgroupsCacheItem {
            hash: 1000 + i,
            options: 0,
            enabled: i % 2,
            name: format!("cgroup-{}", i),
            path: format!("/sys/fs/cgroup/bench/cg-{}", i),
        })
        .collect();

    let mut lookups: u64 = 0;
    let mut hits: u64 = 0;

    let cpu_start = cpu_ns();
    let wall_start = Instant::now();
    let deadline = Duration::from_secs(duration_sec as u64);

    // Simulate L3 cache lookup without transport
    while wall_start.elapsed() < deadline {
        for item in &items {
            // Linear scan like the real L3 cache
            let found = items.iter().find(|it| it.hash == item.hash && it.name == item.name);
            if found.is_some() {
                hits += 1;
            }
            lookups += 1;
        }
    }

    let wall_sec = wall_start.elapsed().as_secs_f64();
    let cpu_end = cpu_ns();
    let cpu_sec = (cpu_end - cpu_start) as f64 / 1e9;
    let throughput = lookups as f64 / wall_sec;
    let cpu_pct = (cpu_sec / wall_sec) * 100.0;

    println!("lookup,rust,rust,{:.0},0,0,0,{:.1},0.0,{:.1}", throughput, cpu_pct, cpu_pct);

    if hits != lookups {
        eprintln!("lookup: missed {}/{}", lookups - hits, lookups);
        return 1;
    }

    0
}

// ---------------------------------------------------------------------------
//  Main
// ---------------------------------------------------------------------------

fn main() {
    // Ignore SIGPIPE
    unsafe {
        libc::signal(libc::SIGPIPE, libc::SIG_IGN);
    }

    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!(
            "Usage: {} <subcommand> [args...]\n\
             Subcommands: uds-ping-pong-{{server,client}}, shm-ping-pong-{{server,client}},\n\
             snapshot-{{server,client}}, snapshot-shm-{{server,client}}, lookup-bench",
            args[0]
        );
        std::process::exit(1);
    }

    let cmd = &args[1];

    let rc = match cmd.as_str() {
        "uds-ping-pong-server" | "shm-ping-pong-server"
        | "snapshot-server" | "snapshot-shm-server" => {
            if args.len() < 4 {
                eprintln!("Usage: {} {} <run_dir> <service> [duration_sec]", args[0], cmd);
                std::process::exit(1);
            }
            let run_dir = &args[2];
            let service = &args[3];
            let duration: u32 = if args.len() >= 5 {
                args[4].parse().unwrap_or(30)
            } else {
                30
            };

            let _ = std::fs::create_dir_all(run_dir);

            let (profiles, handler_type) = match cmd.as_str() {
                "uds-ping-pong-server" => (PROFILE_UDS, "ping-pong"),
                "shm-ping-pong-server" => (PROFILE_SHM, "ping-pong"),
                "snapshot-server" => (PROFILE_UDS, "snapshot"),
                "snapshot-shm-server" => (PROFILE_SHM, "snapshot"),
                _ => unreachable!(),
            };

            run_server(run_dir, service, profiles, duration, handler_type)
        }

        "uds-ping-pong-client" | "shm-ping-pong-client" => {
            if args.len() < 6 {
                eprintln!(
                    "Usage: {} {} <run_dir> <service> <duration_sec> <target_rps>",
                    args[0], cmd
                );
                std::process::exit(1);
            }
            let run_dir = &args[2];
            let service = &args[3];
            let duration: u32 = args[4].parse().unwrap_or(5);
            let target_rps: u64 = args[5].parse().unwrap_or(0);

            let (profiles, scenario) = match cmd.as_str() {
                "uds-ping-pong-client" => (PROFILE_UDS, "uds-ping-pong"),
                "shm-ping-pong-client" => (PROFILE_SHM, "shm-ping-pong"),
                _ => unreachable!(),
            };

            run_ping_pong_client(run_dir, service, profiles, duration, target_rps, scenario, "rust")
        }

        "snapshot-client" | "snapshot-shm-client" => {
            if args.len() < 6 {
                eprintln!(
                    "Usage: {} {} <run_dir> <service> <duration_sec> <target_rps>",
                    args[0], cmd
                );
                std::process::exit(1);
            }
            let run_dir = &args[2];
            let service = &args[3];
            let duration: u32 = args[4].parse().unwrap_or(5);
            let target_rps: u64 = args[5].parse().unwrap_or(0);

            let (profiles, scenario) = match cmd.as_str() {
                "snapshot-client" => (PROFILE_UDS, "snapshot-baseline"),
                "snapshot-shm-client" => (PROFILE_SHM, "snapshot-shm"),
                _ => unreachable!(),
            };

            run_snapshot_client(run_dir, service, profiles, duration, target_rps, scenario, "rust")
        }

        "lookup-bench" => {
            if args.len() < 3 {
                eprintln!("Usage: {} lookup-bench <duration_sec>", args[0]);
                std::process::exit(1);
            }
            let duration: u32 = args[2].parse().unwrap_or(5);
            run_lookup_bench(duration)
        }

        _ => {
            eprintln!("Unknown subcommand: {cmd}");
            std::process::exit(1);
        }
    };

    std::process::exit(rc);
}
