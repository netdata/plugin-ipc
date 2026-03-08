use netipc::{
    decode_increment_request, decode_increment_response, encode_increment_request,
    encode_increment_response, Frame, IncrementRequest, IncrementResponse, FRAME_SIZE, STATUS_OK,
};
use std::env;
use std::fs;
use std::process;

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

fn read_frame(path: &str) -> Frame {
    let data = fs::read(path).unwrap_or_else(|e| {
        eprintln!("read failed for {path}: {e}");
        process::exit(1);
    });

    if data.len() != FRAME_SIZE {
        eprintln!("invalid frame size in {path}: {}", data.len());
        process::exit(1);
    }

    let mut frame = [0u8; FRAME_SIZE];
    frame.copy_from_slice(&data);
    frame
}

fn write_frame(path: &str, frame: &Frame) {
    fs::write(path, frame).unwrap_or_else(|e| {
        eprintln!("write failed for {path}: {e}");
        process::exit(1);
    });
}

fn usage(argv0: &str) {
    eprintln!("usage:");
    eprintln!("  {argv0} encode-req <request_id> <value> <out_file>");
    eprintln!("  {argv0} decode-req <in_file>");
    eprintln!("  {argv0} encode-resp <request_id> <status> <value> <out_file>");
    eprintln!("  {argv0} decode-resp <in_file>");
    eprintln!("  {argv0} serve-once <req_file> <resp_file>");
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
        _ => {
            usage(&args[0]);
            process::exit(2);
        }
    }
}
