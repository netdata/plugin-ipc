//go:build linux

package posix

import (
	"errors"
	"os"
	"syscall"
	"testing"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

func TestRawSHMSplitIncrementRoundTrip(t *testing.T) {
	runDir := tempRunDir(t, "shm-split")
	defer os.RemoveAll(runDir)

	sockPath := endpointSockPath(runDir, "service")
	server, err := newSHMServer(sockPath, protocol.FrameSize, protocol.FrameSize)
	if err != nil {
		t.Fatalf("newSHMServer() error = %v", err)
	}
	defer server.close()

	done := make(chan error, 1)
	go func() {
		requestFrame, err := server.receiveFrame(5 * time.Second)
		if err != nil {
			done <- err
			return
		}
		requestID, request, err := protocol.DecodeIncrementRequest(requestFrame)
		if err != nil {
			done <- err
			return
		}
		if requestID != 1001 || request.Value != 41 {
			done <- errors.New("unexpected request")
			return
		}
		done <- server.sendFrame(protocol.EncodeIncrementResponse(requestID, protocol.IncrementResponse{
			Status: protocol.StatusOK,
			Value:  42,
		}))
	}()

	client, err := newSHMClient(sockPath, protocol.FrameSize, protocol.FrameSize, 5*time.Second)
	if err != nil {
		t.Fatalf("newSHMClient() error = %v", err)
	}
	defer client.close()

	if err := client.sendIncrement(1001, protocol.IncrementRequest{Value: 41}, 5*time.Second); err != nil {
		t.Fatalf("sendIncrement() error = %v", err)
	}
	responseID, response, err := client.receiveIncrement(5 * time.Second)
	if err != nil {
		t.Fatalf("receiveIncrement() error = %v", err)
	}
	if responseID != 1001 || response.Status != protocol.StatusOK || response.Value != 42 {
		t.Fatalf("unexpected response: id=%d response=%+v", responseID, response)
	}
	if err := <-done; err != nil {
		t.Fatalf("server goroutine error = %v", err)
	}
}

func TestRawSHMSendWhilePendingRejected(t *testing.T) {
	runDir := tempRunDir(t, "shm-pending")
	defer os.RemoveAll(runDir)

	sockPath := endpointSockPath(runDir, "service")
	server, err := newSHMServer(sockPath, protocol.FrameSize, protocol.FrameSize)
	if err != nil {
		t.Fatalf("newSHMServer() error = %v", err)
	}
	defer server.close()

	client, err := newSHMClient(sockPath, protocol.FrameSize, protocol.FrameSize, 5*time.Second)
	if err != nil {
		t.Fatalf("newSHMClient() error = %v", err)
	}
	defer client.close()

	if err := client.sendIncrement(1001, protocol.IncrementRequest{Value: 41}, 5*time.Second); err != nil {
		t.Fatalf("first sendIncrement() error = %v", err)
	}
	if err := client.sendIncrement(1002, protocol.IncrementRequest{Value: 42}, 5*time.Second); !errors.Is(err, syscall.EBUSY) {
		t.Fatalf("second sendIncrement() error = %v, want EBUSY", err)
	}
}

func TestNegotiatedSHMSplitIncrementRoundTrip(t *testing.T) {
	runDir := tempRunDir(t, "negotiated-shm-split")
	defer os.RemoveAll(runDir)

	serverCfg := NewConfig(runDir, "service")
	serverCfg.SupportedProfiles = ProfileUDSSeqpacket | ProfileSHMHybrid
	serverCfg.PreferredProfiles = ProfileSHMHybrid
	server, err := Listen(serverCfg)
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
		if server.NegotiatedProfile() != ProfileSHMHybrid {
			done <- errors.New("server negotiated wrong profile")
			return
		}
		requestID, request, err := server.ReceiveIncrement(5 * time.Second)
		if err != nil {
			done <- err
			return
		}
		if requestID != 1001 || request.Value != 41 {
			done <- errors.New("unexpected request")
			return
		}
		done <- server.SendIncrement(1001, protocol.IncrementResponse{
			Status: protocol.StatusOK,
			Value:  42,
		}, 5*time.Second)
	}()

	clientCfg := serverCfg
	client, err := Dial(clientCfg, 5*time.Second)
	if err != nil {
		t.Fatalf("Dial() error = %v", err)
	}
	defer client.Close()

	if client.NegotiatedProfile() != ProfileSHMHybrid {
		t.Fatalf("unexpected negotiated profile: %d", client.NegotiatedProfile())
	}
	if err := client.SendIncrement(1001, protocol.IncrementRequest{Value: 41}, 5*time.Second); err != nil {
		t.Fatalf("SendIncrement() error = %v", err)
	}
	responseID, response, err := client.ReceiveIncrement(5 * time.Second)
	if err != nil {
		t.Fatalf("ReceiveIncrement() error = %v", err)
	}
	if responseID != 1001 || response.Status != protocol.StatusOK || response.Value != 42 {
		t.Fatalf("unexpected response: id=%d response=%+v", responseID, response)
	}
	if err := <-done; err != nil {
		t.Fatalf("server goroutine error = %v", err)
	}
}
