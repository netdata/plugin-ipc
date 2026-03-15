//! Simple server/client for cross-language SHM interop tests.
//!
//! Usage:
//!   interop_shm server <run_dir> <service_name>
//!     Creates SHM region, receives 1 message, echoes it, exits.
//!
//!   interop_shm client <run_dir> <service_name>
//!     Attaches to SHM, sends 1 message, verifies echo, exits 0 on success.

use netipc::protocol::{self, Header};
use netipc::transport::shm::ShmContext;

fn build_message(kind: u16, code: u16, message_id: u64, payload: &[u8]) -> Vec<u8> {
    let hdr = Header {
        magic: protocol::MAGIC_MSG,
        version: protocol::VERSION,
        header_len: protocol::HEADER_LEN,
        kind,
        code,
        flags: 0,
        transport_status: protocol::STATUS_OK,
        payload_len: payload.len() as u32,
        item_count: 1,
        message_id,
    };
    let mut buf = vec![0u8; protocol::HEADER_SIZE + payload.len()];
    hdr.encode(&mut buf[..protocol::HEADER_SIZE]);
    buf[protocol::HEADER_SIZE..].copy_from_slice(payload);
    buf
}

fn run_server(run_dir: &str, service: &str) -> Result<(), Box<dyn std::error::Error>> {
    let mut ctx = ShmContext::server_create(run_dir, service, 1, 65536, 65536)?;

    // Signal readiness
    println!("READY");

    // Receive one message
    let mut buf = vec![0u8; 65536];
    let mlen = ctx.receive(&mut buf, 10000)?;

    if mlen < protocol::HEADER_SIZE {
        return Err("message too short".into());
    }

    // Parse and echo as response
    let hdr = Header::decode(&buf[..mlen])?;
    let payload_copy: Vec<u8> = buf[protocol::HEADER_SIZE..mlen].to_vec();
    let resp = build_message(
        protocol::KIND_RESPONSE,
        hdr.code,
        hdr.message_id,
        &payload_copy,
    );
    ctx.send(&resp)?;

    ctx.destroy();
    Ok(())
}

fn run_client(run_dir: &str, service: &str) -> Result<(), Box<dyn std::error::Error>> {
    // Retry attach -- server may not be fully ready yet
    let mut ctx = None;
    for _ in 0..500 {
        match ShmContext::client_attach(run_dir, service, 1) {
            Ok(c) => {
                ctx = Some(c);
                break;
            }
            Err(netipc::transport::shm::ShmError::NotReady)
            | Err(netipc::transport::shm::ShmError::Open(_))
            | Err(netipc::transport::shm::ShmError::BadMagic) => {
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
            Err(e) => return Err(e.into()),
        }
    }
    let mut ctx = ctx.ok_or("client: attach failed after retries")?;

    // Build payload with known pattern
    let payload: Vec<u8> = (0..256).map(|i| (i & 0xFF) as u8).collect();
    let msg = build_message(
        protocol::KIND_REQUEST,
        protocol::METHOD_INCREMENT,
        12345,
        &payload,
    );
    ctx.send(&msg)?;

    // Receive response
    let mut resp_buf = vec![0u8; 65536];
    let rlen = ctx.receive(&mut resp_buf, 10000)?;
    let resp_copy: Vec<u8> = resp_buf[..rlen].to_vec();

    let mut ok = true;
    if resp_copy.len() < protocol::HEADER_SIZE {
        eprintln!("client: response too short");
        ok = false;
    } else {
        let rhdr = Header::decode(&resp_copy)?;
        if rhdr.kind != protocol::KIND_RESPONSE {
            eprintln!("client: expected RESPONSE, got {}", rhdr.kind);
            ok = false;
        }
        if rhdr.message_id != 12345 {
            eprintln!("client: expected message_id 12345, got {}", rhdr.message_id);
            ok = false;
        }
        let resp_payload = &resp_copy[protocol::HEADER_SIZE..];
        if resp_payload.len() != 256 {
            eprintln!(
                "client: payload length mismatch: {} vs 256",
                resp_payload.len()
            );
            ok = false;
        }
        if resp_payload != &payload[..] {
            eprintln!("client: payload data mismatch");
            ok = false;
        }
    }

    ctx.close();

    if ok {
        println!("PASS");
        Ok(())
    } else {
        println!("FAIL");
        std::process::exit(1);
    }
}

fn main() {
    // Ignore SIGPIPE
    unsafe {
        libc::signal(libc::SIGPIPE, libc::SIG_IGN);
    }

    let args: Vec<String> = std::env::args().collect();
    if args.len() != 4 {
        eprintln!("Usage: {} <server|client> <run_dir> <service_name>", args[0]);
        std::process::exit(1);
    }

    let mode = &args[1];
    let run_dir = &args[2];
    let service = &args[3];

    // Ensure run_dir exists
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
