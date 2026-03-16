//go:build unix

package cgroups

import (
	"testing"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

// pingPongHandler handles INCREMENT (code 1) and STRING_REVERSE (code 3).
// INCREMENT: decode u64, add 1, encode.
// STRING_REVERSE: decode string, reverse bytes, encode.
func pingPongHandler(methodCode uint16, request []byte) ([]byte, bool) {
	switch methodCode {
	case protocol.MethodIncrement:
		var buf [protocol.IncrementPayloadSize]byte
		n, ok := protocol.DispatchIncrement(request, buf[:], func(v uint64) (uint64, bool) {
			return v + 1, true
		})
		if !ok {
			return nil, false
		}
		return buf[:n], true

	case protocol.MethodStringReverse:
		buf := make([]byte, protocol.StringReverseHdrSize+len(request)+1)
		n, ok := protocol.DispatchStringReverse(request, buf, func(s string) (string, bool) {
			return reverseString(s), true
		})
		if !ok {
			return nil, false
		}
		return buf[:n], true

	default:
		return nil, false
	}
}

// reverseString reverses a string byte-by-byte.
func reverseString(s string) string {
	b := []byte(s)
	for i, j := 0, len(b)-1; i < j; i, j = i+1, j-1 {
		b[i], b[j] = b[j], b[i]
	}
	return string(b)
}

func TestIncrementPingPong(t *testing.T) {
	svc := "go_pp_incr"
	ensureRunDir()
	cleanupAll(svc)

	ts := startTestServer(svc, pingPongHandler)
	defer ts.stop()

	client := NewClient(testRunDir, svc, testClientConfig())
	client.Refresh()
	if !client.Ready() {
		t.Fatal("client not ready")
	}

	// 10 rounds: send 0 -> get 1 -> send 1 -> get 2 -> ... -> value == 10
	var val uint64
	responsesReceived := 0
	respBuf := make([]byte, responseBufSize)
	for i := 0; i < 10; i++ {
		got, err := client.CallIncrement(val, respBuf)
		if err != nil {
			t.Fatalf("round %d: CallIncrement(%d) failed: %v", i, val, err)
		}
		responsesReceived++
		expected := val + 1
		if got != expected {
			t.Fatalf("round %d: expected %d, got %d", i, expected, got)
		}
		val = got
	}

	if responsesReceived != 10 {
		t.Fatalf("expected 10 responses received, got %d", responsesReceived)
	}
	if val != 10 {
		t.Fatalf("expected final value 10, got %d", val)
	}

	status := client.Status()
	if status.CallCount != 10 {
		t.Fatalf("expected call_count=10, got %d", status.CallCount)
	}
	if status.ErrorCount != 0 {
		t.Fatalf("expected error_count=0, got %d", status.ErrorCount)
	}

	client.Close()
	cleanupAll(svc)
}

func TestStringReversePingPong(t *testing.T) {
	svc := "go_pp_strrev"
	ensureRunDir()
	cleanupAll(svc)

	ts := startTestServer(svc, pingPongHandler)
	defer ts.stop()

	client := NewClient(testRunDir, svc, testClientConfig())
	client.Refresh()
	if !client.Ready() {
		t.Fatal("client not ready")
	}

	respBuf := make([]byte, responseBufSize)
	original := "abcdefghijklmnopqrstuvwxyz"

	// 6 rounds: feed each response back as the next request
	responsesReceived := 0
	current := original
	for i := 0; i < 6; i++ {
		view, err := client.CallStringReverse(current, respBuf)
		if err != nil {
			t.Fatalf("round %d: CallStringReverse(%q) failed: %v", i+1, current, err)
		}
		responsesReceived++

		// verify response is the character-by-character reverse of the sent string
		expectedReversed := reverseString(current)
		if view.Str != expectedReversed {
			t.Fatalf("round %d: sent %q, expected reverse %q, got %q", i+1, current, expectedReversed, view.Str)
		}

		current = view.Str
	}

	if responsesReceived != 6 {
		t.Fatalf("expected 6 responses received, got %d", responsesReceived)
	}

	// even number of reversals = identity
	if current != original {
		t.Fatalf("after 6 reversals expected original %q, got %q", original, current)
	}

	status := client.Status()
	if status.CallCount != 6 {
		t.Fatalf("expected call_count=6, got %d", status.CallCount)
	}
	if status.ErrorCount != 0 {
		t.Fatalf("expected error_count=0, got %d", status.ErrorCount)
	}

	client.Close()
	cleanupAll(svc)
}

func TestIncrementBatch(t *testing.T) {
	svc := "go_pp_batch"
	ensureRunDir()
	cleanupAll(svc)

	// Server config with batch support
	sCfg := testServerConfig()
	sCfg.MaxRequestBatchItems = 16
	sCfg.MaxResponseBatchItems = 16
	sCfg.MaxRequestPayloadBytes = 65536

	s := NewServer(testRunDir, svc, sCfg, pingPongHandler)
	doneCh := make(chan struct{})
	go func() {
		defer close(doneCh)
		s.Run()
	}()
	defer func() {
		s.Stop()
		<-doneCh
	}()

	// Wait for server
	time.Sleep(100 * time.Millisecond)

	// Client config with batch support
	cCfg := testClientConfig()
	cCfg.MaxRequestBatchItems = 16
	cCfg.MaxResponseBatchItems = 16
	cCfg.MaxRequestPayloadBytes = 65536

	client := NewClient(testRunDir, svc, cCfg)
	client.Refresh()
	if !client.Ready() {
		t.Fatal("client not ready")
	}

	respBuf := make([]byte, responseBufSize)
	input := []uint64{10, 20, 30, 40, 50}

	results, err := client.CallIncrementBatch(input, respBuf)
	if err != nil {
		t.Fatalf("CallIncrementBatch failed: %v", err)
	}

	if len(results) != len(input) {
		t.Fatalf("expected %d results, got %d", len(input), len(results))
	}

	for i, v := range input {
		expected := v + 1
		if results[i] != expected {
			t.Fatalf("item %d: expected %d, got %d", i, expected, results[i])
		}
	}

	status := client.Status()
	if status.CallCount != 1 {
		t.Fatalf("expected call_count=1, got %d", status.CallCount)
	}
	if status.ErrorCount != 0 {
		t.Fatalf("expected error_count=0, got %d", status.ErrorCount)
	}

	client.Close()
	cleanupAll(svc)
}

func TestMixedMethods(t *testing.T) {
	svc := "go_pp_mixed"
	ensureRunDir()
	cleanupAll(svc)

	ts := startTestServer(svc, pingPongHandler)
	defer ts.stop()

	client := NewClient(testRunDir, svc, testClientConfig())
	client.Refresh()
	if !client.Ready() {
		t.Fatal("client not ready")
	}

	respBuf := make([]byte, responseBufSize)

	// increment(100) -> 101
	val, err := client.CallIncrement(100, respBuf)
	if err != nil {
		t.Fatalf("increment(100) failed: %v", err)
	}
	if val != 101 {
		t.Fatalf("expected 101, got %d", val)
	}

	// reverse("hello") -> verify locally computed reverse
	helloStr := "hello"
	view, err := client.CallStringReverse(helloStr, respBuf)
	if err != nil {
		t.Fatalf("reverse(%q) failed: %v", helloStr, err)
	}
	expectedHelloRev := reverseString(helloStr)
	if view.Str != expectedHelloRev {
		t.Fatalf("reverse(%q): expected %q, got %q", helloStr, expectedHelloRev, view.Str)
	}

	// increment(101) -> 102
	val, err = client.CallIncrement(101, respBuf)
	if err != nil {
		t.Fatalf("increment(101) failed: %v", err)
	}
	if val != 102 {
		t.Fatalf("expected 102, got %d", val)
	}

	// reverse("world") -> verify locally computed reverse
	worldStr := "world"
	view, err = client.CallStringReverse(worldStr, respBuf)
	if err != nil {
		t.Fatalf("reverse(%q) failed: %v", worldStr, err)
	}
	expectedWorldRev := reverseString(worldStr)
	if view.Str != expectedWorldRev {
		t.Fatalf("reverse(%q): expected %q, got %q", worldStr, expectedWorldRev, view.Str)
	}

	status := client.Status()
	if status.CallCount != 4 {
		t.Fatalf("expected call_count=4, got %d", status.CallCount)
	}
	if status.ErrorCount != 0 {
		t.Fatalf("expected error_count=0, got %d", status.ErrorCount)
	}

	client.Close()
	cleanupAll(svc)
}
