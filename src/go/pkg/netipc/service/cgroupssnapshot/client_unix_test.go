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

	items := []protocol.CgroupsSnapshotItem{
		{Hash: 123, Options: 0x2, Enabled: true, Name: "system.slice-nginx", Path: "/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs"},
		{Hash: 456, Options: 0x4, Enabled: false, Name: "docker-1234", Path: ""},
		{Hash: 789, Options: 0x6, Enabled: true, Name: "kubepods-burstable-pod01234567_89ab_cdef_0123_456789abcdef.slice", Path: "/sys/fs/cgroup/kubepods.slice/kubepods-burstable.slice/kubepods-burstable-pod01234567_89ab_cdef_0123_456789abcdef.slice/cgroup.procs"},
		{Hash: 1001, Options: 0x2, Enabled: true, Name: "system.slice-sshd.service", Path: "/sys/fs/cgroup/system.slice/sshd.service/cgroup.procs"},
		{Hash: 1002, Options: 0x2, Enabled: true, Name: "system.slice-docker.service", Path: "/sys/fs/cgroup/system.slice/docker.service/cgroup.procs"},
		{Hash: 1003, Options: 0x6, Enabled: true, Name: "user.slice-user-1000.slice-session-3.scope", Path: "/sys/fs/cgroup/user.slice/user-1000.slice/session-3.scope/cgroup.procs"},
		{Hash: 1004, Options: 0x2, Enabled: true, Name: "machine.slice-libvirt-qemu-5-win11.scope", Path: "/sys/fs/cgroup/machine.slice/libvirt-qemu-5-win11.scope/cgroup.procs"},
		{Hash: 1005, Options: 0x8, Enabled: false, Name: "system.slice-telegraf.service", Path: "/sys/fs/cgroup/system.slice/telegraf.service/cgroup.procs"},
		{Hash: 1006, Options: 0x6, Enabled: true, Name: "podman-7f0c8e91f1ce55b0c3d1b5a4f6e8d9c0.scope", Path: "/sys/fs/cgroup/system.slice/podman-7f0c8e91f1ce55b0c3d1b5a4f6e8d9c0.scope/cgroup.procs"},
		{Hash: 1007, Options: 0x4, Enabled: true, Name: "init.scope", Path: "/sys/fs/cgroup/init.scope/cgroup.procs"},
		{Hash: 1008, Options: 0x6, Enabled: true, Name: "system.slice-containerd.service", Path: "/sys/fs/cgroup/system.slice/containerd.service/cgroup.procs"},
		{Hash: 1009, Options: 0x4, Enabled: true, Name: "machine.slice-systemd-nspawn-observability-lab.scope", Path: "/sys/fs/cgroup/machine.slice/systemd-nspawn-observability-lab.scope/cgroup.procs"},
		{Hash: 1010, Options: 0x6, Enabled: true, Name: "user.slice-user-1001.slice-user@1001.service-app.slice-observability-frontend.scope", Path: "/sys/fs/cgroup/user.slice/user-1001.slice/user@1001.service/app.slice/observability-frontend.scope/cgroup.procs"},
		{Hash: 1011, Options: 0x1, Enabled: false, Name: "crio-53d2b1b5d7a04d8f9e2f6a7b8c9d0e1f.scope", Path: "/sys/fs/cgroup/kubepods.slice/kubepods-pod98765432_10fe_dcba_9876_543210fedcba.slice/crio-53d2b1b5d7a04d8f9e2f6a7b8c9d0e1f.scope/cgroup.procs"},
		{Hash: 1012, Options: 0x2, Enabled: true, Name: "system.slice-netdata.service", Path: "/sys/fs/cgroup/system.slice/netdata.service/cgroup.procs"},
		{Hash: 1013, Options: 0x6, Enabled: true, Name: "system.slice-super-long-observability-ingestion-gateway-with-really-long-unit-name-to-stress-view-lifetimes.service", Path: "/sys/fs/cgroup/system.slice/super-long-observability-ingestion-gateway-with-really-long-unit-name-to-stress-view-lifetimes.service/cgroup.procs"},
	}
	payloadLenCap, err := protocol.MaxBatchPayloadLen(DefaultMaxResponsePayloadBytes, uint32(len(items)))
	if err != nil {
		t.Fatalf("MaxBatchPayloadLen() error = %v", err)
	}
	payload := make([]byte, payloadLenCap)
	builder, err := protocol.NewCgroupsSnapshotResponseBuilder(payload, 42, true, 3, uint32(len(items)))
	if err != nil {
		t.Fatalf("builder init failed: %v", err)
	}
	for idx, item := range items {
		if err := builder.AddItem(item); err != nil {
			t.Fatalf("item %d failed: %v", idx, err)
		}
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
		ItemCount:       uint32(len(items)),
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

func malformedButTransportValidResponse(t *testing.T, messageID uint64) []byte {
	t.Helper()

	message := fixedResponse(t, messageID)
	base := protocol.MessageHeaderLen + protocol.CgroupsSnapshotResponseHeaderLen
	if len(message) < base+4 {
		t.Fatalf("fixed response too short to corrupt")
	}

	// Keep the outer envelope valid but corrupt the first item reference so
	// payload-level decoding fails after transport validation succeeds.
	message[base+0] = 1
	message[base+1] = 0
	message[base+2] = 0
	message[base+3] = 0
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

func spawnServerWithProfiles(
	t *testing.T,
	serviceNamespace, serviceName string,
	supportedProfiles, preferredProfiles uint32,
) chan error {
	t.Helper()

	serverErr := make(chan error, 1)
	go func() {
		cfg := posix.NewConfig(serviceNamespace, serviceName)
		cfg.SupportedProfiles = supportedProfiles
		cfg.PreferredProfiles = preferredProfiles
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

func spawnResponseSequenceServer(
	t *testing.T,
	serviceNamespace, serviceName string,
	supportedProfiles, preferredProfiles uint32,
	responses [][]byte,
) chan error {
	t.Helper()

	serverErr := make(chan error, 1)
	go func() {
		cfg := posix.NewConfig(serviceNamespace, serviceName)
		cfg.SupportedProfiles = supportedProfiles
		cfg.PreferredProfiles = preferredProfiles
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

		for _, response := range responses {
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
			if err := server.SendMessage(response, 5*time.Second); err != nil {
				serverErr <- err
				return
			}
		}

		serverErr <- nil
	}()
	return serverErr
}

func strictClientConfig(serviceNamespace, serviceName string) Config {
	cfg := NewConfig(serviceNamespace, serviceName)
	cfg.SupportedProfiles = posix.ProfileUDSSeqpacket
	cfg.PreferredProfiles = posix.ProfileUDSSeqpacket
	cfg.MaxResponsePayloadBytes = DefaultMaxResponsePayloadBytes
	cfg.MaxResponseBatchItems = DefaultMaxResponseBatchItems
	cfg.AuthToken = 1
	return cfg
}

func strictClientConfigWithProfiles(
	serviceNamespace, serviceName string,
	supportedProfiles, preferredProfiles uint32,
) Config {
	cfg := strictClientConfig(serviceNamespace, serviceName)
	cfg.SupportedProfiles = supportedProfiles
	cfg.PreferredProfiles = preferredProfiles
	return cfg
}

func TestRefreshPopulatesCacheAndLookup(t *testing.T) {
	serviceNamespace := filepath.Join(t.TempDir(), "sockdir")
	if err := os.MkdirAll(serviceNamespace, 0o755); err != nil {
		t.Fatalf("mkdir failed: %v", err)
	}
	serviceName := "cgroups-snapshot"
	serverErr := spawnServer(t, serviceNamespace, serviceName)
	time.Sleep(50 * time.Millisecond)

	client, err := NewClient(strictClientConfig(serviceNamespace, serviceName))
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
	if cache.Generation != 42 || !cache.SystemdEnabled || len(cache.Items) != 16 {
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

	client, err := NewClient(strictClientConfig(serviceNamespace, serviceName))
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
	if cache.Generation != 42 || len(cache.Items) != 16 {
		t.Fatalf("cache should remain populated: %+v", cache)
	}
	if client.Lookup(456, "docker-1234") == nil {
		t.Fatalf("expected cached lookup to survive refresh failure")
	}
}

func TestRefreshFailureKeepsPreviousCacheOverSHM(t *testing.T) {
	serviceNamespace := filepath.Join(t.TempDir(), "sockdir")
	if err := os.MkdirAll(serviceNamespace, 0o755); err != nil {
		t.Fatalf("mkdir failed: %v", err)
	}
	serviceName := "cgroups-snapshot"
	supported := posix.ProfileUDSSeqpacket | posix.ProfileSHMHybrid
	preferred := posix.ProfileSHMHybrid
	serverErr := spawnServerWithProfiles(t, serviceNamespace, serviceName, supported, preferred)
	time.Sleep(50 * time.Millisecond)

	client, err := NewClient(strictClientConfigWithProfiles(serviceNamespace, serviceName, supported, preferred))
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

	cache := client.Cache()
	if cache.Generation != 42 || len(cache.Items) != 16 {
		t.Fatalf("cache should remain populated: %+v", cache)
	}
	if client.Lookup(456, "docker-1234") == nil {
		t.Fatalf("expected cached lookup to survive refresh failure")
	}
}

func TestMalformedResponseKeepsPreviousCacheOverSHM(t *testing.T) {
	serviceNamespace := filepath.Join(t.TempDir(), "sockdir")
	if err := os.MkdirAll(serviceNamespace, 0o755); err != nil {
		t.Fatalf("mkdir failed: %v", err)
	}
	serviceName := "cgroups-snapshot"
	supported := posix.ProfileUDSSeqpacket | posix.ProfileSHMHybrid
	preferred := posix.ProfileSHMHybrid
	serverErr := spawnResponseSequenceServer(
		t,
		serviceNamespace,
		serviceName,
		supported,
		preferred,
		[][]byte{
			fixedResponse(t, 1),
			malformedButTransportValidResponse(t, 2),
		},
	)
	time.Sleep(50 * time.Millisecond)

	client, err := NewClient(strictClientConfigWithProfiles(serviceNamespace, serviceName, supported, preferred))
	if err != nil {
		t.Fatalf("NewClient failed: %v", err)
	}
	if err := client.Refresh(5 * time.Second); err != nil {
		t.Fatalf("first Refresh failed: %v", err)
	}
	if err := client.Refresh(5 * time.Second); err == nil {
		t.Fatalf("expected malformed response to fail")
	}
	if err := <-serverErr; err != nil {
		t.Fatalf("server failed: %v", err)
	}

	cache := client.Cache()
	if cache.Generation != 42 || len(cache.Items) != 16 {
		t.Fatalf("cache should remain populated after malformed response: %+v", cache)
	}
	if client.Lookup(123, "system.slice-nginx") == nil {
		t.Fatalf("expected cached lookup to survive malformed response")
	}
}
