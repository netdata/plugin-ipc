//go:build linux

package posix

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"os"
	"sync"
	"testing"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

const testShmRunDir = "/tmp/nipc_shm_go_test"

func ensureShmRunDir(t *testing.T) {
	t.Helper()
	if err := os.MkdirAll(testShmRunDir, 0700); err != nil {
		t.Fatalf("cannot create SHM run dir: %v", err)
	}
}

func cleanupShmFile(t *testing.T, service string) {
	t.Helper()
	os.Remove(fmt.Sprintf("%s/%s.ipcshm", testShmRunDir, service))
}

// buildShmMessage creates a complete wire message (32-byte header + payload).
func buildShmMessage(kind, code uint16, messageID uint64, payload []byte) []byte {
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

func TestShmDirectRoundtrip(t *testing.T) {
	ensureShmRunDir(t)
	svc := "go_shm_rt"
	cleanupShmFile(t, svc)
	defer cleanupShmFile(t, svc)

	var wg sync.WaitGroup
	var serverErr error

	wg.Add(1)
	go func() {
		defer wg.Done()
		ctx, err := ShmServerCreate(testShmRunDir, svc, 4096, 4096)
		if err != nil {
			serverErr = fmt.Errorf("server create: %w", err)
			return
		}
		defer ctx.ShmDestroy()

		buf := make([]byte, 65536)
		mlen, err := ctx.ShmReceive(buf, 5000)
		if err != nil {
			serverErr = fmt.Errorf("server receive: %w", err)
			return
		}

		if mlen < protocol.HeaderSize {
			serverErr = fmt.Errorf("message too short: %d", mlen)
			return
		}

		// Parse header, echo as response
		hdr, err := protocol.DecodeHeader(buf[:mlen])
		if err != nil {
			serverErr = fmt.Errorf("decode header: %w", err)
			return
		}
		payload := make([]byte, mlen-protocol.HeaderSize)
		copy(payload, buf[protocol.HeaderSize:mlen])
		resp := buildShmMessage(protocol.KindResponse, hdr.Code, hdr.MessageID, payload)
		if err := ctx.ShmSend(resp); err != nil {
			serverErr = fmt.Errorf("server send: %w", err)
		}
	}()

	// Wait for server to create region
	time.Sleep(50 * time.Millisecond)

	client, err := ShmClientAttach(testShmRunDir, svc)
	if err != nil {
		t.Fatalf("client attach: %v", err)
	}
	defer client.ShmClose()

	payload := []byte{0xCA, 0xFE, 0xBA, 0xBE}
	msg := buildShmMessage(protocol.KindRequest, protocol.MethodIncrement, 42, payload)
	if err := client.ShmSend(msg); err != nil {
		t.Fatalf("client send: %v", err)
	}

	respBuf := make([]byte, 65536)
	rlen, err := client.ShmReceive(respBuf, 5000)
	if err != nil {
		t.Fatalf("client receive: %v", err)
	}

	if rlen != protocol.HeaderSize+len(payload) {
		t.Fatalf("response length: got %d, want %d", rlen, protocol.HeaderSize+len(payload))
	}

	rhdr, err := protocol.DecodeHeader(respBuf[:rlen])
	if err != nil {
		t.Fatalf("decode response header: %v", err)
	}
	if rhdr.Kind != protocol.KindResponse {
		t.Errorf("response kind: got %d, want %d", rhdr.Kind, protocol.KindResponse)
	}
	if rhdr.MessageID != 42 {
		t.Errorf("response message_id: got %d, want 42", rhdr.MessageID)
	}
	respPayload := respBuf[protocol.HeaderSize:rlen]
	if !bytes.Equal(respPayload, payload) {
		t.Errorf("response payload mismatch")
	}

	wg.Wait()
	if serverErr != nil {
		t.Fatalf("server error: %v", serverErr)
	}
}

func TestShmMultipleRoundtrips(t *testing.T) {
	ensureShmRunDir(t)
	svc := "go_shm_multi"
	cleanupShmFile(t, svc)
	defer cleanupShmFile(t, svc)

	var wg sync.WaitGroup
	var serverErr error

	wg.Add(1)
	go func() {
		defer wg.Done()
		ctx, err := ShmServerCreate(testShmRunDir, svc, 4096, 4096)
		if err != nil {
			serverErr = fmt.Errorf("server create: %w", err)
			return
		}
		defer ctx.ShmDestroy()

		buf := make([]byte, 65536)
		for i := 0; i < 10; i++ {
			mlen, err := ctx.ShmReceive(buf, 5000)
			if err != nil {
				serverErr = fmt.Errorf("server receive %d: %w", i, err)
				return
			}
			hdr, err := protocol.DecodeHeader(buf[:mlen])
			if err != nil {
				serverErr = fmt.Errorf("decode header %d: %w", i, err)
				return
			}
			payload := make([]byte, mlen-protocol.HeaderSize)
			copy(payload, buf[protocol.HeaderSize:mlen])
			resp := buildShmMessage(protocol.KindResponse, hdr.Code, hdr.MessageID, payload)
			if err := ctx.ShmSend(resp); err != nil {
				serverErr = fmt.Errorf("server send %d: %w", i, err)
				return
			}
		}
	}()

	time.Sleep(50 * time.Millisecond)
	client, err := ShmClientAttach(testShmRunDir, svc)
	if err != nil {
		t.Fatalf("client attach: %v", err)
	}
	defer client.ShmClose()

	respBuf := make([]byte, 65536)
	for i := uint64(0); i < 10; i++ {
		payload := []byte{byte(i)}
		msg := buildShmMessage(protocol.KindRequest, 1, i+1, payload)
		if err := client.ShmSend(msg); err != nil {
			t.Fatalf("client send %d: %v", i, err)
		}

		rlen, err := client.ShmReceive(respBuf, 5000)
		if err != nil {
			t.Fatalf("client receive %d: %v", i, err)
		}

		rhdr, err := protocol.DecodeHeader(respBuf[:rlen])
		if err != nil {
			t.Fatalf("decode response %d: %v", i, err)
		}
		if rhdr.Kind != protocol.KindResponse {
			t.Errorf("round %d: kind=%d, want %d", i, rhdr.Kind, protocol.KindResponse)
		}
		if rhdr.MessageID != i+1 {
			t.Errorf("round %d: message_id=%d, want %d", i, rhdr.MessageID, i+1)
		}
		if respBuf[protocol.HeaderSize] != byte(i) {
			t.Errorf("round %d: payload byte=%d, want %d", i, respBuf[protocol.HeaderSize], i)
		}
	}

	wg.Wait()
	if serverErr != nil {
		t.Fatalf("server error: %v", serverErr)
	}
}

func TestShmStaleRecovery(t *testing.T) {
	ensureShmRunDir(t)
	svc := "go_shm_stale"
	cleanupShmFile(t, svc)
	defer cleanupShmFile(t, svc)

	// Create a region, then corrupt owner_pid to simulate dead process
	first, err := ShmServerCreate(testShmRunDir, svc, 1024, 1024)
	if err != nil {
		t.Fatalf("first create: %v", err)
	}

	// Write a dead PID into the header
	binary.LittleEndian.PutUint32(first.data[8:12], 99999) // very unlikely alive
	first.ShmClose() // close without unlink

	// Should succeed via stale recovery
	second, err := ShmServerCreate(testShmRunDir, svc, 2048, 2048)
	if err != nil {
		t.Fatalf("stale recovery create: %v", err)
	}
	if second.requestCapacity < 2048 {
		t.Errorf("new region capacity: %d, want >= 2048", second.requestCapacity)
	}
	second.ShmDestroy()
}

func TestShmLargeMessage(t *testing.T) {
	ensureShmRunDir(t)
	svc := "go_shm_large"
	cleanupShmFile(t, svc)
	defer cleanupShmFile(t, svc)

	var wg sync.WaitGroup
	var serverErr error

	wg.Add(1)
	go func() {
		defer wg.Done()
		ctx, err := ShmServerCreate(testShmRunDir, svc, 65536, 65536)
		if err != nil {
			serverErr = fmt.Errorf("server create: %w", err)
			return
		}
		defer ctx.ShmDestroy()

		buf := make([]byte, 65536)
		mlen, err := ctx.ShmReceive(buf, 5000)
		if err != nil {
			serverErr = fmt.Errorf("server receive: %w", err)
			return
		}

		hdr, err := protocol.DecodeHeader(buf[:mlen])
		if err != nil {
			serverErr = fmt.Errorf("decode: %w", err)
			return
		}
		payload := make([]byte, mlen-protocol.HeaderSize)
		copy(payload, buf[protocol.HeaderSize:mlen])
		resp := buildShmMessage(protocol.KindResponse, hdr.Code, hdr.MessageID, payload)
		if err := ctx.ShmSend(resp); err != nil {
			serverErr = fmt.Errorf("server send: %w", err)
		}
	}()

	time.Sleep(50 * time.Millisecond)
	client, err := ShmClientAttach(testShmRunDir, svc)
	if err != nil {
		t.Fatalf("client attach: %v", err)
	}
	defer client.ShmClose()

	// 60000 bytes of payload
	payload := make([]byte, 60000)
	for i := range payload {
		payload[i] = byte(i & 0xFF)
	}
	msg := buildShmMessage(protocol.KindRequest, 1, 999, payload)
	if err := client.ShmSend(msg); err != nil {
		t.Fatalf("client send: %v", err)
	}

	respBuf := make([]byte, 65536)
	rlen, err := client.ShmReceive(respBuf, 5000)
	if err != nil {
		t.Fatalf("client receive: %v", err)
	}

	if rlen != protocol.HeaderSize+len(payload) {
		t.Fatalf("response length: got %d, want %d", rlen, protocol.HeaderSize+len(payload))
	}

	respPayload := respBuf[protocol.HeaderSize:rlen]
	if !bytes.Equal(respPayload, payload) {
		t.Errorf("response payload pattern mismatch")
	}

	wg.Wait()
	if serverErr != nil {
		t.Fatalf("server error: %v", serverErr)
	}
}
