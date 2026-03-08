use netipc::transport::posix::{ShmClient, ShmConfig, ShmServer};
use netipc::{IncrementRequest, IncrementResponse, STATUS_OK};
use std::env;
use std::io;
use std::process;
use std::time::Duration;

fn usage(argv0: &str) {
    eprintln!("usage:");
    eprintln!("  {argv0} server-once <run_dir> <service>");
    eprintln!("  {argv0} client-once <run_dir> <service> <value>");
}

fn parse_u64(v: &str) -> u64 {
    v.parse::<u64>().unwrap_or_else(|_| {
        eprintln!("invalid u64: {v}");
        process::exit(2);
    })
}

fn protocol_error(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
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
        "RUST-SERVER request_id={request_id} value={} response={}",
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

    println!("RUST-CLIENT request={value} response={}", response.value);
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
