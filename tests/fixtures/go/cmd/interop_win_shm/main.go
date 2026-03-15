//go:build windows

// Simple server/client for cross-language Windows SHM interop tests.
//
// Usage:
//
//	interop_win_shm server <run_dir> <service_name>
//	  Creates SHM region, receives 1 message, echoes it, exits.
//
//	interop_win_shm client <run_dir> <service_name>
//	  Attaches to SHM, sends 1 message, verifies echo, exits 0 on success.
package main

import (
	"bytes"
	"fmt"
	"os"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"
)

const authToken uint64 = 0xDEADBEEFCAFEBABE

func buildMessage(kind, code uint16, messageID uint64, payload []byte) []byte {
	hdr := protocol.Header{
		Magic:           protocol.MagicMsg,
		Version:         protocol.Version,
		HeaderLen:       protocol.HeaderLen,
		Kind:            kind,
		Code:            code,
		Flags:           0,
		TransportStatus: protocol.StatusOK,
		PayloadLen:      uint32(len(payload)),
		ItemCount:       1,
		MessageID:       messageID,
	}
	buf := make([]byte, protocol.HeaderSize+len(payload))
	hdr.Encode(buf[:protocol.HeaderSize])
	copy(buf[protocol.HeaderSize:], payload)
	return buf
}

func runServer(runDir, service string) int {
	ctx, err := windows.WinShmServerCreate(runDir, service, authToken,
		windows.WinShmProfileHybrid, 65536, 65536)
	if err != nil {
		fmt.Fprintf(os.Stderr, "server: create failed: %v\n", err)
		return 1
	}
	defer ctx.WinShmDestroy()

	fmt.Println("READY")

	// Receive one message
	buf := make([]byte, 65536)
	mlen, err := ctx.WinShmReceive(buf, 10000)
	if err != nil {
		fmt.Fprintf(os.Stderr, "server: receive failed: %v\n", err)
		return 1
	}

	if mlen < protocol.HeaderSize {
		fmt.Fprintf(os.Stderr, "server: message too short\n")
		return 1
	}

	// Parse and echo
	hdr, err := protocol.DecodeHeader(buf[:mlen])
	if err != nil {
		fmt.Fprintf(os.Stderr, "server: decode failed: %v\n", err)
		return 1
	}

	payload := make([]byte, mlen-protocol.HeaderSize)
	copy(payload, buf[protocol.HeaderSize:mlen])
	resp := buildMessage(protocol.KindResponse, hdr.Code, hdr.MessageID, payload)

	if err := ctx.WinShmSend(resp); err != nil {
		fmt.Fprintf(os.Stderr, "server: send failed: %v\n", err)
		return 1
	}

	return 0
}

func runClient(runDir, service string) int {
	// Retry attach
	var ctx *windows.WinShmContext
	var err error
	for i := 0; i < 500; i++ {
		ctx, err = windows.WinShmClientAttach(runDir, service, authToken,
			windows.WinShmProfileHybrid)
		if err == nil {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "client: attach failed: %v\n", err)
		return 1
	}
	defer ctx.WinShmClose()

	// Build payload with known pattern
	payload := make([]byte, 256)
	for i := range payload {
		payload[i] = byte(i & 0xFF)
	}
	msg := buildMessage(protocol.KindRequest, protocol.MethodIncrement, 12345, payload)

	if err := ctx.WinShmSend(msg); err != nil {
		fmt.Fprintf(os.Stderr, "client: send failed: %v\n", err)
		return 1
	}

	// Receive response
	rbuf := make([]byte, 65536)
	rlen, err := ctx.WinShmReceive(rbuf, 10000)
	if err != nil {
		fmt.Fprintf(os.Stderr, "client: receive failed: %v\n", err)
		return 1
	}

	ok := true
	if rlen < protocol.HeaderSize {
		fmt.Fprintf(os.Stderr, "client: response too short\n")
		ok = false
	} else {
		rhdr, err := protocol.DecodeHeader(rbuf[:rlen])
		if err != nil {
			fmt.Fprintf(os.Stderr, "client: decode failed: %v\n", err)
			ok = false
		} else {
			if rhdr.Kind != protocol.KindResponse {
				fmt.Fprintf(os.Stderr, "client: expected RESPONSE, got %d\n", rhdr.Kind)
				ok = false
			}
			if rhdr.MessageID != 12345 {
				fmt.Fprintf(os.Stderr, "client: expected message_id 12345, got %d\n", rhdr.MessageID)
				ok = false
			}
			rpayload := rbuf[protocol.HeaderSize:rlen]
			if len(rpayload) != len(payload) {
				fmt.Fprintf(os.Stderr, "client: payload length mismatch: %d vs %d\n",
					len(rpayload), len(payload))
				ok = false
			}
			if len(rpayload) == len(payload) && !bytes.Equal(rpayload, payload) {
				fmt.Fprintf(os.Stderr, "client: payload data mismatch\n")
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
