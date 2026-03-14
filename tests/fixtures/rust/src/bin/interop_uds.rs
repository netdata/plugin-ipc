//! Simple server/client for cross-language UDS interop tests.
//!
//! Usage:
//!   interop_uds server <run_dir> <service_name>
//!     Listens, accepts 1 client, echoes 1 message, exits.
//!
//!   interop_uds client <run_dir> <service_name>
//!     Connects, sends 1 message, verifies echo, exits 0 on success.

use netipc::protocol::{self, Header};
use netipc::transport::posix::{ClientConfig, ServerConfig, UdsListener, UdsSession};

const AUTH_TOKEN: u64 = 0xDEADBEEFCAFEBABE;

fn server_config() -> ServerConfig {
    ServerConfig {
        supported_profiles: protocol::PROFILE_BASELINE,
        max_request_payload_bytes: 65536,
        max_request_batch_items: 16,
        max_response_payload_bytes: 65536,
        max_response_batch_items: 16,
        auth_token: AUTH_TOKEN,
        backlog: 4,
        ..ServerConfig::default()
    }
}

fn client_config() -> ClientConfig {
    ClientConfig {
        supported_profiles: protocol::PROFILE_BASELINE,
        max_request_payload_bytes: 65536,
        max_request_batch_items: 16,
        max_response_payload_bytes: 65536,
        max_response_batch_items: 16,
        auth_token: AUTH_TOKEN,
        ..ClientConfig::default()
    }
}

fn run_server(run_dir: &str, service: &str) -> Result<(), Box<dyn std::error::Error>> {
    let listener = UdsListener::bind(run_dir, service, server_config())?;

    // Signal readiness
    println!("READY");

    let mut session = listener.accept()?;

    // Receive one message
    let mut buf = [0u8; 65600];
    let (hdr, payload) = session.receive(&mut buf)?;

    // Echo as response
    let mut resp = hdr;
    resp.kind = protocol::KIND_RESPONSE;
    resp.transport_status = protocol::STATUS_OK;
    session.send(&mut resp, &payload)?;

    Ok(())
}

fn run_client(run_dir: &str, service: &str) -> Result<(), Box<dyn std::error::Error>> {
    let mut session = UdsSession::connect(run_dir, service, &client_config())?;

    // Build payload with known pattern
    let payload: Vec<u8> = (0..256).map(|i| (i & 0xFF) as u8).collect();

    let mut hdr = Header {
        kind: protocol::KIND_REQUEST,
        code: protocol::METHOD_INCREMENT,
        flags: 0,
        item_count: 1,
        message_id: 12345,
        ..Header::default()
    };

    session.send(&mut hdr, &payload)?;

    // Receive response
    let mut rbuf = [0u8; 65600];
    let (rhdr, rpayload) = session.receive(&mut rbuf)?;

    // Verify
    let mut ok = true;
    if rhdr.kind != protocol::KIND_RESPONSE {
        eprintln!("client: expected RESPONSE, got {}", rhdr.kind);
        ok = false;
    }
    if rhdr.message_id != 12345 {
        eprintln!("client: expected message_id 12345, got {}", rhdr.message_id);
        ok = false;
    }
    if rpayload.len() != 256 {
        eprintln!(
            "client: payload length mismatch: {} vs 256",
            rpayload.len()
        );
        ok = false;
    }
    if rpayload != payload {
        eprintln!("client: payload data mismatch");
        ok = false;
    }

    if ok {
        println!("PASS");
    } else {
        println!("FAIL");
    }

    if ok {
        Ok(())
    } else {
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
