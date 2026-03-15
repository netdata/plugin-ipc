//go:build unix

// Simple server/client for cross-language UDS interop tests.
//
// Usage:
//
//	interop_uds server <run_dir> <service_name>
//	  Listens, accepts 1 client, echoes 1 message, exits.
//
//	interop_uds client <run_dir> <service_name>
//	  Connects, sends 1 message, verifies echo, exits 0 on success.
package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"os"
	"os/signal"
	"strconv"
	"syscall"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix"
)

const authToken uint64 = 0xDEADBEEFCAFEBABE

func serverConfig() posix.ServerConfig {
	return posix.ServerConfig{
		SupportedProfiles:       protocol.ProfileBaseline,
		MaxRequestPayloadBytes:  65536,
		MaxRequestBatchItems:    16,
		MaxResponsePayloadBytes: 65536,
		MaxResponseBatchItems:   16,
		AuthToken:               authToken,
		Backlog:                 4,
	}
}

func clientConfig() posix.ClientConfig {
	return posix.ClientConfig{
		SupportedProfiles:       protocol.ProfileBaseline,
		MaxRequestPayloadBytes:  65536,
		MaxRequestBatchItems:    16,
		MaxResponsePayloadBytes: 65536,
		MaxResponseBatchItems:   16,
		AuthToken:               authToken,
	}
}

func runServer(runDir, service string) int {
	cfg := serverConfig()
	listener, err := posix.Listen(runDir, service, cfg)
	if err != nil {
		fmt.Fprintf(os.Stderr, "server: listen failed: %v\n", err)
		return 1
	}
	defer listener.Close()

	// Signal readiness to parent via stdout
	fmt.Println("READY")

	session, err := listener.Accept()
	if err != nil {
		fmt.Fprintf(os.Stderr, "server: accept failed: %v\n", err)
		return 1
	}
	defer session.Close()

	// Receive one message
	buf := make([]byte, 65600)
	hdr, payload, err := session.Receive(buf)
	if err != nil {
		fmt.Fprintf(os.Stderr, "server: receive failed: %v\n", err)
		return 1
	}

	// Echo as response
	resp := hdr
	resp.Kind = protocol.KindResponse
	resp.TransportStatus = protocol.StatusOK
	if err := session.Send(&resp, payload); err != nil {
		fmt.Fprintf(os.Stderr, "server: send failed: %v\n", err)
		return 1
	}

	return 0
}

func runClient(runDir, service string) int {
	cfg := clientConfig()
	session, err := posix.Connect(runDir, service, &cfg)
	if err != nil {
		fmt.Fprintf(os.Stderr, "client: connect failed: %v\n", err)
		return 1
	}
	defer session.Close()

	// Build a payload with known pattern
	payload := make([]byte, 256)
	for i := range payload {
		payload[i] = byte(i & 0xFF)
	}

	hdr := protocol.Header{
		Kind:      protocol.KindRequest,
		Code:      protocol.MethodIncrement,
		ItemCount: 1,
		MessageID: 12345,
	}

	if err := session.Send(&hdr, payload); err != nil {
		fmt.Fprintf(os.Stderr, "client: send failed: %v\n", err)
		return 1
	}

	// Receive response
	rbuf := make([]byte, 65600)
	rhdr, rpayload, err := session.Receive(rbuf)
	if err != nil {
		fmt.Fprintf(os.Stderr, "client: receive failed: %v\n", err)
		return 1
	}

	// Verify
	ok := true
	if rhdr.Kind != protocol.KindResponse {
		fmt.Fprintf(os.Stderr, "client: expected RESPONSE, got %d\n", rhdr.Kind)
		ok = false
	}
	if rhdr.MessageID != 12345 {
		fmt.Fprintf(os.Stderr, "client: expected message_id 12345, got %d\n", rhdr.MessageID)
		ok = false
	}
	if len(rpayload) != len(payload) {
		fmt.Fprintf(os.Stderr, "client: payload length mismatch: %d vs %d\n", len(rpayload), len(payload))
		ok = false
	}
	if len(rpayload) == len(payload) && !bytes.Equal(rpayload, payload) {
		fmt.Fprintf(os.Stderr, "client: payload data mismatch\n")
		ok = false
	}

	if ok {
		fmt.Println("PASS")
	} else {
		fmt.Println("FAIL")
	}

	if ok {
		return 0
	}
	return 1
}

func runPipelineServer(runDir, service string, count int) int {
	cfg := serverConfig()
	listener, err := posix.Listen(runDir, service, cfg)
	if err != nil {
		fmt.Fprintf(os.Stderr, "server: listen failed: %v\n", err)
		return 1
	}
	defer listener.Close()

	fmt.Println("READY")

	session, err := listener.Accept()
	if err != nil {
		fmt.Fprintf(os.Stderr, "server: accept failed: %v\n", err)
		return 1
	}
	defer session.Close()

	buf := make([]byte, 65600)
	for i := 0; i < count; i++ {
		hdr, payload, err := session.Receive(buf)
		if err != nil {
			fmt.Fprintf(os.Stderr, "server: receive[%d] failed: %v\n", i, err)
			return 1
		}

		resp := hdr
		resp.Kind = protocol.KindResponse
		resp.TransportStatus = protocol.StatusOK
		if err := session.Send(&resp, payload); err != nil {
			fmt.Fprintf(os.Stderr, "server: send[%d] failed: %v\n", i, err)
			return 1
		}
	}

	return 0
}

func runPipelineClient(runDir, service string, count int) int {
	cfg := clientConfig()
	session, err := posix.Connect(runDir, service, &cfg)
	if err != nil {
		fmt.Fprintf(os.Stderr, "client: connect failed: %v\n", err)
		return 1
	}
	defer session.Close()

	// Send all requests before reading any response
	for i := 0; i < count; i++ {
		val := uint64(i + 1)
		payload := make([]byte, 8)
		binary.LittleEndian.PutUint64(payload, val)

		hdr := protocol.Header{
			Kind:      protocol.KindRequest,
			Code:      protocol.MethodIncrement,
			ItemCount: 1,
			MessageID: val,
		}

		if err := session.Send(&hdr, payload); err != nil {
			fmt.Fprintf(os.Stderr, "client: send[%d] failed: %v\n", i, err)
			return 1
		}
	}

	// Read all responses and verify
	ok := true
	rbuf := make([]byte, 65600)
	for i := 0; i < count; i++ {
		rhdr, rpayload, err := session.Receive(rbuf)
		if err != nil {
			fmt.Fprintf(os.Stderr, "client: receive[%d] failed: %v\n", i, err)
			ok = false
			break
		}

		expected := uint64(i + 1)
		if rhdr.Kind != protocol.KindResponse {
			fmt.Fprintf(os.Stderr, "client: [%d] expected RESPONSE, got %d\n", i, rhdr.Kind)
			ok = false
		}
		if rhdr.MessageID != expected {
			fmt.Fprintf(os.Stderr, "client: [%d] message_id %d, want %d\n", i, rhdr.MessageID, expected)
			ok = false
		}
		if len(rpayload) != 8 {
			fmt.Fprintf(os.Stderr, "client: [%d] payload len %d, want 8\n", i, len(rpayload))
			ok = false
		} else {
			val := binary.LittleEndian.Uint64(rpayload)
			if val != expected {
				fmt.Fprintf(os.Stderr, "client: [%d] payload %d, want %d\n", i, val, expected)
				ok = false
			}
		}
	}

	if ok {
		fmt.Println("PASS")
	} else {
		fmt.Println("FAIL")
	}

	if ok {
		return 0
	}
	return 1
}

func main() {
	// Ignore SIGPIPE
	signal.Ignore(syscall.SIGPIPE)

	if len(os.Args) < 4 {
		fmt.Fprintf(os.Stderr,
			"Usage:\n  %s server <run_dir> <service>\n  %s client <run_dir> <service>\n  %s pipeline-server <run_dir> <service> <count>\n  %s pipeline-client <run_dir> <service> <count>\n",
			os.Args[0], os.Args[0], os.Args[0], os.Args[0])
		os.Exit(1)
	}

	mode := os.Args[1]
	runDir := os.Args[2]
	service := os.Args[3]

	// Ensure run_dir exists
	os.MkdirAll(runDir, 0700)

	var rc int
	switch mode {
	case "server":
		rc = runServer(runDir, service)
	case "client":
		rc = runClient(runDir, service)
	case "pipeline-server", "pipeline-client":
		if len(os.Args) < 5 {
			fmt.Fprintf(os.Stderr, "%s requires <count> argument\n", mode)
			os.Exit(1)
		}
		count, err := strconv.Atoi(os.Args[4])
		if err != nil || count <= 0 {
			fmt.Fprintf(os.Stderr, "invalid count: %s\n", os.Args[4])
			os.Exit(1)
		}
		if mode == "pipeline-server" {
			rc = runPipelineServer(runDir, service, count)
		} else {
			rc = runPipelineClient(runDir, service, count)
		}
	default:
		fmt.Fprintf(os.Stderr, "Unknown mode: %s\n", mode)
		rc = 1
	}
	os.Exit(rc)
}
