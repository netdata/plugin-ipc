//go:build windows

package cgroups

import (
	"testing"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	windows "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"
)

func TestWinShmRoundTrip(t *testing.T) {
	svc := uniqueWinService("go_win_shm_roundtrip")
	ts := startTestServerWinWithConfig(svc, testWinShmServerConfig(), winTestHandlers())
	defer ts.stop()

	client := NewClient(winTestRunDir, svc, testWinShmClientConfig())
	defer client.Close()

	waitWinClientReady(t, client)

	if client.session == nil {
		t.Fatal("expected negotiated session")
	}
	if client.shm == nil {
		t.Fatal("expected WinSHM attachment")
	}
	if client.session.SelectedProfile != windows.WinShmProfileHybrid {
		t.Fatalf("selected profile = %d, want %d", client.session.SelectedProfile, windows.WinShmProfileHybrid)
	}

	view, err := client.CallSnapshot()
	if err != nil {
		t.Fatalf("CallSnapshot failed: %v", err)
	}
	if view.ItemCount != 3 {
		t.Fatalf("snapshot item count = %d, want 3", view.ItemCount)
	}

	got, err := client.CallIncrement(41)
	if err != nil {
		t.Fatalf("CallIncrement failed: %v", err)
	}
	if got != 42 {
		t.Fatalf("increment result = %d, want 42", got)
	}

	reversed, err := client.CallStringReverse("hello")
	if err != nil {
		t.Fatalf("CallStringReverse failed: %v", err)
	}
	if reversed.Str != "olleh" {
		t.Fatalf("string reverse result = %q, want %q", reversed.Str, "olleh")
	}

	batch, err := client.CallIncrementBatch([]uint64{1, 41, 99})
	if err != nil {
		t.Fatalf("CallIncrementBatch failed: %v", err)
	}
	wantBatch := []uint64{2, 42, 100}
	if len(batch) != len(wantBatch) {
		t.Fatalf("batch result len = %d, want %d", len(batch), len(wantBatch))
	}
	for i, want := range wantBatch {
		if batch[i] != want {
			t.Fatalf("batch[%d] = %d, want %d", i, batch[i], want)
		}
	}

	status := client.Status()
	if status.CallCount != 4 || status.ErrorCount != 0 {
		t.Fatalf("unexpected client status: %+v", status)
	}
}

func TestWinShmIdleTimeoutKeepsSessionAlive(t *testing.T) {
	svc := uniqueWinService("go_win_shm_idle")
	ts := startTestServerWinWithConfig(svc, testWinShmServerConfig(), winTestHandlers())
	defer ts.stop()

	client := NewClient(winTestRunDir, svc, testWinShmClientConfig())
	defer client.Close()

	waitWinClientReady(t, client)

	time.Sleep(350 * time.Millisecond)

	got, err := client.CallIncrement(9)
	if err != nil {
		t.Fatalf("CallIncrement after idle timeout failed: %v", err)
	}
	if got != 10 {
		t.Fatalf("CallIncrement after idle timeout = %d, want 10", got)
	}
}

func TestWinServerRunInvalidServiceName(t *testing.T) {
	server := NewServer(winTestRunDir, "bad/name", testWinServerConfig(), winTestHandlers())
	if err := server.Run(); err == nil {
		t.Fatal("expected Run() to fail for invalid service name")
	}
}

func TestWinShmAttachFailureLeavesClientDisconnected(t *testing.T) {
	svc := uniqueWinService("go_win_shm_attach_fail")
	cfg := testWinShmServerConfig()

	listener, err := windows.Listen(winTestRunDir, svc, cfg)
	if err != nil {
		t.Fatalf("windows.Listen failed: %v", err)
	}
	defer listener.Close()

	doneCh := make(chan error, 1)
	go func() {
		session, err := listener.Accept()
		if err != nil {
			doneCh <- err
			return
		}
		defer session.Close()

		recvBuf := make([]byte, protocol.HeaderSize+int(cfg.MaxRequestPayloadBytes))
		_, _, err = session.Receive(recvBuf)
		doneCh <- err
	}()

	client := NewClient(winTestRunDir, svc, testWinShmClientConfig())
	defer client.Close()

	if changed := client.Refresh(); changed {
		t.Fatal("refresh should end back in DISCONNECTED when WinSHM attach never becomes available")
	}
	if client.Ready() {
		t.Fatal("client should not be ready after attach failure")
	}
	if client.state != StateDisconnected {
		t.Fatalf("client state = %d, want DISCONNECTED", client.state)
	}
	if client.shm != nil {
		t.Fatal("expected no WinSHM attachment after attach failure")
	}
	if client.session != nil {
		t.Fatal("expected no live session after attach failure")
	}

	if err := <-doneCh; err == nil {
		t.Fatal("expected raw server receive to fail after attach failure disconnect")
	}
}
