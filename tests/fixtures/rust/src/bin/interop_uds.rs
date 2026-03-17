//! Simple server/client for cross-language UDS interop tests.
//!
//! Usage:
//!   interop_uds server <run_dir> <service_name>
//!     Listens, accepts 1 client, echoes 1 message, exits.
//!
//!   interop_uds client <run_dir> <service_name>
//!     Connects, sends 1 message, verifies echo, exits 0 on success.

#[cfg(windows)]
fn main() {
    eprintln!("interop_uds is only supported on POSIX platforms");
    std::process::exit(1);
}

#[cfg(not(windows))]
mod posix_only {

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
        let payload = payload.to_vec();

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
            eprintln!("client: payload length mismatch: {} vs 256", rpayload.len());
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

    fn run_pipeline_server(
        run_dir: &str,
        service: &str,
        count: usize,
    ) -> Result<(), Box<dyn std::error::Error>> {
        let listener = UdsListener::bind(run_dir, service, server_config())?;

        println!("READY");

        let mut session = listener.accept()?;

        for i in 0..count {
            let mut buf = [0u8; 65600];
            let (hdr, payload) = session
                .receive(&mut buf)
                .map_err(|e| format!("receive[{i}]: {e}"))?;
            let payload = payload.to_vec();

            let mut resp = hdr;
            resp.kind = protocol::KIND_RESPONSE;
            resp.transport_status = protocol::STATUS_OK;
            session
                .send(&mut resp, &payload)
                .map_err(|e| format!("send[{i}]: {e}"))?;
        }

        Ok(())
    }

    fn run_pipeline_client(
        run_dir: &str,
        service: &str,
        count: usize,
    ) -> Result<(), Box<dyn std::error::Error>> {
        let mut session = UdsSession::connect(run_dir, service, &client_config())?;

        // Send all requests before reading any response
        for i in 0..count {
            let val = (i + 1) as u64;
            let payload = val.to_ne_bytes();

            let mut hdr = Header {
                kind: protocol::KIND_REQUEST,
                code: protocol::METHOD_INCREMENT,
                flags: 0,
                item_count: 1,
                message_id: val,
                ..Header::default()
            };

            session.send(&mut hdr, &payload)?;
        }

        // Read all responses and verify
        let mut ok = true;
        for i in 0..count {
            let mut rbuf = [0u8; 65600];
            let (rhdr, rpayload) = session.receive(&mut rbuf)?;

            let expected = (i + 1) as u64;
            if rhdr.kind != protocol::KIND_RESPONSE {
                eprintln!("client: [{i}] expected RESPONSE, got {}", rhdr.kind);
                ok = false;
            }
            if rhdr.message_id != expected {
                eprintln!(
                    "client: [{i}] message_id {}, want {expected}",
                    rhdr.message_id
                );
                ok = false;
            }
            if rpayload.len() != 8 {
                eprintln!("client: [{i}] payload len {}, want 8", rpayload.len());
                ok = false;
            } else {
                let val = u64::from_ne_bytes(rpayload.try_into().unwrap());
                if val != expected {
                    eprintln!("client: [{i}] payload {val}, want {expected}");
                    ok = false;
                }
            }
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

    pub(crate) fn main() {
        // Ignore SIGPIPE
        unsafe {
            libc::signal(libc::SIGPIPE, libc::SIG_IGN);
        }

        let args: Vec<String> = std::env::args().collect();
        if args.len() < 4 {
            eprintln!(
            "Usage:\n  {} server <run_dir> <service>\n  {} client <run_dir> <service>\n  {} pipeline-server <run_dir> <service> <count>\n  {} pipeline-client <run_dir> <service> <count>",
            args[0], args[0], args[0], args[0]
        );
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
            "pipeline-server" | "pipeline-client" => {
                if args.len() < 5 {
                    eprintln!("{mode} requires <count> argument");
                    std::process::exit(1);
                }
                let count: usize = args[4].parse().unwrap_or_else(|_| {
                    eprintln!("invalid count: {}", args[4]);
                    std::process::exit(1);
                });
                if mode == "pipeline-server" {
                    run_pipeline_server(run_dir, service, count)
                } else {
                    run_pipeline_client(run_dir, service, count)
                }
            }
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
}

#[cfg(not(windows))]
fn main() {
    posix_only::main();
}
