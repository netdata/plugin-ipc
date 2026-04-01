//! bench_posix - POSIX benchmark driver for netipc (Rust).
//!
//! Exercises the public L1/L2/L3 API surface. Measures throughput,
//! latency (p50/p95/p99), and CPU.
//!
//! Same subcommands and output format as the C driver.

#[cfg(windows)]
fn main() {
    eprintln!("bench_posix is only supported on POSIX platforms");
    std::process::exit(1);
}

#[cfg(not(windows))]
mod posix_only {

    use netipc::protocol::{
        self, batch_item_get, increment_decode, increment_encode, BatchBuilder, Header, FLAG_BATCH,
        HEADER_SIZE, INCREMENT_PAYLOAD_SIZE, KIND_REQUEST, KIND_RESPONSE, MAGIC_MSG,
        METHOD_CGROUPS_SNAPSHOT, METHOD_INCREMENT, PROFILE_BASELINE, PROFILE_SHM_HYBRID, STATUS_OK,
        VERSION,
    };
    use netipc::service::cgroups::{CgroupsClient, ClientConfig as TypedClientConfig};
    use netipc::service::raw::{
        increment_dispatch, snapshot_dispatch, CgroupsCacheItem, DispatchHandler, ManagedServer,
    };
    use netipc::transport::posix::{ClientConfig, ServerConfig, UdsSession};

    #[cfg(target_os = "linux")]
    use netipc::transport::shm::ShmContext;

    use std::sync::{atomic::Ordering, OnceLock};
    use std::time::{Duration, Instant};

    // ---------------------------------------------------------------------------
    //  Constants
    // ---------------------------------------------------------------------------

    const AUTH_TOKEN: u64 = 0xBE4C400000C0FFEE;
    const RESPONSE_BUF_SIZE: usize = 65536;
    const MAX_LATENCY_SAMPLES: usize = 10_000_000;

    const PROFILE_UDS: u32 = PROFILE_BASELINE;
    const PROFILE_SHM: u32 = PROFILE_BASELINE | PROFILE_SHM_HYBRID;

    // Batch benchmark constants (mirror C driver)
    const BENCH_MAX_BATCH_ITEMS: u32 = 1000;
    const BENCH_BATCH_BUF_SIZE: usize = BENCH_MAX_BATCH_ITEMS as usize * 48 + 4096;

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

    fn typed_client_config(profiles: u32) -> TypedClientConfig {
        TypedClientConfig {
            supported_profiles: profiles,
            preferred_profiles: profiles,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 1,
            max_response_payload_bytes: RESPONSE_BUF_SIZE as u32,
            max_response_batch_items: 1,
            auth_token: AUTH_TOKEN,
            ..TypedClientConfig::default()
        }
    }

    fn batch_server_config(profiles: u32) -> ServerConfig {
        ServerConfig {
            supported_profiles: profiles,
            preferred_profiles: profiles,
            max_request_payload_bytes: BENCH_BATCH_BUF_SIZE as u32,
            max_request_batch_items: BENCH_MAX_BATCH_ITEMS,
            max_response_payload_bytes: BENCH_BATCH_BUF_SIZE as u32,
            max_response_batch_items: BENCH_MAX_BATCH_ITEMS,
            auth_token: AUTH_TOKEN,
            backlog: 4,
            ..ServerConfig::default()
        }
    }

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

    fn ping_pong_handler() -> DispatchHandler {
        increment_dispatch(std::sync::Arc::new(|counter| Some(counter + 1)))
    }

    // ---------------------------------------------------------------------------
    //  Snapshot handler (16 cgroup items)
    // ---------------------------------------------------------------------------

    fn snapshot_template() -> &'static Vec<(Box<[u8]>, Box<[u8]>)> {
        static TEMPLATE: OnceLock<Vec<(Box<[u8]>, Box<[u8]>)>> = OnceLock::new();
        TEMPLATE.get_or_init(|| {
            (0..16u32)
                .map(|i| {
                    (
                        format!("cgroup-{i}").into_bytes().into_boxed_slice(),
                        format!("/sys/fs/cgroup/bench/cg-{i}")
                            .into_bytes()
                            .into_boxed_slice(),
                    )
                })
                .collect()
        })
    }

    fn snapshot_handler() -> DispatchHandler {
        snapshot_dispatch(
            std::sync::Arc::new(|request, builder| {
                if request.layout_version != 1 || request.flags != 0 {
                    return false;
                }

                thread_local! {
                    static GEN: std::cell::Cell<u64> = const { std::cell::Cell::new(0) };
                }
                let gen = GEN.with(|g| {
                    let v = g.get() + 1;
                    g.set(v);
                    v
                });

                builder.set_header(1, gen);
                for (i, (name, path)) in snapshot_template().iter().enumerate() {
                    if builder
                        .add(1000 + i as u32, 0, i as u32 % 2, name, path)
                        .is_err()
                    {
                        return false;
                    }
                }
                true
            }),
            16,
        )
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
        let (expected_method_code, handler) = match handler_type {
            "ping-pong" => (METHOD_INCREMENT, Some(ping_pong_handler())),
            "snapshot" => (METHOD_CGROUPS_SNAPSHOT, Some(snapshot_handler())),
            _ => {
                eprintln!("Unknown handler type: {handler_type}");
                return 1;
            }
        };

        let mut server = ManagedServer::new(
            run_dir,
            service,
            server_config(profiles),
            expected_method_code,
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

    fn run_batch_server(run_dir: &str, service: &str, profiles: u32, duration_sec: u32) -> i32 {
        let mut server = ManagedServer::new(
            run_dir,
            service,
            batch_server_config(profiles),
            METHOD_INCREMENT,
            Some(ping_pong_handler()),
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
    //  Batch ping-pong client (random 2-1000 items per batch)
    // ---------------------------------------------------------------------------

    /// Simple xorshift32 PRNG (mirrors C bench_rand)
    struct XorShift32 {
        state: u32,
    }

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
            match UdsSession::connect(run_dir, service, &batch_client_config(profiles)) {
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
        let mut msg_buf = vec![0u8; BENCH_BATCH_BUF_SIZE + HEADER_SIZE];
        let mut expected = vec![0u64; BENCH_MAX_BATCH_ITEMS as usize];

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
                if shm_ctx.is_none() {
                    eprintln!(
                    "batch client: shm attach failed after retries (profile=0x{:x}, session={})",
                    sp, session.session_id
                );
                }
                shm_ctx
            } else {
                None
            }
        };
        #[cfg(not(target_os = "linux"))]
        let shm: Option<()> = None;

        #[cfg(target_os = "linux")]
        if (session.selected_profile == PROFILE_SHM_HYBRID
            || session.selected_profile == protocol::PROFILE_SHM_FUTEX)
            && shm.is_none()
        {
            drop(session);
            return 1;
        }

        let cpu_start = cpu_ns();
        let wall_start = Instant::now();
        let deadline = Duration::from_secs(duration_sec as u64);

        while wall_start.elapsed() < deadline {
            rl.wait();

            // Random batch size 2-1000. item_count==1 is normalized to the
            // single-item increment path by the server, so it is not a real
            // batch round trip.
            let batch_size = (rng.next() % (BENCH_MAX_BATCH_ITEMS - 1)) + 2;

            // Build batch request
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

            let t0 = Instant::now();

            // Send + receive via SHM or UDS
            #[cfg(target_os = "linux")]
            let io_result = if let Some(ref mut shm_ctx) = shm {
                // SHM path: assemble header + payload into one message
                let msg_len = HEADER_SIZE + req_len;
                let msg = &mut msg_buf[..msg_len];
                hdr.magic = MAGIC_MSG;
                hdr.version = VERSION;
                hdr.header_len = protocol::HEADER_LEN;
                hdr.payload_len = req_len as u32;
                hdr.encode(&mut msg[..HEADER_SIZE]);
                msg[HEADER_SIZE..].copy_from_slice(&req_buf[..req_len]);

                if let Err(err) = shm_ctx.send(&msg) {
                    eprintln!("batch client: shm send failed: {err}");
                    Err(())
                } else {
                    match shm_ctx.receive(&mut recv_buf, 30000) {
                        Ok(resp_len) => {
                            if resp_len < HEADER_SIZE {
                                eprintln!(
                                    "batch client: shm response too short: {} bytes",
                                    resp_len
                                );
                                Err(())
                            } else {
                                match Header::decode(&recv_buf[..resp_len]) {
                                    Ok(resp_hdr) => {
                                        let payload = &recv_buf[HEADER_SIZE..resp_len];
                                        Ok((resp_hdr, payload))
                                    }
                                    Err(err) => {
                                        eprintln!("batch client: shm header decode failed: {err}");
                                        Err(())
                                    }
                                }
                            }
                        }
                        Err(err) => {
                            eprintln!("batch client: shm receive failed: {err}");
                            Err(())
                        }
                    }
                }
            } else {
                // UDS path
                match session.send(&mut hdr, &req_buf[..req_len]) {
                    Ok(_) => session.receive(&mut recv_buf).map_err(|_| ()),
                    Err(_) => Err(()),
                }
            };

            #[cfg(not(target_os = "linux"))]
            let io_result = match session.send(&mut hdr, &req_buf[..req_len]) {
                Ok(_) => session.receive(&mut recv_buf).map_err(|_| ()),
                Err(_) => Err(()),
            };

            let (resp_hdr, resp_payload) = match io_result {
                Ok(r) => r,
                Err(_) => {
                    errors += 1;
                    break; // desync — stop
                }
            };

            // Validate response header
            if resp_hdr.kind != KIND_RESPONSE
                || resp_hdr.code != METHOD_INCREMENT
                || resp_hdr.item_count != batch_size
            {
                errors += 1;
                counter += batch_size as u64;
                total_items += batch_size as u64;
                continue;
            }

            // Verify each item
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

            let t1 = Instant::now();
            lr.record((t1 - t0).as_nanos() as u64);

            // Count items regardless of verification errors
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

        #[cfg(target_os = "linux")]
        if let Some(mut shm_ctx) = shm {
            shm_ctx.close();
        }
        drop(session);

        if errors > 0 {
            eprintln!(
                "batch client: {} errors out of {} items",
                errors, total_items
            );
        }

        if errors > 0 {
            1
        } else {
            0
        }
    }

    // ---------------------------------------------------------------------------
    //  Pipeline client (sends depth requests, then reads depth responses)
    // ---------------------------------------------------------------------------

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
            match UdsSession::connect(run_dir, service, &client_config(PROFILE_UDS)) {
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
        let mut recv_buf = vec![0u8; 256];

        let cpu_start = cpu_ns();
        let wall_start = Instant::now();
        let deadline = Duration::from_secs(duration_sec as u64);

        while wall_start.elapsed() < deadline {
            rl.wait();

            let t0 = Instant::now();

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

            let t1 = Instant::now();
            lr.record((t1 - t0).as_nanos() as u64);

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

        let scenario = format!("uds-pipeline-d{}", depth);
        println!(
            "{},rust,{},{:.0},{},{},{},{:.1},0.0,{:.1}",
            scenario, server_lang, throughput, p50, p95, p99, cpu_pct, cpu_pct
        );

        drop(session);

        if errors > 0 {
            eprintln!("pipeline client: {} errors", errors);
        }

        if errors > 0 {
            1
        } else {
            0
        }
    }

    // ---------------------------------------------------------------------------
    //  Pipeline+batch client (sends depth batch messages, reads depth responses)
    // ---------------------------------------------------------------------------

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
            match UdsSession::connect(run_dir, service, &batch_client_config(PROFILE_UDS)) {
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
        let mut req_bufs: Vec<Vec<u8>> = (0..depth)
            .map(|_| vec![0u8; BENCH_BATCH_BUF_SIZE])
            .collect();
        let mut req_lens = vec![0usize; depth];
        let mut batch_sizes = vec![0u32; depth];
        let mut recv_buf = vec![0u8; BENCH_BATCH_BUF_SIZE + HEADER_SIZE];

        let cpu_start = cpu_ns();
        let wall_start = Instant::now();
        let deadline = Duration::from_secs(duration_sec as u64);

        while wall_start.elapsed() < deadline {
            rl.wait();

            let t0 = Instant::now();

            // Build and send `depth` batch requests
            let mut send_ok = true;
            for d in 0..depth {
                let bs = (rng.next() % (BENCH_MAX_BATCH_ITEMS - 1)) + 2;
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

            let t1 = Instant::now();
            lr.record((t1 - t0).as_nanos() as u64);
        }

        let wall_sec = wall_start.elapsed().as_secs_f64();
        let cpu_end = cpu_ns();
        let cpu_sec = (cpu_end - cpu_start) as f64 / 1e9;
        let throughput = total_items as f64 / wall_sec;
        let cpu_pct = (cpu_sec / wall_sec) * 100.0;

        let p50 = lr.percentile(50.0) / 1000;
        let p95 = lr.percentile(95.0) / 1000;
        let p99 = lr.percentile(99.0) / 1000;

        let scenario = format!("uds-pipeline-batch-d{}", depth);
        println!(
            "{},rust,{},{:.0},{},{},{},{:.1},0.0,{:.1}",
            scenario, server_lang, throughput, p50, p95, p99, cpu_pct, cpu_pct
        );

        drop(session);

        if errors > 0 {
            eprintln!("pipeline-batch client: {} errors", errors);
        }

        if errors > 0 {
            1
        } else {
            0
        }
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

        #[cfg(target_os = "linux")]
        if (session.selected_profile == PROFILE_SHM_HYBRID
            || session.selected_profile == protocol::PROFILE_SHM_FUTEX)
            && shm.is_none()
        {
            eprintln!("client: shm attach failed after retries");
            drop(session);
            return 1;
        }

        let mut msg_buf = vec![0u8; HEADER_SIZE + 8];
        let mut resp_shm_buf = vec![0u8; HEADER_SIZE + 64];
        let mut recv_buf = vec![0u8; 256];

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
                let msg = &mut msg_buf[..msg_len];
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

                match shm_ctx.receive(&mut resp_shm_buf, 30000) {
                    Ok(resp_len) => {
                        if resp_len >= HEADER_SIZE + 8 {
                            let resp_val = u64::from_ne_bytes(
                                resp_shm_buf[HEADER_SIZE..HEADER_SIZE + 8]
                                    .try_into()
                                    .unwrap(),
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
                // UDS path
                if session.send(&mut hdr, &req_payload).is_err() {
                    errors += 1;
                    continue;
                }
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

        if errors > 0 {
            1
        } else {
            0
        }
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
        let mut client = CgroupsClient::new(run_dir, service, typed_client_config(profiles));

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

        while wall_start.elapsed() < deadline {
            rl.wait();

            let t0 = Instant::now();

            match client.call_snapshot() {
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

        if errors > 0 {
            1
        } else {
            0
        }
    }

    // ---------------------------------------------------------------------------
    //  Lookup benchmark (L3 cache, no transport)
    // ---------------------------------------------------------------------------

    fn run_lookup_bench(duration_sec: u32) -> i32 {
        #[derive(Clone, Copy, Default)]
        struct Bucket {
            index: usize,
            used: bool,
        }

        fn hash_name(name: &str) -> u32 {
            let mut h: u32 = 5381;
            for b in name.as_bytes() {
                h = ((h << 5).wrapping_add(h)).wrapping_add(*b as u32);
            }
            h
        }

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

        let mut lookup_index = vec![Bucket::default(); 32];
        let mask = (lookup_index.len() - 1) as u32;
        for (idx, item) in items.iter().enumerate() {
            let mut slot = (item.hash ^ hash_name(&item.name)) & mask;
            while lookup_index[slot as usize].used {
                slot = (slot + 1) & mask;
            }
            lookup_index[slot as usize] = Bucket {
                index: idx,
                used: true,
            };
        }

        let mut lookups: u64 = 0;
        let mut hits: u64 = 0;

        let cpu_start = cpu_ns();
        let wall_start = Instant::now();
        let deadline = Duration::from_secs(duration_sec as u64);

        while wall_start.elapsed() < deadline {
            for item in &items {
                let mut slot = (item.hash ^ hash_name(&item.name)) & mask;
                let mut found = false;
                while lookup_index[slot as usize].used {
                    let bucket_item = &items[lookup_index[slot as usize].index];
                    if bucket_item.hash == item.hash && bucket_item.name == item.name {
                        found = true;
                        break;
                    }
                    slot = (slot + 1) & mask;
                }
                if found {
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

    pub(crate) fn main() {
        // Ignore SIGPIPE
        unsafe {
            libc::signal(libc::SIGPIPE, libc::SIG_IGN);
        }

        let args: Vec<String> = std::env::args().collect();
        if args.len() < 2 {
            eprintln!(
                "Usage: {} <subcommand> [args...]\n\
             Subcommands:\n  \
               uds-ping-pong-{{server,client}}, shm-ping-pong-{{server,client}}\n  \
               uds-batch-ping-pong-{{server,client}}, shm-batch-ping-pong-{{server,client}}\n  \
               snapshot-{{server,client}}, snapshot-shm-{{server,client}}\n  \
               uds-pipeline-client, uds-pipeline-batch-client\n  \
               lookup-bench",
                args[0]
            );
            std::process::exit(1);
        }

        let cmd = &args[1];

        let rc = match cmd.as_str() {
            "uds-ping-pong-server"
            | "shm-ping-pong-server"
            | "snapshot-server"
            | "snapshot-shm-server" => {
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

                run_ping_pong_client(
                    run_dir, service, profiles, duration, target_rps, scenario, "rust",
                )
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

                run_snapshot_client(
                    run_dir, service, profiles, duration, target_rps, scenario, "rust",
                )
            }

            // Batch server subcommands
            "uds-batch-ping-pong-server" | "shm-batch-ping-pong-server" => {
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
                    args[4].parse().unwrap_or(30)
                } else {
                    30
                };

                let _ = std::fs::create_dir_all(run_dir);

                let profiles = if cmd == "uds-batch-ping-pong-server" {
                    PROFILE_UDS
                } else {
                    PROFILE_SHM
                };

                run_batch_server(run_dir, service, profiles, duration)
            }

            // Batch client subcommands
            "uds-batch-ping-pong-client" | "shm-batch-ping-pong-client" => {
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

                let (profiles, scenario) = if cmd == "uds-batch-ping-pong-client" {
                    (PROFILE_UDS, "uds-batch-ping-pong")
                } else {
                    (PROFILE_SHM, "shm-batch-ping-pong")
                };

                run_batch_ping_pong_client(
                    run_dir, service, profiles, duration, target_rps, scenario, "rust",
                )
            }

            // Pipeline client
            "uds-pipeline-client" => {
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

            // Pipeline+batch client
            "uds-pipeline-batch-client" => {
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
}

#[cfg(not(windows))]
fn main() {
    posix_only::main();
}
