//go:build windows

// Simple server/client for cross-language Named Pipe interop tests.
//
// Usage:
//
//	interop_named_pipe server <run_dir> <service_name>
//	  Listens, accepts 1 client, echoes 1 message, exits.
//
//	interop_named_pipe client <run_dir> <service_name>
//	  Connects, sends 1 message, verifies echo, exits 0 on success.
package main

import (
	"bytes"
	"fmt"
	"os"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"
)

const authToken uint64 = 0xDEADBEEFCAFEBABE

func serverConfig() windows.ServerConfig {
	return windows.ServerConfig{
		SupportedProfiles:       protocol.ProfileBaseline,
		MaxRequestPayloadBytes:  65536,
		MaxRequestBatchItems:    16,
		MaxResponsePayloadBytes: 65536,
		MaxResponseBatchItems:   16,
		AuthToken:               authToken,
	}
}

func clientConfig() windows.ClientConfig {
	return windows.ClientConfig{
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
	listener, err := windows.Listen(runDir, service, cfg)
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
	session, err := windows.Connect(runDir, service, &cfg)
	if err != nil {
		fmt.Fprintf(os.Stderr, "client: connect failed: %v\n", err)
		return 1
	}
	defer session.Close()

	// Build payload with known pattern
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

func main() {
	if len(os.Args) != 4 {
		fmt.Fprintf(os.Stderr, "Usage: %s <server|client> <run_dir> <service_name>\n", os.Args[0])
		os.Exit(1)
	}

	mode := os.Args[1]
	runDir := os.Args[2]
	service := os.Args[3]

	var rc int
	switch mode {
	case "server":
		rc = runServer(runDir, service)
	case "client":
		rc = runClient(runDir, service)
	default:
		fmt.Fprintf(os.Stderr, "Unknown mode: %s\n", mode)
		rc = 1
	}
	os.Exit(rc)
}
