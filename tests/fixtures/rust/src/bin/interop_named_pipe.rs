//! Simple server/client for cross-language Named Pipe interop tests.
//!
//! Usage:
//!   interop_named_pipe server <run_dir> <service_name>
//!     Listens, accepts 1 client, echoes 1 message, exits.
//!
//!   interop_named_pipe client <run_dir> <service_name>
//!     Connects, sends 1 message, verifies echo, exits 0 on success.

#[cfg(windows)]
use netipc::protocol::{self, Header};
#[cfg(windows)]
use netipc::transport::windows::{ClientConfig, NpListener, NpSession, ServerConfig};

#[cfg(windows)]
const AUTH_TOKEN: u64 = 0xDEADBEEFCAFEBABE;

#[cfg(windows)]
fn server_config() -> ServerConfig {
    ServerConfig {
        supported_profiles: protocol::PROFILE_BASELINE,
        max_request_payload_bytes: 65536,
        max_request_batch_items: 16,
        max_response_payload_bytes: 65536,
        max_response_batch_items: 16,
        auth_token: AUTH_TOKEN,
        ..ServerConfig::default()
    }
}

#[cfg(windows)]
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

#[cfg(windows)]
fn run_server(run_dir: &str, service: &str) -> Result<(), Box<dyn std::error::Error>> {
    let mut listener = NpListener::bind(run_dir, service, server_config())?;

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

    session.close();
    listener.close();
    Ok(())
}

#[cfg(windows)]
fn run_client(run_dir: &str, service: &str) -> Result<(), Box<dyn std::error::Error>> {
    let mut session = NpSession::connect(run_dir, service, &client_config())?;

    // Build payload with known pattern
    let mut payload = [0u8; 256];
    for i in 0..payload.len() {
        payload[i] = (i & 0xFF) as u8;
    }

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
    if rpayload.len() != payload.len() {
        eprintln!(
            "client: payload length mismatch: {} vs {}",
            rpayload.len(),
            payload.len()
        );
        ok = false;
    }
    if rpayload.len() == payload.len() && rpayload != payload {
        eprintln!("client: payload data mismatch");
        ok = false;
    }

    session.close();

    if ok {
        println!("PASS");
    } else {
        println!("FAIL");
    }

    if ok { Ok(()) } else { Err("interop failed".into()) }
}

fn main() {
    #[cfg(not(windows))]
    {
        eprintln!("Named Pipe interop not supported on this platform");
        std::process::exit(1);
    }

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

        let result = match mode.as_str() {
            "server" => run_server(run_dir, service),
            "client" => run_client(run_dir, service),
            _ => {
                eprintln!("Unknown mode: {mode}");
                std::process::exit(1);
            }
        };

        if let Err(e) = result {
            eprintln!("Error: {e}");
            std::process::exit(1);
        }
    }
}
