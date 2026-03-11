//go:build unix

package posix

import (
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

func tempRunDir(t *testing.T, name string) string {
	t.Helper()
	path := filepath.Join(os.TempDir(), "netipc-go-"+name+"-"+strconv.Itoa(os.Getpid())+"-"+strconv.FormatInt(time.Now().UnixNano(), 10))
	if err := os.MkdirAll(path, 0o755); err != nil {
		t.Fatalf("MkdirAll(%q) failed: %v", path, err)
	}
	return path
}

func TestIncrementRoundTrip(t *testing.T) {
	runDir := tempRunDir(t, "roundtrip")
	defer os.RemoveAll(runDir)

	server, err := Listen(NewConfig(runDir, "service"))
	if err != nil {
		t.Fatalf("Listen() error = %v", err)
	}
	defer server.Close()

	done := make(chan error, 1)
	go func() {
		if err := server.Accept(5 * time.Second); err != nil {
			done <- err
			return
		}
		requestID, request, err := server.ReceiveIncrement(5 * time.Second)
		if err != nil {
			done <- err
			return
		}
		if request.Value != 41 {
			done <- strconv.ErrSyntax
			return
		}
		done <- server.SendIncrement(requestID, protocol.IncrementResponse{Status: protocol.StatusOK, Value: 42}, 5*time.Second)
	}()

	client, err := Dial(NewConfig(runDir, "service"), 5*time.Second)
	if err != nil {
		t.Fatalf("Dial() error = %v", err)
	}
	defer client.Close()

	response, err := client.CallIncrement(protocol.IncrementRequest{Value: 41}, 5*time.Second)
	if err != nil {
		t.Fatalf("CallIncrement() error = %v", err)
	}
	if response.Status != protocol.StatusOK || response.Value != 42 {
		t.Fatalf("unexpected response: %+v", response)
	}
	if client.NegotiatedProfile() != ProfileUDSSeqpacket {
		t.Fatalf("unexpected negotiated profile: %d", client.NegotiatedProfile())
	}
	if err := <-done; err != nil {
		t.Fatalf("server goroutine error = %v", err)
	}
}

func TestAuthMismatchRejected(t *testing.T) {
	runDir := tempRunDir(t, "auth")
	defer os.RemoveAll(runDir)

	serverCfg := NewConfig(runDir, "service")
	serverCfg.AuthToken = 111
	server, err := Listen(serverCfg)
	if err != nil {
		t.Fatalf("Listen() error = %v", err)
	}
	defer server.Close()

	done := make(chan error, 1)
	go func() {
		done <- server.Accept(5 * time.Second)
	}()

	clientCfg := NewConfig(runDir, "service")
	clientCfg.AuthToken = 222
	client, err := Dial(clientCfg, 5*time.Second)
	if err == nil {
		client.Close()
		t.Fatal("Dial() unexpectedly succeeded")
	}
	if serverErr := <-done; serverErr == nil {
		t.Fatal("server Accept() unexpectedly succeeded")
	}
}
