//go:build windows

package cgroups

import (
	"fmt"
	"os"
	"sync/atomic"
	"testing"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

const testWinRunDir = `C:\Temp\nipc_go_cgroups_public`

const (
	testWinAuthToken    = uint64(0xDEADBEEFCAFEBABE)
	testWinResponseSize = 65536
)

var winServiceCounter atomic.Uint64

func uniqueWinService(prefix string) string {
	return fmt.Sprintf("%s_%d_%d", prefix, os.Getpid(), winServiceCounter.Add(1))
}

func ensureWinRunDir(t *testing.T) {
	t.Helper()
	if err := os.MkdirAll(testWinRunDir, 0o700); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
}

func testWinServerConfig() ServerConfig {
	return ServerConfig{
		SupportedProfiles:       protocol.ProfileBaseline,
		PreferredProfiles:       protocol.ProfileBaseline,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    1,
		MaxResponsePayloadBytes: testWinResponseSize,
		MaxResponseBatchItems:   1,
		AuthToken:               testWinAuthToken,
	}
}

func testWinClientConfig() ClientConfig {
	return ClientConfig{
		SupportedProfiles:       protocol.ProfileBaseline,
		PreferredProfiles:       protocol.ProfileBaseline,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    1,
		MaxResponsePayloadBytes: testWinResponseSize,
		MaxResponseBatchItems:   1,
		AuthToken:               testWinAuthToken,
	}
}

func testWinHandler() Handler {
	return Handler{
		Handle: func(req *protocol.CgroupsRequest, builder *protocol.CgroupsBuilder) bool {
			if req.LayoutVersion != 1 || req.Flags != 0 {
				return false
			}
			builder.SetHeader(1, 42)
			items := []struct {
				hash, options, enabled uint32
				name, path             []byte
			}{
				{1001, 0, 1, []byte("docker-abc123"), []byte("/sys/fs/cgroup/docker/abc123")},
				{2002, 0, 1, []byte("k8s-pod-xyz"), []byte("/sys/fs/cgroup/kubepods/xyz")},
				{3003, 0, 0, []byte("systemd-user"), []byte("/sys/fs/cgroup/user.slice/user-1000")},
			}
			for _, item := range items {
				if err := builder.Add(item.hash, item.options, item.enabled, item.name, item.path); err != nil {
					return false
				}
			}
			return true
		},
		SnapshotMaxItems: 3,
	}
}

type winTestServer struct {
	server *Server
	done   chan struct{}
}

func startWinTestServer(t *testing.T, service string) *winTestServer {
	t.Helper()
	ensureWinRunDir(t)
	s := NewServer(testWinRunDir, service, testWinServerConfig(), testWinHandler())
	done := make(chan struct{})
	go func() {
		defer close(done)
		_ = s.Run()
	}()
	time.Sleep(200 * time.Millisecond)
	return &winTestServer{server: s, done: done}
}

func (ts *winTestServer) stop() {
	ts.server.Stop()
	select {
	case <-ts.done:
	case <-time.After(2 * time.Second):
	}
}

func connectReadyWin(t *testing.T, client *Client) {
	t.Helper()
	for i := 0; i < 200; i++ {
		client.Refresh()
		if client.Ready() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("client did not reach READY state")
}

func TestSnapshotRoundTripWindows(t *testing.T) {
	service := uniqueWinService("snapshot")
	ts := startWinTestServer(t, service)
	defer ts.stop()

	client := NewClient(testWinRunDir, service, testWinClientConfig())
	defer client.Close()
	connectReadyWin(t, client)

	view, err := client.CallSnapshot()
	if err != nil {
		t.Fatalf("CallSnapshot failed: %v", err)
	}
	if view.ItemCount != 3 || view.SystemdEnabled != 1 || view.Generation != 42 {
		t.Fatalf("unexpected snapshot header: %+v", view)
	}
}

func TestCacheRoundTripWindows(t *testing.T) {
	service := uniqueWinService("cache")
	ts := startWinTestServer(t, service)
	defer ts.stop()

	cache := NewCache(testWinRunDir, service, testWinClientConfig())
	defer cache.Close()

	var updated bool
	for i := 0; i < 200; i++ {
		if cache.Refresh() {
			updated = true
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if !updated {
		t.Fatal("Refresh never succeeded")
	}
	item, ok := cache.Lookup(1001, "docker-abc123")
	if !ok || item.Path != "/sys/fs/cgroup/docker/abc123" {
		t.Fatalf("unexpected cache item: %+v ok=%v", item, ok)
	}
}

func TestClientNotReadyReturnsErrorWindows(t *testing.T) {
	service := uniqueWinService("not_ready")
	client := NewClient(testWinRunDir, service, testWinClientConfig())
	defer client.Close()

	if _, err := client.CallSnapshot(); err != protocol.ErrBadLayout {
		t.Fatalf("CallSnapshot err = %v, want %v", err, protocol.ErrBadLayout)
	}
}
