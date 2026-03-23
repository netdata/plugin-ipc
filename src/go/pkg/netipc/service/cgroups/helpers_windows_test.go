//go:build windows

package cgroups

import (
	"fmt"
	"os"
	"sync/atomic"
	"testing"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	windows "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"
)

var winServiceCounter atomic.Uint64

func uniqueWinService(prefix string) string {
	return fmt.Sprintf("%s_%d_%d", prefix, os.Getpid(), winServiceCounter.Add(1))
}

func testWinShmServerConfig() windows.ServerConfig {
	cfg := testWinServerConfig()
	cfg.SupportedProfiles = protocol.ProfileBaseline | protocol.ProfileSHMHybrid
	cfg.PreferredProfiles = protocol.ProfileSHMHybrid
	return cfg
}

func testWinShmClientConfig() windows.ClientConfig {
	cfg := testWinClientConfig()
	cfg.SupportedProfiles = protocol.ProfileBaseline | protocol.ProfileSHMHybrid
	cfg.PreferredProfiles = protocol.ProfileSHMHybrid
	return cfg
}

func waitWinClientReady(t *testing.T, client *Client) {
	t.Helper()

	for i := 0; i < 100; i++ {
		client.Refresh()
		if client.Ready() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}

	t.Fatalf("client not ready, final state=%d", client.state)
}

func startTestServerWinWithConfig(service string, cfg windows.ServerConfig, handlers Handlers) *winTestServer {
	s := NewServer(winTestRunDir, service, cfg, handlers)
	doneCh := make(chan struct{})

	go func() {
		defer close(doneCh)
		_ = s.Run()
	}()

	time.Sleep(200 * time.Millisecond)

	return &winTestServer{server: s, doneCh: doneCh}
}

type winRawSessionServer struct {
	listener *windows.Listener
	doneCh   chan error
}

func startRawWinSessionServer(t *testing.T, service string, cfg windows.ServerConfig,
	handler func(*windows.Session, protocol.Header, []byte) error) *winRawSessionServer {
	return startRawWinSessionServerN(t, service, cfg, 1, handler)
}

func startRawWinSessionServerN(t *testing.T, service string, cfg windows.ServerConfig, accepts int,
	handler func(*windows.Session, protocol.Header, []byte) error) *winRawSessionServer {
	t.Helper()

	listener, err := windows.Listen(winTestRunDir, service, cfg)
	if err != nil {
		t.Fatalf("windows.Listen failed: %v", err)
	}

	srv := &winRawSessionServer{
		listener: listener,
		doneCh:   make(chan error, 1),
	}

	go func() {
		defer close(srv.doneCh)
		defer listener.Close()

		for i := 0; i < accepts; i++ {
			session, err := listener.Accept()
			if err != nil {
				srv.doneCh <- err
				return
			}

			recvBuf := make([]byte, protocol.HeaderSize+int(cfg.MaxRequestPayloadBytes))
			hdr, payload, err := session.Receive(recvBuf)
			if err != nil {
				session.Close()
				srv.doneCh <- err
				return
			}

			if err := handler(session, hdr, payload); err != nil {
				session.Close()
				srv.doneCh <- err
				return
			}

			session.Close()
		}

		srv.doneCh <- nil
	}()

	time.Sleep(200 * time.Millisecond)
	return srv
}

func (s *winRawSessionServer) wait(t *testing.T) {
	t.Helper()
	if err := <-s.doneCh; err != nil {
		t.Fatalf("raw windows session server failed: %v", err)
	}
}

func winTestCgroupsHandler(request *protocol.CgroupsRequest, builder *protocol.CgroupsBuilder) bool {
	if request.LayoutVersion != 1 || request.Flags != 0 {
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
}

func winFailingCgroupsHandler(*protocol.CgroupsRequest, *protocol.CgroupsBuilder) bool {
	return false
}

func winTestHandlers() Handlers {
	return Handlers{
		OnIncrement: func(v uint64) (uint64, bool) {
			return v + 1, true
		},
		OnStringReverse: func(s string) (string, bool) {
			return winReverseString(s), true
		},
		OnSnapshot:       winTestCgroupsHandler,
		SnapshotMaxItems: 3,
	}
}

func winFailingHandlers() Handlers {
	return Handlers{
		OnSnapshot:       winFailingCgroupsHandler,
		SnapshotMaxItems: 3,
	}
}
