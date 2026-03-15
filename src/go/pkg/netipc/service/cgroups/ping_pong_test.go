//go:build unix

package cgroups

import (
	"testing"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

// pingPongHandler handles INCREMENT (code 1) and STRING_REVERSE (code 3).
// INCREMENT: decode u64, add 1, encode.
// STRING_REVERSE: decode string, reverse bytes, encode.
func pingPongHandler(methodCode uint16, request []byte) ([]byte, bool) {
	switch methodCode {
	case protocol.MethodIncrement:
		val, err := protocol.IncrementDecode(request)
		if err != nil {
			return nil, false
		}
		var buf [protocol.IncrementPayloadSize]byte
		if protocol.IncrementEncode(val+1, buf[:]) == 0 {
			return nil, false
		}
		return buf[:], true

	case protocol.MethodStringReverse:
		view, err := protocol.StringReverseDecode(request)
		if err != nil {
			return nil, false
		}
		reversed := reverseString(view.Str)
		buf := make([]byte, protocol.StringReverseHdrSize+len(reversed)+1)
		if protocol.StringReverseEncode(reversed, buf) == 0 {
			return nil, false
		}
		return buf, true

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
