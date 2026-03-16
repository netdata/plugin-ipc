//! bench_windows - Windows benchmark driver for netipc (Rust).
//!
//! Exercises Named Pipe and Win SHM transports. Measures throughput,
//! latency (p50/p95/p99), and CPU.
//!
//! Same subcommands and output format as the C/Go Windows drivers,
//! plus batch and pipeline scenarios from the POSIX Rust driver.

// Non-Windows stub: print error and exit.
#[cfg(not(windows))]
fn main() {
    eprintln!("Windows benchmark driver: not supported on this platform");
    std::process::exit(1);
}

// ---------------------------------------------------------------------------
//  Windows implementation
// ---------------------------------------------------------------------------

#[cfg(windows)]
use netipc::protocol::{
    self, BatchBuilder, CgroupsBuilder, CgroupsRequest, Header,
    HEADER_SIZE, KIND_REQUEST, KIND_RESPONSE, FLAG_BATCH, MAGIC_MSG,
    METHOD_CGROUPS_SNAPSHOT, METHOD_INCREMENT, PROFILE_BASELINE,
    STATUS_OK, VERSION, INCREMENT_PAYLOAD_SIZE,
    batch_item_get, increment_decode, increment_encode,
};
#[cfg(windows)]
use netipc::service::cgroups::{
    CgroupsCacheItem, CgroupsClient, ManagedServer,
};
#[cfg(windows)]
use netipc::transport::windows::{ClientConfig, ServerConfig, NpSession};
#[cfg(windows)]
use netipc::transport::win_shm::{
    WinShmContext, PROFILE_HYBRID as WIN_SHM_PROFILE_HYBRID,
};

#[cfg(windows)]
use std::sync::atomic::Ordering;
#[cfg(windows)]
use std::time::{Duration, Instant};

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

#[cfg(windows)]
const AUTH_TOKEN: u64 = 0xBE4C400000C0FFEE;
#[cfg(windows)]
const RESPONSE_BUF_SIZE: usize = 65536;
#[cfg(windows)]
const MAX_LATENCY_SAMPLES: usize = 10_000_000;
#[cfg(windows)]
const DEFAULT_DURATION: u32 = 30;

#[cfg(windows)]
const PROFILE_NP: u32 = PROFILE_BASELINE;
#[cfg(windows)]
const PROFILE_SHM: u32 = PROFILE_BASELINE | WIN_SHM_PROFILE_HYBRID;

// Batch benchmark constants (mirror C driver)
#[cfg(windows)]
const BENCH_MAX_BATCH_ITEMS: u32 = 1000;
#[cfg(windows)]
const BENCH_BATCH_BUF_SIZE: usize = BENCH_MAX_BATCH_ITEMS as usize * 48 + 4096;

// ---------------------------------------------------------------------------
//  Win32 FFI for timing
// ---------------------------------------------------------------------------

#[cfg(windows)]
mod ffi {
    #![allow(non_snake_case)]

    pub type HANDLE = isize;

    extern "system" {
        pub fn GetProcessTimes(
            hProcess: HANDLE,
            lpCreationTime: *mut u64,
            lpExitTime: *mut u64,
            lpKernelTime: *mut u64,
            lpUserTime: *mut u64,
        ) -> i32;
        pub fn GetCurrentProcess() -> HANDLE;
        pub fn GetTickCount64() -> u64;
    }
}

/// Cheap deadline check using GetTickCount64 (~1ms resolution).
/// Avoids QPC overhead in hot loops.
#[cfg(windows)]
struct TickDeadline {
    deadline: u64,
}

#[cfg(windows)]
impl TickDeadline {
    fn new(duration_sec: u32) -> Self {
        let now = unsafe { ffi::GetTickCount64() };
        Self { deadline: now + duration_sec as u64 * 1000 }
    }
    #[inline]
    fn expired(&self) -> bool {
        unsafe { ffi::GetTickCount64() >= self.deadline }
    }
}

#[cfg(windows)]
fn cpu_ns() -> u64 {
    let mut creation: u64 = 0;
    let mut exit: u64 = 0;
    let mut kernel: u64 = 0;
    let mut user: u64 = 0;
    unsafe {
        ffi::GetProcessTimes(
            ffi::GetCurrentProcess(),
            &mut creation,
            &mut exit,
            &mut kernel,
            &mut user,
        );
    }
    // FILETIME is in 100ns intervals
    (kernel + user) * 100
}

// ---------------------------------------------------------------------------
//  Latency recorder
// ---------------------------------------------------------------------------

#[cfg(windows)]
struct LatencyRecorder {
    samples: Vec<u64>,
}

#[cfg(windows)]
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
//  Rate limiter
// ---------------------------------------------------------------------------

#[cfg(windows)]
struct RateLimiter {
    interval: Option<Duration>,
    next: Instant,
}

#[cfg(windows)]
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
//  XorShift32 PRNG (for batch size randomization)
// ---------------------------------------------------------------------------

#[cfg(windows)]
struct XorShift32 {
    state: u32,
}

#[cfg(windows)]
impl XorShift32 {
    fn new() -> Self {
        XorShift32 { state: 12345 }
    }
    fn next(&mut self) -> u32 {
        self.state ^= self.state << 13;
        self.state ^= self.state >> 17;
        self.state ^= self.state << 5;
        self.state
    }
}

// ---------------------------------------------------------------------------
//  Config helpers
// ---------------------------------------------------------------------------

#[cfg(windows)]
fn server_config(profiles: u32) -> ServerConfig {
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

#[cfg(windows)]
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

#[cfg(windows)]
fn batch_server_config(profiles: u32) -> ServerConfig {
    ServerConfig {
        supported_profiles: profiles,
        preferred_profiles: profiles,
        max_request_payload_bytes: BENCH_BATCH_BUF_SIZE as u32,
        max_request_batch_items: BENCH_MAX_BATCH_ITEMS,
        max_response_payload_bytes: BENCH_BATCH_BUF_SIZE as u32,
        max_response_batch_items: BENCH_MAX_BATCH_ITEMS,
        auth_token: AUTH_TOKEN,
        ..ServerConfig::default()
    }
}

#[cfg(windows)]
fn batch_client_config(profiles: u32) -> ClientConfig {
    ClientConfig {
        supported_profiles: profiles,
        preferred_profiles: profiles,
        max_request_payload_bytes: BENCH_BATCH_BUF_SIZE as u32,
        max_request_batch_items: BENCH_MAX_BATCH_ITEMS,
        max_response_payload_bytes: BENCH_BATCH_BUF_SIZE as u32,
        max_response_batch_items: BENCH_MAX_BATCH_ITEMS,
        auth_token: AUTH_TOKEN,
        ..ClientConfig::default()
    }
}

// ---------------------------------------------------------------------------
//  Ping-pong handler (INCREMENT method)
// ---------------------------------------------------------------------------

#[cfg(windows)]
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

#[cfg(windows)]
fn snapshot_handler(method_code: u16, payload: &[u8]) -> Option<Vec<u8>> {
    if method_code != METHOD_CGROUPS_SNAPSHOT {
        return None;
    }
    CgroupsRequest::decode(payload).ok()?;

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
//  Server (ping-pong or snapshot)
// ---------------------------------------------------------------------------

#[cfg(windows)]
fn run_server(
    run_dir: &str,
    service: &str,
    profiles: u32,
    duration_sec: u32,
    handler_type: &str,
) -> i32 {
    let handler: std::sync::Arc<dyn Fn(u16, &[u8]) -> Option<Vec<u8>> + Send + Sync> =
        match handler_type {
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
//  Batch server (same handler, higher batch/payload limits)
// ---------------------------------------------------------------------------

#[cfg(windows)]
fn run_batch_server(
    run_dir: &str,
    service: &str,
    profiles: u32,
    duration_sec: u32,
) -> i32 {
    let handler: std::sync::Arc<dyn Fn(u16, &[u8]) -> Option<Vec<u8>> + Send + Sync> =
        std::sync::Arc::new(|code, payload| ping_pong_handler(code, payload));

    let resp_buf_size = BENCH_BATCH_BUF_SIZE * 2;

    let mut server = ManagedServer::new(
        run_dir,
        service,
        batch_server_config(profiles),
        resp_buf_size,
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
//  SHM upgrade helper (shared by client functions)
// ---------------------------------------------------------------------------

#[cfg(windows)]
fn try_shm_upgrade(
    run_dir: &str,
    service: &str,
    session: &NpSession,
) -> Option<WinShmContext> {
    if session.selected_profile & WIN_SHM_PROFILE_HYBRID == 0 {
        return None;
    }
    for _ in 0..200 {
        match WinShmContext::client_attach(
            run_dir,
            service,
            AUTH_TOKEN,
            session.session_id,
            WIN_SHM_PROFILE_HYBRID,
        ) {
            Ok(ctx) => return Some(ctx),
            Err(_) => std::thread::sleep(Duration::from_millis(5)),
        }
    }
    None
}

// ---------------------------------------------------------------------------
//  Ping-pong client (Named Pipe or SHM)
// ---------------------------------------------------------------------------

#[cfg(windows)]
fn run_ping_pong_client(
    run_dir: &str,
    service: &str,
    profiles: u32,
    duration_sec: u32,
    target_rps: u64,
    scenario: &str,
    server_lang: &str,
) -> i32 {
    let mut session = None;
    for _ in 0..200 {
        match NpSession::connect(run_dir, service, &client_config(profiles)) {
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

    let mut shm = try_shm_upgrade(run_dir, service, &session);

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
    let tick_deadline = TickDeadline::new(duration_sec);

    while !tick_deadline.expired() {
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

        let t0 = if requests & 63 == 0 { Some(Instant::now()) } else { None };

        let send_ok = if let Some(ref mut shm_ctx) = shm {
            // Win SHM path
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

            let mut resp_buf = vec![0u8; HEADER_SIZE + 64];
            match shm_ctx.receive(&mut resp_buf, 30000) {
                Ok(resp_len) => {
                    if resp_len >= HEADER_SIZE + 8 {
                        let resp_val = u64::from_ne_bytes(
                            resp_buf[HEADER_SIZE..HEADER_SIZE + 8].try_into().unwrap(),
                        );
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
            // Named Pipe path
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

        if let Some(t0v) = t0 {
            if send_ok {
                lr.record(t0v.elapsed().as_nanos() as u64);
            }
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

    if let Some(mut shm_ctx) = shm {
        shm_ctx.close();
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

#[cfg(windows)]
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
    let tick_deadline = TickDeadline::new(duration_sec);

    let mut resp_buf = vec![0u8; RESPONSE_BUF_SIZE];

    while !tick_deadline.expired() {
        rl.wait();

        let t0 = if requests & 63 == 0 { Some(Instant::now()) } else { None };

        match client.call_snapshot(&mut resp_buf) {
            Ok(view) => {
                if view.item_count != 16 {
                    eprintln!("snapshot: expected 16 items, got {}", view.item_count);
                    errors += 1;
                }
                if let Some(t0v) = t0 {
                    lr.record(t0v.elapsed().as_nanos() as u64);
                }
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
//  Batch ping-pong client (random 1-1000 items per batch)
// ---------------------------------------------------------------------------

#[cfg(windows)]
fn run_batch_ping_pong_client(
    run_dir: &str,
    service: &str,
    profiles: u32,
    duration_sec: u32,
    target_rps: u64,
    scenario: &str,
    server_lang: &str,
) -> i32 {
    let mut session = None;
    for _ in 0..200 {
        match NpSession::connect(run_dir, service, &batch_client_config(profiles)) {
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
            eprintln!("batch client: connect failed after retries");
            return 1;
        }
    };

    let est_samples = if target_rps == 0 {
        2_000_000
    } else {
        (target_rps * duration_sec as u64) as usize
    };
    let mut lr = LatencyRecorder::new(est_samples);
    let mut rl = RateLimiter::new(target_rps);
    let mut rng = XorShift32::new();

    let mut counter: u64 = 0;
    let mut total_items: u64 = 0;
    let mut errors: u64 = 0;

    let mut req_buf = vec![0u8; BENCH_BATCH_BUF_SIZE];
    let mut recv_buf = vec![0u8; BENCH_BATCH_BUF_SIZE + HEADER_SIZE];
    let mut expected = vec![0u64; BENCH_MAX_BATCH_ITEMS as usize];

    // SHM upgrade if negotiated
    let mut shm = try_shm_upgrade(run_dir, service, &session);

    let cpu_start = cpu_ns();
    let wall_start = Instant::now();
    let tick_deadline = TickDeadline::new(duration_sec);

    while !tick_deadline.expired() {
        rl.wait();

        let batch_size = (rng.next() % BENCH_MAX_BATCH_ITEMS) + 1;

        let mut bb = BatchBuilder::new(&mut req_buf, batch_size);

        let mut build_ok = true;
        for i in 0..batch_size {
            let mut item = [0u8; INCREMENT_PAYLOAD_SIZE];
            let val = counter + i as u64;
            increment_encode(val, &mut item);
            expected[i as usize] = val + 1;

            if bb.add(&item).is_err() {
                errors += 1;
                build_ok = false;
                break;
            }
        }
        if !build_ok {
            continue;
        }

        let (req_len, _out_count) = bb.finish();

        let mut hdr = Header {
            kind: KIND_REQUEST,
            code: METHOD_INCREMENT,
            flags: FLAG_BATCH,
            item_count: batch_size,
            message_id: counter + 1,
            transport_status: STATUS_OK,
            ..Header::default()
        };

        let t0 = if total_items & 63 == 0 { Some(Instant::now()) } else { None };

        // Send + receive via SHM or Named Pipe
        let io_result: Result<(Header, Vec<u8>), ()> = if let Some(ref mut shm_ctx) = shm {
            let msg_len = HEADER_SIZE + req_len;
            let mut msg = vec![0u8; msg_len];
            hdr.magic = MAGIC_MSG;
            hdr.version = VERSION;
            hdr.header_len = protocol::HEADER_LEN;
            hdr.payload_len = req_len as u32;
            hdr.encode(&mut msg[..HEADER_SIZE]);
            msg[HEADER_SIZE..].copy_from_slice(&req_buf[..req_len]);

            if shm_ctx.send(&msg).is_err() {
                Err(())
            } else {
                match shm_ctx.receive(&mut recv_buf, 30000) {
                    Ok(resp_len) => {
                        if resp_len < HEADER_SIZE {
                            Err(())
                        } else {
                            match Header::decode(&recv_buf[..resp_len]) {
                                Ok(resp_hdr) => {
                                    let payload = recv_buf[HEADER_SIZE..resp_len].to_vec();
                                    Ok((resp_hdr, payload))
                                }
                                Err(_) => Err(()),
                            }
                        }
                    }
                    Err(_) => Err(()),
                }
            }
        } else {
            match session.send(&mut hdr, &req_buf[..req_len]) {
                Ok(_) => session.receive(&mut recv_buf).map_err(|_| ()),
                Err(_) => Err(()),
            }
        };

        let (resp_hdr, resp_payload) = match io_result {
            Ok(r) => r,
            Err(_) => {
                errors += 1;
                break; // desync
            }
        };

        if resp_hdr.kind != KIND_RESPONSE
            || resp_hdr.code != METHOD_INCREMENT
            || resp_hdr.item_count != batch_size
        {
            errors += 1;
            counter += batch_size as u64;
            total_items += batch_size as u64;
            continue;
        }

        let mut batch_ok = true;
        for i in 0..batch_size {
            match batch_item_get(&resp_payload, batch_size, i) {
                Ok((item_data, _item_len)) => match increment_decode(item_data) {
                    Ok(resp_val) => {
                        if resp_val != expected[i as usize] {
                            errors += 1;
                            batch_ok = false;
                            break;
                        }
                    }
                    Err(_) => {
                        errors += 1;
                        batch_ok = false;
                        break;
                    }
                },
                Err(_) => {
                    errors += 1;
                    batch_ok = false;
                    break;
                }
            }
        }

        if let Some(t0v) = t0 {
            lr.record(t0v.elapsed().as_nanos() as u64);
        }

        total_items += batch_size as u64;
        let _ = batch_ok;
        counter += batch_size as u64;
    }

    let wall_sec = wall_start.elapsed().as_secs_f64();
    let cpu_end = cpu_ns();
    let cpu_sec = (cpu_end - cpu_start) as f64 / 1e9;
    let throughput = total_items as f64 / wall_sec;
    let cpu_pct = (cpu_sec / wall_sec) * 100.0;

    let p50 = lr.percentile(50.0) / 1000;
    let p95 = lr.percentile(95.0) / 1000;
    let p99 = lr.percentile(99.0) / 1000;

    println!(
        "{},rust,{},{:.0},{},{},{},{:.1},0.0,{:.1}",
        scenario, server_lang, throughput, p50, p95, p99, cpu_pct, cpu_pct
    );

    if let Some(mut shm_ctx) = shm {
        shm_ctx.close();
    }
    drop(session);

    if errors > 0 {
        eprintln!("batch client: {} errors out of {} items", errors, total_items);
    }

    if errors > 0 { 1 } else { 0 }
}

// ---------------------------------------------------------------------------
//  Pipeline client (sends depth requests, then reads depth responses)
// ---------------------------------------------------------------------------

#[cfg(windows)]
fn run_pipeline_client(
    run_dir: &str,
    service: &str,
    duration_sec: u32,
    target_rps: u64,
    depth: u32,
    server_lang: &str,
) -> i32 {
    let mut session = None;
    for _ in 0..200 {
        match NpSession::connect(run_dir, service, &client_config(PROFILE_NP)) {
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
            eprintln!("pipeline client: connect failed after retries");
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
    let tick_deadline = TickDeadline::new(duration_sec);

    while !tick_deadline.expired() {
        rl.wait();

        let t0 = if requests & 63 == 0 { Some(Instant::now()) } else { None };

        // Send `depth` requests
        let mut send_ok = true;
        for d in 0..depth {
            let val = counter + d as u64;
            let req_payload = val.to_ne_bytes();

            let mut hdr = Header {
                kind: KIND_REQUEST,
                code: METHOD_INCREMENT,
                flags: 0,
                item_count: 1,
                message_id: val + 1,
                transport_status: STATUS_OK,
                ..Header::default()
            };

            if session.send(&mut hdr, &req_payload).is_err() {
                send_ok = false;
                errors += 1;
                break;
            }
        }

        if !send_ok {
            continue;
        }

        // Receive `depth` responses
        let mut recv_buf = vec![0u8; 256];
        for d in 0..depth {
            match session.receive(&mut recv_buf) {
                Ok((_resp_hdr, payload)) => {
                    if payload.len() >= 8 {
                        let resp_val = u64::from_ne_bytes(payload[..8].try_into().unwrap());
                        let expected = counter + d as u64 + 1;
                        if resp_val != expected {
                            eprintln!(
                                "pipeline chain broken at depth {}: expected {}, got {}",
                                d, expected, resp_val
                            );
                            errors += 1;
                        }
                    }
                }
                Err(_) => {
                    errors += 1;
                    break;
                }
            }
        }

        if let Some(t0v) = t0 {
            lr.record(t0v.elapsed().as_nanos() as u64);
        }

        counter += depth as u64;
        requests += depth as u64;
    }

    let wall_sec = wall_start.elapsed().as_secs_f64();
    let cpu_end = cpu_ns();
    let cpu_sec = (cpu_end - cpu_start) as f64 / 1e9;
    let throughput = requests as f64 / wall_sec;
    let cpu_pct = (cpu_sec / wall_sec) * 100.0;

    let p50 = lr.percentile(50.0) / 1000;
    let p95 = lr.percentile(95.0) / 1000;
    let p99 = lr.percentile(99.0) / 1000;

    let scenario = format!("np-pipeline-d{}", depth);
    println!(
        "{},rust,{},{:.0},{},{},{},{:.1},0.0,{:.1}",
        scenario, server_lang, throughput, p50, p95, p99, cpu_pct, cpu_pct
    );

    drop(session);

    if errors > 0 {
        eprintln!("pipeline client: {} errors", errors);
    }

    if errors > 0 { 1 } else { 0 }
}

// ---------------------------------------------------------------------------
//  Pipeline+batch client (sends depth batch messages, reads depth responses)
// ---------------------------------------------------------------------------

#[cfg(windows)]
fn run_pipeline_batch_client(
    run_dir: &str,
    service: &str,
    duration_sec: u32,
    target_rps: u64,
    depth: u32,
    server_lang: &str,
) -> i32 {
    let mut session = None;
    for _ in 0..200 {
        match NpSession::connect(run_dir, service, &batch_client_config(PROFILE_NP)) {
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
            eprintln!("pipeline-batch client: connect failed after retries");
            return 1;
        }
    };

    let mut lr = LatencyRecorder::new(2_000_000);
    let mut rl = RateLimiter::new(target_rps);
    let mut rng = XorShift32::new();

    let mut counter: u64 = 0;
    let mut total_items: u64 = 0;
    let mut errors: u64 = 0;

    let depth = depth.min(128) as usize;
    let mut req_bufs: Vec<Vec<u8>> = (0..depth).map(|_| vec![0u8; BENCH_BATCH_BUF_SIZE]).collect();
    let mut req_lens = vec![0usize; depth];
    let mut batch_sizes = vec![0u32; depth];

    let cpu_start = cpu_ns();
    let wall_start = Instant::now();
    let tick_deadline = TickDeadline::new(duration_sec);

    while !tick_deadline.expired() {
        rl.wait();

        let t0 = if total_items & 63 == 0 { Some(Instant::now()) } else { None };

        let mut send_ok = true;
        for d in 0..depth {
            let bs = (rng.next() % BENCH_MAX_BATCH_ITEMS) + 1;
            batch_sizes[d] = bs;

            let mut bb = BatchBuilder::new(&mut req_bufs[d], bs);

            for i in 0..bs {
                let mut item = [0u8; INCREMENT_PAYLOAD_SIZE];
                increment_encode(counter + i as u64, &mut item);
                if bb.add(&item).is_err() {
                    send_ok = false;
                    errors += 1;
                    break;
                }
            }
            if !send_ok {
                break;
            }

            let (len, _out_count) = bb.finish();
            req_lens[d] = len;

            let mut hdr = Header {
                kind: KIND_REQUEST,
                code: METHOD_INCREMENT,
                flags: FLAG_BATCH,
                item_count: bs,
                message_id: counter + 1 + d as u64,
                transport_status: STATUS_OK,
                ..Header::default()
            };

            if session.send(&mut hdr, &req_bufs[d][..len]).is_err() {
                send_ok = false;
                errors += 1;
                break;
            }

            counter += bs as u64;
        }

        if !send_ok {
            continue;
        }

        // Receive `depth` batch responses
        let mut recv_buf = vec![0u8; BENCH_BATCH_BUF_SIZE + HEADER_SIZE];
        for d in 0..depth {
            match session.receive(&mut recv_buf) {
                Ok((_resp_hdr, _payload)) => {
                    total_items += batch_sizes[d] as u64;
                }
                Err(_) => {
                    errors += 1;
                    break;
                }
            }
        }

        if let Some(t0v) = t0 {
            lr.record(t0v.elapsed().as_nanos() as u64);
        }
    }

    let wall_sec = wall_start.elapsed().as_secs_f64();
    let cpu_end = cpu_ns();
    let cpu_sec = (cpu_end - cpu_start) as f64 / 1e9;
    let throughput = total_items as f64 / wall_sec;
    let cpu_pct = (cpu_sec / wall_sec) * 100.0;

    let p50 = lr.percentile(50.0) / 1000;
    let p95 = lr.percentile(95.0) / 1000;
    let p99 = lr.percentile(99.0) / 1000;

    let scenario = format!("np-pipeline-batch-d{}", depth);
    println!(
        "{},rust,{},{:.0},{},{},{},{:.1},0.0,{:.1}",
        scenario, server_lang, throughput, p50, p95, p99, cpu_pct, cpu_pct
    );

    drop(session);

    if errors > 0 {
        eprintln!("pipeline-batch client: {} errors", errors);
    }

    if errors > 0 { 1 } else { 0 }
}

// ---------------------------------------------------------------------------
//  Lookup benchmark (L3 cache, no transport)
// ---------------------------------------------------------------------------

#[cfg(windows)]
fn run_lookup_bench(duration_sec: u32) -> i32 {
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
    let tick_deadline = TickDeadline::new(duration_sec);

    while !tick_deadline.expired() {
        for item in &items {
            let found = items
                .iter()
                .find(|it| it.hash == item.hash && it.name == item.name);
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

    println!(
        "lookup,rust,rust,{:.0},0,0,0,{:.1},0.0,{:.1}",
        throughput, cpu_pct, cpu_pct
    );

    if hits != lookups {
        eprintln!("lookup: missed {}/{}", lookups - hits, lookups);
        return 1;
    }

    0
}

// ---------------------------------------------------------------------------
//  Main
// ---------------------------------------------------------------------------

#[cfg(windows)]
fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!(
            "Usage: {} <subcommand> [args...]\n\
             Subcommands:\n  \
               np-ping-pong-{{server,client}}, shm-ping-pong-{{server,client}}\n  \
               snapshot-{{server,client}}, snapshot-shm-{{server,client}}\n  \
               np-batch-ping-pong-{{server,client}}, shm-batch-ping-pong-{{server,client}}\n  \
               np-pipeline-client, np-pipeline-batch-client\n  \
               lookup-bench",
            args[0]
        );
        std::process::exit(1);
    }

    let cmd = &args[1];

    let rc = match cmd.as_str() {
        // --- Server subcommands ---
        "np-ping-pong-server" | "shm-ping-pong-server"
        | "snapshot-server" | "snapshot-shm-server" => {
            if args.len() < 4 {
                eprintln!(
                    "Usage: {} {} <run_dir> <service> [duration_sec]",
                    args[0], cmd
                );
                std::process::exit(1);
            }
            let run_dir = &args[2];
            let service = &args[3];
            let duration: u32 = if args.len() >= 5 {
                args[4].parse().unwrap_or(DEFAULT_DURATION)
            } else {
                DEFAULT_DURATION
            };

            let _ = std::fs::create_dir_all(run_dir);

            let (profiles, handler_type) = match cmd.as_str() {
                "np-ping-pong-server" => (PROFILE_NP, "ping-pong"),
                "shm-ping-pong-server" => (PROFILE_SHM, "ping-pong"),
                "snapshot-server" => (PROFILE_NP, "snapshot"),
                "snapshot-shm-server" => (PROFILE_SHM, "snapshot"),
                _ => unreachable!(),
            };

            run_server(run_dir, service, profiles, duration, handler_type)
        }

        // --- Ping-pong client ---
        "np-ping-pong-client" | "shm-ping-pong-client" => {
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
                "np-ping-pong-client" => (PROFILE_NP, "np-ping-pong"),
                "shm-ping-pong-client" => (PROFILE_SHM, "shm-ping-pong"),
                _ => unreachable!(),
            };

            run_ping_pong_client(
                run_dir, service, profiles, duration, target_rps, scenario, "rust",
            )
        }

        // --- Snapshot client ---
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
                "snapshot-client" => (PROFILE_NP, "snapshot-baseline"),
                "snapshot-shm-client" => (PROFILE_SHM, "snapshot-shm"),
                _ => unreachable!(),
            };

            run_snapshot_client(
                run_dir, service, profiles, duration, target_rps, scenario, "rust",
            )
        }

        // --- Batch server ---
        "np-batch-ping-pong-server" | "shm-batch-ping-pong-server" => {
            if args.len() < 4 {
                eprintln!(
                    "Usage: {} {} <run_dir> <service> [duration_sec]",
                    args[0], cmd
                );
                std::process::exit(1);
            }
            let run_dir = &args[2];
            let service = &args[3];
            let duration: u32 = if args.len() >= 5 {
                args[4].parse().unwrap_or(DEFAULT_DURATION)
            } else {
                DEFAULT_DURATION
            };

            let _ = std::fs::create_dir_all(run_dir);

            let profiles = if cmd == "np-batch-ping-pong-server" {
                PROFILE_NP
            } else {
                PROFILE_SHM
            };

            run_batch_server(run_dir, service, profiles, duration)
        }

        // --- Batch client ---
        "np-batch-ping-pong-client" | "shm-batch-ping-pong-client" => {
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

            let (profiles, scenario) = if cmd == "np-batch-ping-pong-client" {
                (PROFILE_NP, "np-batch-ping-pong")
            } else {
                (PROFILE_SHM, "shm-batch-ping-pong")
            };

            run_batch_ping_pong_client(
                run_dir, service, profiles, duration, target_rps, scenario, "rust",
            )
        }

        // --- Pipeline client (Named Pipe only) ---
        "np-pipeline-client" => {
            if args.len() < 7 {
                eprintln!(
                    "Usage: {} {} <run_dir> <service> <duration_sec> <target_rps> <depth>",
                    args[0], cmd
                );
                std::process::exit(1);
            }
            let run_dir = &args[2];
            let service = &args[3];
            let duration: u32 = args[4].parse().unwrap_or(5);
            let target_rps: u64 = args[5].parse().unwrap_or(0);
            let depth: u32 = args[6].parse().unwrap_or(1).max(1);

            run_pipeline_client(run_dir, service, duration, target_rps, depth, "rust")
        }

        // --- Pipeline+batch client (Named Pipe only) ---
        "np-pipeline-batch-client" => {
            if args.len() < 7 {
                eprintln!(
                    "Usage: {} {} <run_dir> <service> <duration_sec> <target_rps> <depth>",
                    args[0], cmd
                );
                std::process::exit(1);
            }
            let run_dir = &args[2];
            let service = &args[3];
            let duration: u32 = args[4].parse().unwrap_or(5);
            let target_rps: u64 = args[5].parse().unwrap_or(0);
            let depth: u32 = args[6].parse().unwrap_or(1).max(1);

            run_pipeline_batch_client(run_dir, service, duration, target_rps, depth, "rust")
        }

        // --- Lookup benchmark ---
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
