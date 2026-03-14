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

func TestListenerAcceptsTwoSessions(t *testing.T) {
	runDir := tempRunDir(t, "multi-client")
	defer os.RemoveAll(runDir)

	listener, err := NewListener(NewConfig(runDir, "service"))
	if err != nil {
		t.Fatalf("NewListener() error = %v", err)
	}
	defer listener.Close()

	serverDone := make(chan error, 1)
	go func() {
		sessionA, err := listener.Accept(5 * time.Second)
		if err != nil {
			serverDone <- err
			return
		}
		defer sessionA.Close()

		sessionB, err := listener.Accept(5 * time.Second)
		if err != nil {
			serverDone <- err
			return
		}
		defer sessionB.Close()

		requestIDA, requestA, err := sessionA.ReceiveIncrement(5 * time.Second)
		if err != nil {
			serverDone <- err
			return
		}
		requestIDB, requestB, err := sessionB.ReceiveIncrement(5 * time.Second)
		if err != nil {
			serverDone <- err
			return
		}
		if requestA.Value+requestB.Value != 140 {
			serverDone <- strconv.ErrSyntax
			return
		}
		if err := sessionA.SendIncrement(requestIDA, protocol.IncrementResponse{Status: protocol.StatusOK, Value: requestA.Value + 1}, 5*time.Second); err != nil {
			serverDone <- err
			return
		}
		serverDone <- sessionB.SendIncrement(requestIDB, protocol.IncrementResponse{Status: protocol.StatusOK, Value: requestB.Value + 1}, 5*time.Second)
	}()

	clientDone := make(chan error, 2)
	callClient := func(value uint64) {
		client, err := Dial(NewConfig(runDir, "service"), 5*time.Second)
		if err != nil {
			clientDone <- err
			return
		}
		defer client.Close()

		response, err := client.CallIncrement(protocol.IncrementRequest{Value: value}, 5*time.Second)
		if err != nil {
			clientDone <- err
			return
		}
		if response.Status != protocol.StatusOK || response.Value != value+1 {
			clientDone <- strconv.ErrSyntax
			return
		}
		clientDone <- nil
	}

	go callClient(41)
	go callClient(99)

	for i := 0; i < 2; i++ {
		if err := <-clientDone; err != nil {
			t.Fatalf("client goroutine error = %v", err)
		}
	}
	if err := <-serverDone; err != nil {
		t.Fatalf("server goroutine error = %v", err)
	}
}

func TestPipelinedResponsesMayArriveOutOfOrder(t *testing.T) {
	runDir := tempRunDir(t, "pipeline")
	defer os.RemoveAll(runDir)

	server, err := Listen(NewConfig(runDir, "service"))
	if err != nil {
		t.Fatalf("Listen() error = %v", err)
	}
	defer server.Close()

	serverDone := make(chan error, 1)
	go func() {
		if err := server.Accept(5 * time.Second); err != nil {
			serverDone <- err
			return
		}
		requestIDA, requestA, err := server.ReceiveIncrement(5 * time.Second)
		if err != nil {
			serverDone <- err
			return
		}
		requestIDB, requestB, err := server.ReceiveIncrement(5 * time.Second)
		if err != nil {
			serverDone <- err
			return
		}
		if err := server.SendIncrement(requestIDB, protocol.IncrementResponse{Status: protocol.StatusOK, Value: requestB.Value + 1}, 5*time.Second); err != nil {
			serverDone <- err
			return
		}
		serverDone <- server.SendIncrement(requestIDA, protocol.IncrementResponse{Status: protocol.StatusOK, Value: requestA.Value + 1}, 5*time.Second)
	}()

	client, err := Dial(NewConfig(runDir, "service"), 5*time.Second)
	if err != nil {
		t.Fatalf("Dial() error = %v", err)
	}
	defer client.Close()

	if err := client.SendIncrement(1001, protocol.IncrementRequest{Value: 41}, 5*time.Second); err != nil {
		t.Fatalf("SendIncrement(request A) error = %v", err)
	}
	if err := client.SendIncrement(1002, protocol.IncrementRequest{Value: 42}, 5*time.Second); err != nil {
		t.Fatalf("SendIncrement(request B) error = %v", err)
	}

	responseID0, response0, err := client.ReceiveIncrement(5 * time.Second)
	if err != nil {
		t.Fatalf("ReceiveIncrement(response 0) error = %v", err)
	}
	responseID1, response1, err := client.ReceiveIncrement(5 * time.Second)
	if err != nil {
		t.Fatalf("ReceiveIncrement(response 1) error = %v", err)
	}

	if responseID0 != 1002 || response0.Status != protocol.StatusOK || response0.Value != 43 {
		t.Fatalf("unexpected first pipelined response: id=%d response=%+v", responseID0, response0)
	}
	if responseID1 != 1001 || response1.Status != protocol.StatusOK || response1.Value != 42 {
		t.Fatalf("unexpected second pipelined response: id=%d response=%+v", responseID1, response1)
	}
	if err := <-serverDone; err != nil {
		t.Fatalf("server goroutine error = %v", err)
	}
}
