use netipc::{
    aligned_item_size, cgroups_snapshot_item_payload_len,
    decode_cgroups_snapshot_request_view, decode_cgroups_snapshot_view, decode_increment_request,
    decode_increment_response, decode_message_header, encode_cgroups_snapshot_request_payload,
    encode_increment_request, encode_increment_response, encode_message_header,
    CgroupsSnapshotClient, CgroupsSnapshotClientConfig, CgroupsSnapshotItem,
    CgroupsSnapshotRequest, CgroupsSnapshotResponseBuilder, Frame, IncrementRequest,
    IncrementResponse, MessageHeader, CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN,
    CGROUPS_SNAPSHOT_RESPONSE_HEADER_LEN, FRAME_SIZE, MESSAGE_FLAG_BATCH, MESSAGE_HEADER_LEN,
    MESSAGE_ITEM_REF_LEN, MESSAGE_KIND_REQUEST, MESSAGE_KIND_RESPONSE, MESSAGE_MAGIC,
    MESSAGE_VERSION, METHOD_CGROUPS_SNAPSHOT, STATUS_OK, TRANSPORT_STATUS_OK,
};
use std::env;
use std::fs;
use std::io;
use std::process;
use std::time::{Duration, Instant};

#[cfg(unix)]
use netipc::transport::posix::{UdsSeqpacketConfig, UdsSeqpacketServer, PROFILE_UDS_SEQPACKET};
#[cfg(windows)]
use netipc::transport::windows::self_cpu_seconds as transport_self_cpu_seconds;
#[cfg(windows)]
use netipc::transport::windows::{NamedPipeConfig, NamedPipeServer, PROFILE_NAMED_PIPE};

struct BenchResult {
    duration_sec: i32,
    target_rps: i32,
    requests: u64,
    responses: u64,
    mismatches: u64,
    throughput_rps: f64,
    p50_us: f64,
    p95_us: f64,
    p99_us: f64,
    client_cpu_cores: f64,
}

#[cfg(unix)]
fn self_cpu_seconds() -> f64 {
    let mut usage = std::mem::MaybeUninit::<libc::rusage>::uninit();
    let rc = unsafe { libc::getrusage(libc::RUSAGE_SELF, usage.as_mut_ptr()) };
    if rc != 0 {
        return 0.0;
    }
    let usage = unsafe { usage.assume_init() };
    usage.ru_utime.tv_sec as f64
        + usage.ru_utime.tv_usec as f64 / 1e6
        + usage.ru_stime.tv_sec as f64
        + usage.ru_stime.tv_usec as f64 / 1e6
}

#[cfg(windows)]
fn self_cpu_seconds() -> f64 {
    transport_self_cpu_seconds()
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

    let now = Instant::now();
    if now >= end_at {
        return false;
    }

    let target_elapsed_ns = (requests_sent * 1_000_000_000u64) / target_rps as u64;
    let due_at = start + Duration::from_nanos(target_elapsed_ns);
    if now < due_at {
        std::thread::sleep(due_at - now);
    }

    Instant::now() < end_at
}

fn percentile_micros(lat_ns: &[u64], pct: f64) -> f64 {
    if lat_ns.is_empty() {
        return 0.0;
    }

    let mut sorted = lat_ns.to_vec();
    sorted.sort_unstable();
    let index = ((pct / 100.0) * (sorted.len().saturating_sub(1)) as f64).round() as usize;
    sorted[index.min(sorted.len() - 1)] as f64 / 1000.0
}

fn print_bench_header() {
    println!(
        "mode,duration_sec,target_rps,requests,responses,mismatches,throughput_rps,p50_us,p95_us,p99_us,client_cpu_cores"
    );
}

fn print_bench_row(mode: &str, result: &BenchResult) {
    println!(
        "{},{},{},{},{},{},{:.3},{:.3},{:.3},{:.3},{:.3}",
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
        result.client_cpu_cores
    );
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

fn parse_u32(v: &str) -> u32 {
    let parsed = parse_u64(v);
    if parsed > u32::MAX as u64 {
        eprintln!("invalid u32: {v}");
        process::exit(2);
    }
    parsed as u32
}

fn parse_env_u32(name: &str, fallback: u32) -> u32 {
    match env::var(name) {
        Ok(value) if !value.is_empty() => parse_u32(&value),
        _ => fallback,
    }
}

fn server_receive_timeout() -> Duration {
    match env::var("NETIPC_CGROUPS_SERVER_IDLE_TIMEOUT_MS") {
        Ok(value) if !value.is_empty() => Duration::from_millis(parse_u64(&value)),
        _ => Duration::from_secs(10),
    }
}

fn request_message_capacity() -> usize {
    netipc::protocol::max_batch_total_size(CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN as u32, 1)
        .unwrap_or_else(|e| {
            eprintln!("request capacity failed: {e}");
            process::exit(1);
        })
}

fn read_bytes(path: &str) -> Vec<u8> {
    fs::read(path).unwrap_or_else(|e| {
        eprintln!("read failed for {path}: {e}");
        process::exit(1);
    })
}

fn write_bytes(path: &str, data: &[u8]) {
    fs::write(path, data).unwrap_or_else(|e| {
        eprintln!("write failed for {path}: {e}");
        process::exit(1);
    });
}

fn read_frame(path: &str) -> Frame {
    let data = read_bytes(path);
    if data.len() != FRAME_SIZE {
        eprintln!("invalid frame size in {path}: {}", data.len());
        process::exit(1);
    }

    let mut frame = [0u8; FRAME_SIZE];
    frame.copy_from_slice(&data);
    frame
}

fn write_frame(path: &str, frame: &Frame) {
    write_bytes(path, frame);
}

const FIXED_CGROUPS_SEED_COUNT: usize = 16;

fn configured_cgroups_item_count() -> usize {
    let count = parse_env_u32("NETIPC_CGROUPS_ITEM_COUNT", FIXED_CGROUPS_SEED_COUNT as u32) as usize;
    if count < FIXED_CGROUPS_SEED_COUNT {
        eprintln!(
            "NETIPC_CGROUPS_ITEM_COUNT must be >= {} (got {})",
            FIXED_CGROUPS_SEED_COUNT, count
        );
        process::exit(2);
    }
    count
}

fn generated_cgroups_options(index: usize) -> u32 {
    match index % 5 {
        0 => 0x2,
        1 => 0x4,
        2 => 0x6,
        3 => 0x8,
        _ => 0x1,
    }
}

fn fixed_cgroups_items() -> Vec<CgroupsSnapshotItem<'static>> {
    vec![
        CgroupsSnapshotItem {
            hash: 123,
            options: 0x2,
            enabled: true,
            name: b"system.slice-nginx",
            path: b"/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs",
        },
        CgroupsSnapshotItem {
            hash: 456,
            options: 0x4,
            enabled: false,
            name: b"docker-1234",
            path: b"",
        },
        CgroupsSnapshotItem {
            hash: 789,
            options: 0x6,
            enabled: true,
            name: b"kubepods-burstable-pod01234567_89ab_cdef_0123_456789abcdef.slice",
            path: b"/sys/fs/cgroup/kubepods.slice/kubepods-burstable.slice/kubepods-burstable-pod01234567_89ab_cdef_0123_456789abcdef.slice/cgroup.procs",
        },
        CgroupsSnapshotItem {
            hash: 1001,
            options: 0x2,
            enabled: true,
            name: b"system.slice-sshd.service",
            path: b"/sys/fs/cgroup/system.slice/sshd.service/cgroup.procs",
        },
        CgroupsSnapshotItem {
            hash: 1002,
            options: 0x2,
            enabled: true,
            name: b"system.slice-docker.service",
            path: b"/sys/fs/cgroup/system.slice/docker.service/cgroup.procs",
        },
        CgroupsSnapshotItem {
            hash: 1003,
            options: 0x6,
            enabled: true,
            name: b"user.slice-user-1000.slice-session-3.scope",
            path: b"/sys/fs/cgroup/user.slice/user-1000.slice/session-3.scope/cgroup.procs",
        },
        CgroupsSnapshotItem {
            hash: 1004,
            options: 0x2,
            enabled: true,
            name: b"machine.slice-libvirt-qemu-5-win11.scope",
            path: b"/sys/fs/cgroup/machine.slice/libvirt-qemu-5-win11.scope/cgroup.procs",
        },
        CgroupsSnapshotItem {
            hash: 1005,
            options: 0x8,
            enabled: false,
            name: b"system.slice-telegraf.service",
            path: b"/sys/fs/cgroup/system.slice/telegraf.service/cgroup.procs",
        },
        CgroupsSnapshotItem {
            hash: 1006,
            options: 0x6,
            enabled: true,
            name: b"podman-7f0c8e91f1ce55b0c3d1b5a4f6e8d9c0.scope",
            path: b"/sys/fs/cgroup/system.slice/podman-7f0c8e91f1ce55b0c3d1b5a4f6e8d9c0.scope/cgroup.procs",
        },
        CgroupsSnapshotItem {
            hash: 1007,
            options: 0x4,
            enabled: true,
            name: b"init.scope",
            path: b"/sys/fs/cgroup/init.scope/cgroup.procs",
        },
        CgroupsSnapshotItem {
            hash: 1008,
            options: 0x6,
            enabled: true,
            name: b"system.slice-containerd.service",
            path: b"/sys/fs/cgroup/system.slice/containerd.service/cgroup.procs",
        },
        CgroupsSnapshotItem {
            hash: 1009,
            options: 0x4,
            enabled: true,
            name: b"machine.slice-systemd-nspawn-observability-lab.scope",
            path: b"/sys/fs/cgroup/machine.slice/systemd-nspawn-observability-lab.scope/cgroup.procs",
        },
        CgroupsSnapshotItem {
            hash: 1010,
            options: 0x6,
            enabled: true,
            name: b"user.slice-user-1001.slice-user@1001.service-app.slice-observability-frontend.scope",
            path: b"/sys/fs/cgroup/user.slice/user-1001.slice/user@1001.service/app.slice/observability-frontend.scope/cgroup.procs",
        },
        CgroupsSnapshotItem {
            hash: 1011,
            options: 0x1,
            enabled: false,
            name: b"crio-53d2b1b5d7a04d8f9e2f6a7b8c9d0e1f.scope",
            path: b"/sys/fs/cgroup/kubepods.slice/kubepods-pod98765432_10fe_dcba_9876_543210fedcba.slice/crio-53d2b1b5d7a04d8f9e2f6a7b8c9d0e1f.scope/cgroup.procs",
        },
        CgroupsSnapshotItem {
            hash: 1012,
            options: 0x2,
            enabled: true,
            name: b"system.slice-netdata.service",
            path: b"/sys/fs/cgroup/system.slice/netdata.service/cgroup.procs",
        },
        CgroupsSnapshotItem {
            hash: 1013,
            options: 0x6,
            enabled: true,
            name: b"system.slice-super-long-observability-ingestion-gateway-with-really-long-unit-name-to-stress-view-lifetimes.service",
            path: b"/sys/fs/cgroup/system.slice/super-long-observability-ingestion-gateway-with-really-long-unit-name-to-stress-view-lifetimes.service/cgroup.procs",
        },
    ]
}

fn fixed_cgroups_payload_capacity(item_count: usize) -> usize {
    let fixed_items = fixed_cgroups_items();
    let mut total = CGROUPS_SNAPSHOT_RESPONSE_HEADER_LEN + item_count * MESSAGE_ITEM_REF_LEN;
    for item in &fixed_items {
        let start = aligned_item_size(total as u32).unwrap_or_else(|e| {
            eprintln!("snapshot item align failed: {e}");
            process::exit(1);
        });
        let item_len = cgroups_snapshot_item_payload_len(item).unwrap_or_else(|e| {
            eprintln!("snapshot item length failed: {e}");
            process::exit(1);
        });
        total = start + item_len;
    }
    for index in fixed_items.len()..item_count {
        let name = format!(
            "system.slice-generated-observability-worker-{index:06}-with-long-synthetic-name.scope"
        );
        let path = format!(
            "/sys/fs/cgroup/system.slice/generated-observability-worker-{index:06}-with-long-synthetic-name.scope/cgroup.procs"
        );
        let item = CgroupsSnapshotItem {
            hash: 2000 + index as u32,
            options: generated_cgroups_options(index),
            enabled: index % 2 == 0,
            name: name.as_bytes(),
            path: path.as_bytes(),
        };
        let start = aligned_item_size(total as u32).unwrap_or_else(|e| {
            eprintln!("snapshot item align failed: {e}");
            process::exit(1);
        });
        let item_len = cgroups_snapshot_item_payload_len(&item).unwrap_or_else(|e| {
            eprintln!("snapshot item length failed: {e}");
            process::exit(1);
        });
        total = start + item_len;
    }
    total
}

fn fixed_cgroups_response(message_id: u64) -> Vec<u8> {
    let item_count = configured_cgroups_item_count();
    let fixed_items = fixed_cgroups_items();
    let mut payload = vec![0u8; fixed_cgroups_payload_capacity(item_count)];
    let mut builder =
        CgroupsSnapshotResponseBuilder::new(&mut payload, 42, true, 3, item_count as u32)
            .unwrap_or_else(|e| {
                eprintln!("snapshot builder init failed: {e}");
                process::exit(1);
            });
    for (idx, item) in fixed_items.iter().enumerate() {
        builder.push_item(item).unwrap_or_else(|e| {
            eprintln!("snapshot builder item {idx} failed: {e}");
            process::exit(1);
        });
    }
    for index in fixed_items.len()..item_count {
        let name = format!(
            "system.slice-generated-observability-worker-{index:06}-with-long-synthetic-name.scope"
        );
        let path = format!(
            "/sys/fs/cgroup/system.slice/generated-observability-worker-{index:06}-with-long-synthetic-name.scope/cgroup.procs"
        );
        let item = CgroupsSnapshotItem {
            hash: 2000 + index as u32,
            options: generated_cgroups_options(index),
            enabled: index % 2 == 0,
            name: name.as_bytes(),
            path: path.as_bytes(),
        };
        builder.push_item(&item).unwrap_or_else(|e| {
            eprintln!("snapshot builder item {index} failed: {e}");
            process::exit(1);
        });
    }
    let payload_len = builder.finish().unwrap_or_else(|e| {
        eprintln!("snapshot builder finish failed: {e}");
        process::exit(1);
    });

    let header = MessageHeader {
        magic: MESSAGE_MAGIC,
        version: MESSAGE_VERSION,
        header_len: MESSAGE_HEADER_LEN as u16,
        kind: MESSAGE_KIND_RESPONSE,
        flags: MESSAGE_FLAG_BATCH,
        code: METHOD_CGROUPS_SNAPSHOT,
        transport_status: TRANSPORT_STATUS_OK,
        payload_len: payload_len as u32,
        item_count: item_count as u32,
        message_id,
    };
    let encoded_header = encode_message_header(&header).unwrap_or_else(|e| {
        eprintln!("encode cgroups response header failed: {e}");
        process::exit(1);
    });

    let mut message = Vec::with_capacity(MESSAGE_HEADER_LEN + payload_len);
    message.extend_from_slice(&encoded_header);
    message.extend_from_slice(&payload[..payload_len]);
    message
}

fn print_cgroups_cache(client: &CgroupsSnapshotClient) {
    let cache = client.cache();
    println!(
        "CGROUPS_CACHE\t{}\t{}\t{}",
        cache.generation,
        if cache.systemd_enabled { 1 } else { 0 },
        cache.items.len()
    );
    for (index, item) in cache.items.iter().enumerate() {
        println!(
            "ITEM\t{}\t{}\t{}\t{}\t{}\t{}",
            index,
            item.hash,
            item.options,
            if item.enabled { 1 } else { 0 },
            String::from_utf8_lossy(&item.name),
            String::from_utf8_lossy(&item.path),
        );
    }
}

fn validate_lookup(
    client: &CgroupsSnapshotClient,
    lookup_hash: u32,
    lookup_name: &str,
) -> io::Result<()> {
    match client.lookup(lookup_hash, lookup_name.as_bytes()) {
        Some(item)
            if item.hash == lookup_hash && item.name.as_slice() == lookup_name.as_bytes() =>
        {
            Ok(())
        }
        _ => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "expected lookup hit",
        )),
    }
}

fn validate_cgroups_snapshot_request(message: &[u8]) -> io::Result<MessageHeader> {
    let header = decode_message_header(message)?;
    if header.kind != MESSAGE_KIND_REQUEST
        || header.flags != 0
        || header.code != METHOD_CGROUPS_SNAPSHOT
        || header.transport_status != TRANSPORT_STATUS_OK
        || header.item_count != 1
        || header.payload_len as usize != CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN
        || MESSAGE_HEADER_LEN + CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN != message.len()
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid cgroups request envelope",
        ));
    }

    decode_cgroups_snapshot_request_view(&message[MESSAGE_HEADER_LEN..])?;
    Ok(header)
}

fn is_disconnect_error(err: &io::Error) -> bool {
    matches!(
        err.kind(),
        io::ErrorKind::BrokenPipe
            | io::ErrorKind::ConnectionReset
            | io::ErrorKind::NotConnected
            | io::ErrorKind::UnexpectedEof
    ) || matches!(err.raw_os_error(), Some(code) if code == libc::EPROTO || code == 109 || code == 232 || code == 233)
}

fn is_idle_timeout_error(err: &io::Error) -> bool {
    matches!(err.kind(), io::ErrorKind::TimedOut)
        || matches!(err.raw_os_error(), Some(code) if code == libc::ETIMEDOUT)
}

#[cfg(unix)]
fn run_cgroups_server_once(service_namespace: &str, service_name: &str, auth_token: u64) {
    let mut config = UdsSeqpacketConfig::new(service_namespace, service_name);
    config.supported_profiles = parse_env_u32("NETIPC_SUPPORTED_PROFILES", PROFILE_UDS_SEQPACKET);
    config.preferred_profiles = parse_env_u32("NETIPC_PREFERRED_PROFILES", PROFILE_UDS_SEQPACKET);
    config.max_request_payload_bytes = CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN as u32;
    config.max_request_batch_items = 1;
    config.max_response_payload_bytes = netipc::CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES;
    config.max_response_batch_items = netipc::CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_BATCH_ITEMS;
    config.auth_token = auth_token;

    let mut server = UdsSeqpacketServer::bind(&config).unwrap_or_else(|e| {
        eprintln!("server-once failed: {e}");
        process::exit(1);
    });
    server
        .accept(Some(Duration::from_secs(10)))
        .unwrap_or_else(|e| {
            eprintln!("server-once failed: {e}");
            process::exit(1);
        });

    let mut request = vec![0u8; request_message_capacity()];
    let request_len = server
        .receive_message(&mut request, Some(Duration::from_secs(10)))
        .unwrap_or_else(|e| {
            eprintln!("server-once failed: {e}");
            process::exit(1);
        });
    let header = validate_cgroups_snapshot_request(&request[..request_len]).unwrap_or_else(|e| {
        eprintln!("server-once failed: {e}");
        process::exit(1);
    });
    server
        .send_message(
            &fixed_cgroups_response(header.message_id),
            Some(Duration::from_secs(10)),
        )
        .unwrap_or_else(|e| {
            eprintln!("server-once failed: {e}");
            process::exit(1);
        });
    println!(
        "CGROUPS_SERVER\t{}\t{}",
        header.message_id,
        configured_cgroups_item_count()
    );
}

#[cfg(windows)]
fn run_cgroups_server_once(service_namespace: &str, service_name: &str, auth_token: u64) {
    let mut config = NamedPipeConfig::new(service_namespace, service_name);
    config.supported_profiles = parse_env_u32("NETIPC_SUPPORTED_PROFILES", PROFILE_NAMED_PIPE);
    config.preferred_profiles = parse_env_u32("NETIPC_PREFERRED_PROFILES", PROFILE_NAMED_PIPE);
    config.max_request_payload_bytes = CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN as u32;
    config.max_request_batch_items = 1;
    config.max_response_payload_bytes = netipc::CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES;
    config.max_response_batch_items = netipc::CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_BATCH_ITEMS;
    config.auth_token = auth_token;

    let mut server = NamedPipeServer::bind(&config).unwrap_or_else(|e| {
        eprintln!("server-once failed: {e}");
        process::exit(1);
    });
    server
        .accept(Some(Duration::from_secs(10)))
        .unwrap_or_else(|e| {
            eprintln!("server-once failed: {e}");
            process::exit(1);
        });

    let mut request = vec![0u8; request_message_capacity()];
    let request_len = server
        .receive_message(&mut request, Some(Duration::from_secs(10)))
        .unwrap_or_else(|e| {
            eprintln!("server-once failed: {e}");
            process::exit(1);
        });
    let header = validate_cgroups_snapshot_request(&request[..request_len]).unwrap_or_else(|e| {
        eprintln!("server-once failed: {e}");
        process::exit(1);
    });
    server
        .send_message(&fixed_cgroups_response(header.message_id))
        .unwrap_or_else(|e| {
            eprintln!("server-once failed: {e}");
            process::exit(1);
        });
    println!(
        "CGROUPS_SERVER\t{}\t{}",
        header.message_id,
        configured_cgroups_item_count()
    );
}

#[cfg(unix)]
fn run_cgroups_server_loop(
    service_namespace: &str,
    service_name: &str,
    max_requests: u64,
    auth_token: u64,
) {
    let mut config = UdsSeqpacketConfig::new(service_namespace, service_name);
    config.supported_profiles = parse_env_u32("NETIPC_SUPPORTED_PROFILES", PROFILE_UDS_SEQPACKET);
    config.preferred_profiles = parse_env_u32("NETIPC_PREFERRED_PROFILES", PROFILE_UDS_SEQPACKET);
    config.max_request_payload_bytes = CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN as u32;
    config.max_request_batch_items = 1;
    config.max_response_payload_bytes = netipc::CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES;
    config.max_response_batch_items = netipc::CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_BATCH_ITEMS;
    config.auth_token = auth_token;

    let mut server = UdsSeqpacketServer::bind(&config).unwrap_or_else(|e| {
        eprintln!("server-loop failed: {e}");
        process::exit(1);
    });
    server
        .accept(Some(Duration::from_secs(10)))
        .unwrap_or_else(|e| {
            eprintln!("server-loop failed: {e}");
            process::exit(1);
        });

    let mut request = vec![0u8; request_message_capacity()];
    let mut handled = 0u64;
    while max_requests == 0 || handled < max_requests {
        let request_len = match server.receive_message(&mut request, Some(server_receive_timeout()))
        {
            Ok(len) => len,
            Err(e) => {
                if is_disconnect_error(&e) {
                    break;
                }
                if max_requests == 0 && handled > 0 && is_idle_timeout_error(&e) {
                    break;
                }
                eprintln!("server-loop failed: {e}");
                process::exit(1);
            }
        };
        let header =
            validate_cgroups_snapshot_request(&request[..request_len]).unwrap_or_else(|e| {
                eprintln!("server-loop failed: {e}");
                process::exit(1);
            });
        server
            .send_message(
                &fixed_cgroups_response(header.message_id),
                Some(Duration::from_secs(10)),
            )
            .unwrap_or_else(|e| {
                eprintln!("server-loop failed: {e}");
                process::exit(1);
            });
        handled += 1;
    }
    println!("CGROUPS_SERVER_LOOP\t{}", handled);
}

#[cfg(windows)]
fn run_cgroups_server_loop(
    service_namespace: &str,
    service_name: &str,
    max_requests: u64,
    auth_token: u64,
) {
    let mut config = NamedPipeConfig::new(service_namespace, service_name);
    config.supported_profiles = parse_env_u32("NETIPC_SUPPORTED_PROFILES", PROFILE_NAMED_PIPE);
    config.preferred_profiles = parse_env_u32("NETIPC_PREFERRED_PROFILES", PROFILE_NAMED_PIPE);
    config.max_request_payload_bytes = CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN as u32;
    config.max_request_batch_items = 1;
    config.max_response_payload_bytes = netipc::CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES;
    config.max_response_batch_items = netipc::CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_BATCH_ITEMS;
    config.auth_token = auth_token;

    let mut server = NamedPipeServer::bind(&config).unwrap_or_else(|e| {
        eprintln!("server-loop failed: {e}");
        process::exit(1);
    });
    server
        .accept(Some(Duration::from_secs(10)))
        .unwrap_or_else(|e| {
            eprintln!("server-loop failed: {e}");
            process::exit(1);
        });

    let mut request = vec![0u8; request_message_capacity()];
    let mut handled = 0u64;
    while max_requests == 0 || handled < max_requests {
        let request_len = match server.receive_message(&mut request, Some(server_receive_timeout()))
        {
            Ok(len) => len,
            Err(e) => {
                if is_disconnect_error(&e) {
                    break;
                }
                if max_requests == 0 && handled > 0 && is_idle_timeout_error(&e) {
                    break;
                }
                eprintln!("server-loop failed: {e}");
                process::exit(1);
            }
        };
        let header =
            validate_cgroups_snapshot_request(&request[..request_len]).unwrap_or_else(|e| {
                eprintln!("server-loop failed: {e}");
                process::exit(1);
            });
        server
            .send_message(&fixed_cgroups_response(header.message_id))
            .unwrap_or_else(|e| {
                eprintln!("server-loop failed: {e}");
                process::exit(1);
            });
        handled += 1;
    }
    println!("CGROUPS_SERVER_LOOP\t{}", handled);
}

fn client_refresh_once(
    service_namespace: &str,
    service_name: &str,
    lookup_hash: u32,
    lookup_name: &str,
    auth_token: u64,
) {
    let config = strict_cgroups_client_config(service_namespace, service_name, auth_token);
    let mut client = CgroupsSnapshotClient::new(config).unwrap_or_else(|e| {
        eprintln!("client-refresh-once failed: {e}");
        process::exit(1);
    });
    client
        .refresh(Some(Duration::from_secs(10)))
        .unwrap_or_else(|e| {
            eprintln!("client-refresh-once failed: {e}");
            process::exit(1);
        });

    print_cgroups_cache(&client);
    if let Some(item) = client.lookup(lookup_hash, lookup_name.as_bytes()) {
        println!(
            "LOOKUP\t{}\t{}\t{}\t{}\t{}",
            item.hash,
            item.options,
            if item.enabled { 1 } else { 0 },
            String::from_utf8_lossy(&item.name),
            String::from_utf8_lossy(&item.path),
        );
    } else {
        println!("LOOKUP_MISS\t{}\t{}", lookup_hash, lookup_name);
    }
}

fn client_refresh_loop(
    service_namespace: &str,
    service_name: &str,
    iterations: u64,
    lookup_hash: u32,
    lookup_name: &str,
    auth_token: u64,
) {
    let config = strict_cgroups_client_config(service_namespace, service_name, auth_token);
    let mut client = CgroupsSnapshotClient::new(config).unwrap_or_else(|e| {
        eprintln!("client-refresh-loop failed: {e}");
        process::exit(1);
    });
    for _ in 0..iterations {
        client
            .refresh(Some(Duration::from_secs(10)))
            .unwrap_or_else(|e| {
                eprintln!("client-refresh-loop failed: {e}");
                process::exit(1);
            });
    }

    println!("REFRESHES\t{}", iterations);
    print_cgroups_cache(&client);
    if let Some(item) = client.lookup(lookup_hash, lookup_name.as_bytes()) {
        println!(
            "LOOKUP\t{}\t{}\t{}\t{}\t{}",
            item.hash,
            item.options,
            if item.enabled { 1 } else { 0 },
            String::from_utf8_lossy(&item.name),
            String::from_utf8_lossy(&item.path),
        );
    } else {
        println!("LOOKUP_MISS\t{}\t{}", lookup_hash, lookup_name);
    }
}

fn run_cgroups_server_bench(
    service_namespace: &str,
    service_name: &str,
    max_requests: u64,
    auth_token: u64,
) {
    let start = Instant::now();
    let cpu_start = self_cpu_seconds();
    run_cgroups_server_loop(service_namespace, service_name, max_requests, auth_token);
    let elapsed_sec = start.elapsed().as_secs_f64();
    let server_cpu_cores = if elapsed_sec > 0.0 {
        (self_cpu_seconds() - cpu_start) / elapsed_sec
    } else {
        0.0
    };
    eprintln!("SERVER_CPU_CORES={server_cpu_cores:.3}");
}

fn strict_cgroups_client_config(
    service_namespace: &str,
    service_name: &str,
    auth_token: u64,
) -> CgroupsSnapshotClientConfig {
    let mut config = CgroupsSnapshotClientConfig::new(service_namespace, service_name);
    #[cfg(unix)]
    {
        config.supported_profiles =
            parse_env_u32("NETIPC_SUPPORTED_PROFILES", PROFILE_UDS_SEQPACKET);
        config.preferred_profiles =
            parse_env_u32("NETIPC_PREFERRED_PROFILES", PROFILE_UDS_SEQPACKET);
    }
    #[cfg(windows)]
    {
        config.supported_profiles = parse_env_u32("NETIPC_SUPPORTED_PROFILES", PROFILE_NAMED_PIPE);
        config.preferred_profiles = parse_env_u32("NETIPC_PREFERRED_PROFILES", PROFILE_NAMED_PIPE);
    }
    config.max_response_payload_bytes = parse_env_u32(
        "NETIPC_MAX_RESPONSE_PAYLOAD_BYTES",
        netipc::CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES,
    );
    config.max_response_batch_items = parse_env_u32(
        "NETIPC_MAX_RESPONSE_BATCH_ITEMS",
        netipc::CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_BATCH_ITEMS,
    );
    config.auth_token = auth_token;
    config
}

fn client_refresh_bench(
    service_namespace: &str,
    service_name: &str,
    duration_sec: i32,
    target_rps: i32,
    lookup_hash: u32,
    lookup_name: &str,
    auth_token: u64,
) {
    if duration_sec <= 0 {
        eprintln!("client-refresh-bench failed: duration_sec must be > 0");
        process::exit(2);
    }
    if target_rps < 0 {
        eprintln!("client-refresh-bench failed: target_rps must be >= 0");
        process::exit(2);
    }

    let config = strict_cgroups_client_config(service_namespace, service_name, auth_token);
    let mut client = CgroupsSnapshotClient::new(config).unwrap_or_else(|e| {
        eprintln!("client-refresh-bench failed: {e}");
        process::exit(1);
    });

    let start = Instant::now();
    let end_at = start + Duration::from_secs(duration_sec as u64);
    let cpu_start = self_cpu_seconds();
    let mut lat_ns = Vec::with_capacity(1024);
    let mut requests = 0u64;
    let mut responses = 0u64;

    while wait_for_benchmark_slot(start, end_at, target_rps, requests) {
        requests += 1;
        let call_start = Instant::now();
        client
            .refresh(Some(Duration::from_secs(10)))
            .unwrap_or_else(|e| {
                eprintln!("client-refresh-bench failed: {e}");
                process::exit(1);
            });
        validate_lookup(&client, lookup_hash, lookup_name).unwrap_or_else(|e| {
            eprintln!("client-refresh-bench failed: {e}");
            process::exit(1);
        });
        lat_ns.push(call_start.elapsed().as_nanos() as u64);
        responses += 1;
    }

    let elapsed_sec = start.elapsed().as_secs_f64();
    let result = BenchResult {
        duration_sec,
        target_rps,
        requests,
        responses,
        mismatches: 0,
        throughput_rps: if elapsed_sec > 0.0 {
            responses as f64 / elapsed_sec
        } else {
            0.0
        },
        p50_us: percentile_micros(&lat_ns, 50.0),
        p95_us: percentile_micros(&lat_ns, 95.0),
        p99_us: percentile_micros(&lat_ns, 99.0),
        client_cpu_cores: if elapsed_sec > 0.0 {
            (self_cpu_seconds() - cpu_start) / elapsed_sec
        } else {
            0.0
        },
    };

    print_bench_header();
    print_bench_row("rust-cgroups-refresh", &result);
}

fn client_lookup_bench(
    service_namespace: &str,
    service_name: &str,
    duration_sec: i32,
    target_rps: i32,
    lookup_hash: u32,
    lookup_name: &str,
    auth_token: u64,
) {
    if duration_sec <= 0 {
        eprintln!("client-lookup-bench failed: duration_sec must be > 0");
        process::exit(2);
    }
    if target_rps < 0 {
        eprintln!("client-lookup-bench failed: target_rps must be >= 0");
        process::exit(2);
    }

    let config = strict_cgroups_client_config(service_namespace, service_name, auth_token);
    let mut client = CgroupsSnapshotClient::new(config).unwrap_or_else(|e| {
        eprintln!("client-lookup-bench failed: {e}");
        process::exit(1);
    });
    client
        .refresh(Some(Duration::from_secs(10)))
        .unwrap_or_else(|e| {
            eprintln!("client-lookup-bench failed: {e}");
            process::exit(1);
        });
    validate_lookup(&client, lookup_hash, lookup_name).unwrap_or_else(|e| {
        eprintln!("client-lookup-bench failed: {e}");
        process::exit(1);
    });

    let start = Instant::now();
    let end_at = start + Duration::from_secs(duration_sec as u64);
    let cpu_start = self_cpu_seconds();
    let mut lat_ns = Vec::with_capacity(1024);
    let mut requests = 0u64;
    let mut responses = 0u64;

    while wait_for_benchmark_slot(start, end_at, target_rps, requests) {
        requests += 1;
        let call_start = Instant::now();
        validate_lookup(&client, lookup_hash, lookup_name).unwrap_or_else(|e| {
            eprintln!("client-lookup-bench failed: {e}");
            process::exit(1);
        });
        lat_ns.push(call_start.elapsed().as_nanos() as u64);
        responses += 1;
    }

    let elapsed_sec = start.elapsed().as_secs_f64();
    let result = BenchResult {
        duration_sec,
        target_rps,
        requests,
        responses,
        mismatches: 0,
        throughput_rps: if elapsed_sec > 0.0 {
            responses as f64 / elapsed_sec
        } else {
            0.0
        },
        p50_us: percentile_micros(&lat_ns, 50.0),
        p95_us: percentile_micros(&lat_ns, 95.0),
        p99_us: percentile_micros(&lat_ns, 99.0),
        client_cpu_cores: if elapsed_sec > 0.0 {
            (self_cpu_seconds() - cpu_start) / elapsed_sec
        } else {
            0.0
        },
    };

    print_bench_header();
    print_bench_row("rust-cgroups-lookup", &result);
}

fn usage(argv0: &str) {
    eprintln!("usage:");
    eprintln!("  {argv0} encode-req <request_id> <value> <out_file>");
    eprintln!("  {argv0} decode-req <in_file>");
    eprintln!("  {argv0} encode-resp <request_id> <status> <value> <out_file>");
    eprintln!("  {argv0} decode-resp <in_file>");
    eprintln!("  {argv0} serve-once <req_file> <resp_file>");
    eprintln!("  {argv0} encode-cgroups-req <message_id> <out_file>");
    eprintln!("  {argv0} decode-cgroups-req <in_file>");
    eprintln!("  {argv0} encode-cgroups-resp <message_id> <out_file>");
    eprintln!("  {argv0} decode-cgroups-resp <in_file>");
    eprintln!("  {argv0} serve-cgroups-once <req_file> <resp_file>");
    eprintln!("  {argv0} server-once <service_namespace> <service> [auth_token]");
    eprintln!("  {argv0} server-loop <service_namespace> <service> <max_requests|0> [auth_token]");
    eprintln!("  {argv0} server-bench <service_namespace> <service> <max_requests|0> [auth_token]");
    eprintln!(
        "  {argv0} client-refresh-once <service_namespace> <service> <lookup_hash> <lookup_name> [auth_token]"
    );
    eprintln!(
        "  {argv0} client-refresh-loop <service_namespace> <service> <iterations> <lookup_hash> <lookup_name> [auth_token]"
    );
    eprintln!(
        "  {argv0} client-refresh-bench <service_namespace> <service> <duration_sec> <target_rps> <lookup_hash> <lookup_name> [auth_token]"
    );
    eprintln!(
        "  {argv0} client-lookup-bench <service_namespace> <service> <duration_sec> <target_rps> <lookup_hash> <lookup_name> [auth_token]"
    );
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        usage(&args[0]);
        process::exit(2);
    }

    match args[1].as_str() {
        "encode-req" => {
            if args.len() != 5 {
                usage(&args[0]);
                process::exit(2);
            }
            let request_id = parse_u64(&args[2]);
            let value = parse_u64(&args[3]);
            let frame = encode_increment_request(request_id, &IncrementRequest { value });
            write_frame(&args[4], &frame);
        }
        "decode-req" => {
            if args.len() != 3 {
                usage(&args[0]);
                process::exit(2);
            }
            let frame = read_frame(&args[2]);
            let (request_id, request) = decode_increment_request(&frame).unwrap_or_else(|e| {
                eprintln!("decode-req failed: {e}");
                process::exit(1);
            });
            println!("REQ {request_id} {}", request.value);
        }
        "encode-resp" => {
            if args.len() != 6 {
                usage(&args[0]);
                process::exit(2);
            }
            let request_id = parse_u64(&args[2]);
            let status = parse_i32(&args[3]);
            let value = parse_u64(&args[4]);
            let frame = encode_increment_response(request_id, &IncrementResponse { status, value });
            write_frame(&args[5], &frame);
        }
        "decode-resp" => {
            if args.len() != 3 {
                usage(&args[0]);
                process::exit(2);
            }
            let frame = read_frame(&args[2]);
            let (request_id, response) = decode_increment_response(&frame).unwrap_or_else(|e| {
                eprintln!("decode-resp failed: {e}");
                process::exit(1);
            });
            println!("RESP {request_id} {} {}", response.status, response.value);
        }
        "serve-once" => {
            if args.len() != 4 {
                usage(&args[0]);
                process::exit(2);
            }
            let frame = read_frame(&args[2]);
            let (request_id, request) = decode_increment_request(&frame).unwrap_or_else(|e| {
                eprintln!("serve-once decode failed: {e}");
                process::exit(1);
            });
            let response = IncrementResponse {
                status: STATUS_OK,
                value: request.value + 1,
            };
            let response_frame = encode_increment_response(request_id, &response);
            write_frame(&args[3], &response_frame);
        }
        "encode-cgroups-req" => {
            if args.len() != 4 {
                usage(&args[0]);
                process::exit(2);
            }
            let message_id = parse_u64(&args[2]);
            let payload =
                encode_cgroups_snapshot_request_payload(&CgroupsSnapshotRequest { flags: 0 });
            let header = MessageHeader {
                magic: MESSAGE_MAGIC,
                version: MESSAGE_VERSION,
                header_len: MESSAGE_HEADER_LEN as u16,
                kind: MESSAGE_KIND_REQUEST,
                flags: 0,
                code: METHOD_CGROUPS_SNAPSHOT,
                transport_status: TRANSPORT_STATUS_OK,
                payload_len: CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN as u32,
                item_count: 1,
                message_id,
            };
            let encoded_header = encode_message_header(&header).unwrap_or_else(|e| {
                eprintln!("encode-cgroups-req failed: {e}");
                process::exit(1);
            });
            let mut message = Vec::with_capacity(MESSAGE_HEADER_LEN + payload.len());
            message.extend_from_slice(&encoded_header);
            message.extend_from_slice(&payload);
            write_bytes(&args[3], &message);
        }
        "decode-cgroups-req" => {
            if args.len() != 3 {
                usage(&args[0]);
                process::exit(2);
            }
            let message = read_bytes(&args[2]);
            let header = decode_message_header(&message).unwrap_or_else(|e| {
                eprintln!("decode-cgroups-req failed: {e}");
                process::exit(1);
            });
            if header.kind != MESSAGE_KIND_REQUEST
                || header.code != METHOD_CGROUPS_SNAPSHOT
                || header.transport_status != TRANSPORT_STATUS_OK
                || header.flags != 0
                || header.item_count != 1
                || header.payload_len as usize != CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN
            {
                eprintln!("decode-cgroups-req failed: invalid header");
                process::exit(1);
            }
            let view = decode_cgroups_snapshot_request_view(&message[MESSAGE_HEADER_LEN..])
                .unwrap_or_else(|e| {
                    eprintln!("decode-cgroups-req failed: {e}");
                    process::exit(1);
                });
            println!("CGROUPS_REQ\t{}\t{}", header.message_id, view.flags);
        }
        "encode-cgroups-resp" => {
            if args.len() != 4 {
                usage(&args[0]);
                process::exit(2);
            }
            write_bytes(&args[3], &fixed_cgroups_response(parse_u64(&args[2])));
        }
        "decode-cgroups-resp" => {
            if args.len() != 3 {
                usage(&args[0]);
                process::exit(2);
            }
            let message = read_bytes(&args[2]);
            let header = decode_message_header(&message).unwrap_or_else(|e| {
                eprintln!("decode-cgroups-resp failed: {e}");
                process::exit(1);
            });
            if header.kind != MESSAGE_KIND_RESPONSE
                || header.code != METHOD_CGROUPS_SNAPSHOT
                || header.transport_status != TRANSPORT_STATUS_OK
                || header.flags != MESSAGE_FLAG_BATCH
            {
                eprintln!("decode-cgroups-resp failed: invalid header");
                process::exit(1);
            }
            let view =
                decode_cgroups_snapshot_view(&message[MESSAGE_HEADER_LEN..], header.item_count)
                    .unwrap_or_else(|e| {
                        eprintln!("decode-cgroups-resp failed: {e}");
                        process::exit(1);
                    });
            println!(
                "CGROUPS_RESP\t{}\t{}\t{}\t{}",
                header.message_id,
                view.generation,
                if view.systemd_enabled { 1 } else { 0 },
                view.item_count
            );
            for index in 0..view.item_count {
                let item = view.item_view_at(index).unwrap_or_else(|e| {
                    eprintln!("decode-cgroups-resp failed: {e}");
                    process::exit(1);
                });
                println!(
                    "ITEM\t{}\t{}\t{}\t{}\t{}\t{}",
                    index,
                    item.hash,
                    item.options,
                    if item.enabled { 1 } else { 0 },
                    item.name_view.as_bytes().escape_ascii(),
                    item.path_view.as_bytes().escape_ascii(),
                );
            }
        }
        "serve-cgroups-once" => {
            if args.len() != 4 {
                usage(&args[0]);
                process::exit(2);
            }
            let message = read_bytes(&args[2]);
            let header = decode_message_header(&message).unwrap_or_else(|e| {
                eprintln!("serve-cgroups-once decode failed: {e}");
                process::exit(1);
            });
            if header.kind != MESSAGE_KIND_REQUEST
                || header.code != METHOD_CGROUPS_SNAPSHOT
                || header.transport_status != TRANSPORT_STATUS_OK
                || header.flags != 0
                || header.item_count != 1
                || header.payload_len as usize != CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN
            {
                eprintln!("serve-cgroups-once decode failed: invalid header");
                process::exit(1);
            }
            decode_cgroups_snapshot_request_view(&message[MESSAGE_HEADER_LEN..]).unwrap_or_else(
                |e| {
                    eprintln!("serve-cgroups-once decode failed: {e}");
                    process::exit(1);
                },
            );
            write_bytes(&args[3], &fixed_cgroups_response(header.message_id));
        }
        "server-once" => {
            if args.len() != 4 && args.len() != 5 {
                usage(&args[0]);
                process::exit(2);
            }
            let auth_token = if args.len() == 5 {
                parse_u64(&args[4])
            } else {
                0
            };
            run_cgroups_server_once(&args[2], &args[3], auth_token);
        }
        "server-loop" => {
            if args.len() != 5 && args.len() != 6 {
                usage(&args[0]);
                process::exit(2);
            }
            let auth_token = if args.len() == 6 {
                parse_u64(&args[5])
            } else {
                0
            };
            run_cgroups_server_loop(&args[2], &args[3], parse_u64(&args[4]), auth_token);
        }
        "server-bench" => {
            if args.len() != 5 && args.len() != 6 {
                usage(&args[0]);
                process::exit(2);
            }
            let auth_token = if args.len() == 6 {
                parse_u64(&args[5])
            } else {
                0
            };
            run_cgroups_server_bench(&args[2], &args[3], parse_u64(&args[4]), auth_token);
        }
        "client-refresh-once" => {
            if args.len() != 6 && args.len() != 7 {
                usage(&args[0]);
                process::exit(2);
            }
            let auth_token = if args.len() == 7 {
                parse_u64(&args[6])
            } else {
                0
            };
            client_refresh_once(
                &args[2],
                &args[3],
                parse_u32(&args[4]),
                &args[5],
                auth_token,
            );
        }
        "client-refresh-loop" => {
            if args.len() != 7 && args.len() != 8 {
                usage(&args[0]);
                process::exit(2);
            }
            let auth_token = if args.len() == 8 {
                parse_u64(&args[7])
            } else {
                0
            };
            client_refresh_loop(
                &args[2],
                &args[3],
                parse_u64(&args[4]),
                parse_u32(&args[5]),
                &args[6],
                auth_token,
            );
        }
        "client-refresh-bench" => {
            if args.len() != 8 && args.len() != 9 {
                usage(&args[0]);
                process::exit(2);
            }
            let auth_token = if args.len() == 9 {
                parse_u64(&args[8])
            } else {
                0
            };
            client_refresh_bench(
                &args[2],
                &args[3],
                parse_u64(&args[4]) as i32,
                parse_u64(&args[5]) as i32,
                parse_u32(&args[6]),
                &args[7],
                auth_token,
            );
        }
        "client-lookup-bench" => {
            if args.len() != 8 && args.len() != 9 {
                usage(&args[0]);
                process::exit(2);
            }
            let auth_token = if args.len() == 9 {
                parse_u64(&args[8])
            } else {
                0
            };
            client_lookup_bench(
                &args[2],
                &args[3],
                parse_u64(&args[4]) as i32,
                parse_u64(&args[5]) as i32,
                parse_u32(&args[6]),
                &args[7],
                auth_token,
            );
        }
        _ => {
            usage(&args[0]);
            process::exit(2);
        }
    }
}
