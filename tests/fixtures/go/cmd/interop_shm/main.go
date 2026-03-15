//go:build linux

// Simple server/client for cross-language SHM interop tests.
//
// Usage:
//
//	interop_shm server <run_dir> <service_name>
//	  Creates SHM region, receives 1 message, echoes it, exits.
//
//	interop_shm client <run_dir> <service_name>
//	  Attaches to SHM, sends 1 message, verifies echo, exits 0 on success.
package main

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix"
)

func buildMessage(kind, code uint16, messageID uint64, payload []byte) []byte {
	hdr := protocol.Header{
		Magic:      protocol.MagicMsg,
		Version:    protocol.Version,
		HeaderLen:  protocol.HeaderLen,
		Kind:       kind,
		Code:       code,
		ItemCount:  1,
		MessageID:  messageID,
		PayloadLen: uint32(len(payload)),
	}
	buf := make([]byte, protocol.HeaderSize+len(payload))
	hdr.Encode(buf[:protocol.HeaderSize])
	copy(buf[protocol.HeaderSize:], payload)
	return buf
}

func runServer(runDir, service string) int {
	ctx, err := posix.ShmServerCreate(runDir, service, 1, 65536, 65536)
	if err != nil {
		fmt.Fprintf(os.Stderr, "server: shm create failed: %v\n", err)
		return 1
	}
	defer ctx.ShmDestroy()

	// Signal readiness
	fmt.Println("READY")

	buf := make([]byte, 65536)
	mlen, err := ctx.ShmReceive(buf, 10000)
	if err != nil {
		fmt.Fprintf(os.Stderr, "server: receive failed: %v\n", err)
		return 1
	}

	if mlen < protocol.HeaderSize {
		fmt.Fprintf(os.Stderr, "server: message too short: %d\n", mlen)
		return 1
	}

	// Parse and echo as response
	hdr, err := protocol.DecodeHeader(buf[:mlen])
	if err != nil {
		fmt.Fprintf(os.Stderr, "server: decode header: %v\n", err)
		return 1
	}

	// Copy payload before sending (send overwrites SHM region for response area)
	payload := make([]byte, mlen-protocol.HeaderSize)
	copy(payload, buf[protocol.HeaderSize:mlen])

	resp := buildMessage(protocol.KindResponse, hdr.Code, hdr.MessageID, payload)
	if err := ctx.ShmSend(resp); err != nil {
		fmt.Fprintf(os.Stderr, "server: send failed: %v\n", err)
		return 1
	}

	return 0
}

func runClient(runDir, service string) int {
	// Retry attach -- server may not be fully ready yet
	var ctx *posix.ShmContext
	var err error
	for i := 0; i < 500; i++ {
		ctx, err = posix.ShmClientAttach(runDir, service, 1)
		if err == nil {
			break
		}
		if errors.Is(err, posix.ErrShmNotReady) ||
			errors.Is(err, posix.ErrShmOpen) ||
			errors.Is(err, posix.ErrShmBadMagic) {
			time.Sleep(10 * time.Millisecond)
			continue
		}
		break
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "client: attach failed: %v\n", err)
		return 1
	}
	defer ctx.ShmClose()

	// Build payload with known pattern
	payload := make([]byte, 256)
	for i := range payload {
		payload[i] = byte(i & 0xFF)
	}

	msg := buildMessage(protocol.KindRequest, protocol.MethodIncrement, 12345, payload)
	if err := ctx.ShmSend(msg); err != nil {
		fmt.Fprintf(os.Stderr, "client: send failed: %v\n", err)
		return 1
	}

	// Receive response
	respBuf := make([]byte, 65536)
	rlen, err := ctx.ShmReceive(respBuf, 10000)
	if err != nil {
		fmt.Fprintf(os.Stderr, "client: receive failed: %v\n", err)
		return 1
	}

	respCopy := make([]byte, rlen)
	copy(respCopy, respBuf[:rlen])

	// Verify
	ok := true
	if len(respCopy) < protocol.HeaderSize {
		fmt.Fprintf(os.Stderr, "client: response too short\n")
		ok = false
	} else {
		rhdr, err := protocol.DecodeHeader(respCopy)
		if err != nil {
			fmt.Fprintf(os.Stderr, "client: decode response: %v\n", err)
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
			respPayload := respCopy[protocol.HeaderSize:]
			if len(respPayload) != len(payload) {
				fmt.Fprintf(os.Stderr, "client: payload length mismatch: %d vs %d\n",
					len(respPayload), len(payload))
				ok = false
			}
			if len(respPayload) == len(payload) && !bytes.Equal(respPayload, payload) {
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
	signal.Ignore(syscall.SIGPIPE)

	if len(os.Args) != 4 {
		fmt.Fprintf(os.Stderr, "Usage: %s <server|client> <run_dir> <service_name>\n", os.Args[0])
		os.Exit(1)
	}

	mode := os.Args[1]
	runDir := os.Args[2]
	service := os.Args[3]

	os.MkdirAll(runDir, 0700)

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
