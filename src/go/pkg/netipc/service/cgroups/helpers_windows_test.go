//go:build windows

package cgroups

import (
	"fmt"
	"os"
	"sync/atomic"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	windows "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"
)

var winServiceCounter atomic.Uint64

func uniqueWinService(prefix string) string {
	return fmt.Sprintf("%s_%d_%d", prefix, os.Getpid(), winServiceCounter.Add(1))
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
