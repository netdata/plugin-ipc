package main

import (
	"fmt"
	"os"
	"strconv"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

func usage(argv0 string) {
	fmt.Fprintf(os.Stderr, "usage:\n")
	fmt.Fprintf(os.Stderr, "  %s encode-req <request_id> <value> <out_file>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s decode-req <in_file>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s encode-resp <request_id> <status> <value> <out_file>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s decode-resp <in_file>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s serve-once <req_file> <resp_file>\n", argv0)
}

func mustParseU64(s string) uint64 {
	v, err := strconv.ParseUint(s, 10, 64)
	if err != nil {
		fmt.Fprintf(os.Stderr, "invalid u64 %q: %v\n", s, err)
		os.Exit(2)
	}
	return v
}

func mustParseI32(s string) int32 {
	v, err := strconv.ParseInt(s, 10, 32)
	if err != nil {
		fmt.Fprintf(os.Stderr, "invalid i32 %q: %v\n", s, err)
		os.Exit(2)
	}
	return int32(v)
}

func readFrame(path string) protocol.Frame {
	data, err := os.ReadFile(path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "read failed for %s: %v\n", path, err)
		os.Exit(1)
	}
	if len(data) != protocol.FrameSize {
		fmt.Fprintf(os.Stderr, "invalid frame size in %s: %d\n", path, len(data))
		os.Exit(1)
	}
	var frame protocol.Frame
	copy(frame[:], data)
	return frame
}

func writeFrame(path string, frame protocol.Frame) {
	if err := os.WriteFile(path, frame[:], 0o644); err != nil {
		fmt.Fprintf(os.Stderr, "write failed for %s: %v\n", path, err)
		os.Exit(1)
	}
}

func main() {
	args := os.Args
	if len(args) < 2 {
		usage(args[0])
		os.Exit(2)
	}

	switch args[1] {
	case "encode-req":
		if len(args) != 5 {
			usage(args[0])
			os.Exit(2)
		}
		frame := protocol.EncodeIncrementRequest(
			mustParseU64(args[2]),
			protocol.IncrementRequest{Value: mustParseU64(args[3])},
		)
		writeFrame(args[4], frame)

	case "decode-req":
		if len(args) != 3 {
			usage(args[0])
			os.Exit(2)
		}
		requestID, request, err := protocol.DecodeIncrementRequest(readFrame(args[2]))
		if err != nil {
			fmt.Fprintf(os.Stderr, "decode-req failed: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("REQ %d %d\n", requestID, request.Value)

	case "encode-resp":
		if len(args) != 6 {
			usage(args[0])
			os.Exit(2)
		}
		frame := protocol.EncodeIncrementResponse(
			mustParseU64(args[2]),
			protocol.IncrementResponse{Status: mustParseI32(args[3]), Value: mustParseU64(args[4])},
		)
		writeFrame(args[5], frame)

	case "decode-resp":
		if len(args) != 3 {
			usage(args[0])
			os.Exit(2)
		}
		requestID, response, err := protocol.DecodeIncrementResponse(readFrame(args[2]))
		if err != nil {
			fmt.Fprintf(os.Stderr, "decode-resp failed: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("RESP %d %d %d\n", requestID, response.Status, response.Value)

	case "serve-once":
		if len(args) != 4 {
			usage(args[0])
			os.Exit(2)
		}
		requestID, request, err := protocol.DecodeIncrementRequest(readFrame(args[2]))
		if err != nil {
			fmt.Fprintf(os.Stderr, "serve-once decode failed: %v\n", err)
			os.Exit(1)
		}
		response := protocol.IncrementResponse{Status: protocol.StatusOK, Value: request.Value + 1}
		writeFrame(args[3], protocol.EncodeIncrementResponse(requestID, response))

	default:
		usage(args[0])
		os.Exit(2)
	}
}
