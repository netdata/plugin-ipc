//go:build unix

package cgroupssnapshot

import (
	"errors"
	"net"
	"os"
	"path/filepath"
	"syscall"
	"testing"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix"
)

func fixedResponse(t *testing.T, messageID uint64) []byte {
	t.Helper()

	payload := make([]byte, 1024)
	builder, err := protocol.NewCgroupsSnapshotResponseBuilder(payload, 42, true, 3, 2)
	if err != nil {
		t.Fatalf("builder init failed: %v", err)
	}
	if err := builder.AddItem(protocol.CgroupsSnapshotItem{
		Hash:    123,
		Options: 0x2,
		Enabled: true,
		Name:    "system.slice-nginx",
		Path:    "/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs",
	}); err != nil {
		t.Fatalf("first item failed: %v", err)
	}
	if err := builder.AddItem(protocol.CgroupsSnapshotItem{
		Hash:    456,
		Options: 0x4,
		Enabled: false,
		Name:    "docker-1234",
		Path:    "",
	}); err != nil {
		t.Fatalf("second item failed: %v", err)
	}
	payloadLen, err := builder.Finish()
	if err != nil {
		t.Fatalf("builder finish failed: %v", err)
	}

	header, err := protocol.EncodeMessageHeader(protocol.MessageHeader{
		Magic:           protocol.MessageMagic,
		Version:         protocol.MessageVersion,
		HeaderLen:       protocol.MessageHeaderLen,
		Kind:            protocol.MessageKindResponse,
		Flags:           protocol.MessageFlagBatch,
		Code:            protocol.MethodCgroupsSnapshot,
		TransportStatus: protocol.TransportStatusOK,
		PayloadLen:      uint32(payloadLen),
		ItemCount:       2,
		MessageID:       messageID,
	})
	if err != nil {
		t.Fatalf("header encode failed: %v", err)
	}

	message := make([]byte, 0, protocol.MessageHeaderLen+payloadLen)
	message = append(message, header[:]...)
	message = append(message, payload[:payloadLen]...)
	return message
}

func spawnServer(t *testing.T, serviceNamespace, serviceName string) chan error {
	t.Helper()

	serverErr := make(chan error, 1)
	go func() {
		cfg := posix.NewConfig(serviceNamespace, serviceName)
		cfg.SupportedProfiles = posix.ProfileUDSSeqpacket
		cfg.PreferredProfiles = posix.ProfileUDSSeqpacket
		cfg.MaxRequestPayloadBytes = protocol.CgroupsSnapshotRequestPayloadLen
		cfg.MaxRequestBatchItems = 1
		cfg.MaxResponsePayloadBytes = DefaultMaxResponsePayloadBytes
		cfg.MaxResponseBatchItems = DefaultMaxResponseBatchItems

		server, err := posix.Listen(cfg)
		if err != nil {
			serverErr <- err
			return
		}
		defer server.Close()

		if err := server.Accept(5 * time.Second); err != nil {
			serverErr <- err
			return
		}

		requestCapacity, err := protocol.MaxBatchTotalSize(protocol.CgroupsSnapshotRequestPayloadLen, 1)
		if err != nil {
			serverErr <- err
			return
		}
		request := make([]byte, requestCapacity)
		requestLen, err := server.ReceiveMessage(request, 5*time.Second)
		if err != nil {
			serverErr <- err
			return
		}
		header, err := protocol.DecodeMessageHeader(request[:requestLen])
		if err != nil {
			serverErr <- err
			return
		}
		if header.Kind != protocol.MessageKindRequest || header.Code != protocol.MethodCgroupsSnapshot || header.ItemCount != 1 {
			serverErr <- errors.New("invalid request envelope")
			return
		}
		if _, err := protocol.DecodeCgroupsSnapshotRequestView(request[protocol.MessageHeaderLen:requestLen]); err != nil {
			serverErr <- err
			return
		}

		serverErr <- server.SendMessage(fixedResponse(t, header.MessageID), 5*time.Second)
	}()
	return serverErr
}

func TestRefreshPopulatesCacheAndLookup(t *testing.T) {
	serviceNamespace := filepath.Join(t.TempDir(), "sockdir")
	if err := os.MkdirAll(serviceNamespace, 0o755); err != nil {
		t.Fatalf("mkdir failed: %v", err)
	}
	serviceName := "cgroups-snapshot"
	serverErr := spawnServer(t, serviceNamespace, serviceName)
	time.Sleep(50 * time.Millisecond)

	client, err := NewClient(NewConfig(serviceNamespace, serviceName))
	if err != nil {
		t.Fatalf("NewClient failed: %v", err)
	}
	if err := client.Refresh(5 * time.Second); err != nil {
		t.Fatalf("Refresh failed: %v", err)
	}
	if err := <-serverErr; err != nil {
		t.Fatalf("server failed: %v", err)
	}

	cache := client.Cache()
	if cache.Generation != 42 || !cache.SystemdEnabled || len(cache.Items) != 2 {
		t.Fatalf("unexpected cache: %+v", cache)
	}
	item := client.Lookup(123, "system.slice-nginx")
	if item == nil {
		t.Fatalf("lookup failed")
	}
	if item.Path != "/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs" || !item.Enabled {
		t.Fatalf("unexpected item: %+v", item)
	}
}

func TestRefreshFailureKeepsPreviousCache(t *testing.T) {
	serviceNamespace := filepath.Join(t.TempDir(), "sockdir")
	if err := os.MkdirAll(serviceNamespace, 0o755); err != nil {
		t.Fatalf("mkdir failed: %v", err)
	}
	serviceName := "cgroups-snapshot"
	serverErr := spawnServer(t, serviceNamespace, serviceName)
	time.Sleep(50 * time.Millisecond)

	client, err := NewClient(NewConfig(serviceNamespace, serviceName))
	if err != nil {
		t.Fatalf("NewClient failed: %v", err)
	}
	if err := client.Refresh(5 * time.Second); err != nil {
		t.Fatalf("Refresh failed: %v", err)
	}
	if err := <-serverErr; err != nil {
		t.Fatalf("server failed: %v", err)
	}

	err = client.Refresh(200 * time.Millisecond)
	if err == nil {
		t.Fatalf("expected refresh failure after server exit")
	}
	var netErr *net.OpError
	if !errors.As(err, &netErr) && !errors.Is(err, os.ErrNotExist) && !errors.Is(err, syscall.EPIPE) &&
		!errors.Is(err, syscall.ECONNRESET) && !errors.Is(err, syscall.ENOTCONN) {
		t.Fatalf("unexpected refresh error: %T %v", err, err)
	}

	cache := client.Cache()
	if cache.Generation != 42 || len(cache.Items) != 2 {
		t.Fatalf("cache should remain populated: %+v", cache)
	}
	if client.Lookup(456, "docker-1234") == nil {
		t.Fatalf("expected cached lookup to survive refresh failure")
	}
}
