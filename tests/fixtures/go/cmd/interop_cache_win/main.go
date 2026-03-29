//go:build windows

// L3 cross-language cache interop binary (Windows).
package main

import (
	"fmt"
	"os"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/service/cgroups"
)

const (
	authToken       = uint64(0xDEADBEEFCAFEBABE)
	responseBufSize = 65536
)

// detectProfiles reads NIPC_PROFILE env var: "shm" enables SHM_HYBRID|BASELINE,
// default BASELINE only.
func detectProfiles() uint32 {
	if os.Getenv("NIPC_PROFILE") == "shm" {
		return protocol.ProfileSHMHybrid | protocol.ProfileBaseline
	}
	return protocol.ProfileBaseline
}

func serverConfig() cgroups.ServerConfig {
	profiles := detectProfiles()
	return cgroups.ServerConfig{
		SupportedProfiles:       profiles,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    1,
		MaxResponsePayloadBytes: responseBufSize,
		MaxResponseBatchItems:   1,
		AuthToken:               authToken,
	}
}

func clientConfig() cgroups.ClientConfig {
	profiles := detectProfiles()
	return cgroups.ClientConfig{
		SupportedProfiles:       profiles,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    1,
		MaxResponsePayloadBytes: responseBufSize,
		MaxResponseBatchItems:   1,
		AuthToken:               authToken,
	}
}

func testHandler() cgroups.Handler {
	return cgroups.Handler{
		Handle: func(request *protocol.CgroupsRequest, builder *protocol.CgroupsBuilder) bool {
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
		},
		SnapshotMaxItems: 3,
	}
}

func runServer(runDir, service string) int {
	server := cgroups.NewServer(runDir, service, serverConfig(), testHandler())

	fmt.Println("READY")

	go func() {
		time.Sleep(10 * time.Second)
		server.Stop()
	}()

	if err := server.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "server: %v\n", err)
		return 1
	}
	return 0
}

func runClient(runDir, service string) int {
	cache := cgroups.NewCache(runDir, service, clientConfig())

	updated := false
	for i := 0; i < 200; i++ {
		if cache.Refresh() {
			updated = true
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if !updated || !cache.Ready() {
		fmt.Fprintf(os.Stderr, "client: cache not ready after refresh\n")
		cache.Close()
		fmt.Println("FAIL")
		return 1
	}

	ok := true

	status := cache.Status()
	if status.ItemCount != 3 {
		fmt.Fprintf(os.Stderr, "client: expected 3 items, got %d\n", status.ItemCount)
		ok = false
	}
	if status.SystemdEnabled != 1 {
		fmt.Fprintf(os.Stderr, "client: expected systemd_enabled=1, got %d\n", status.SystemdEnabled)
		ok = false
	}
	if status.Generation != 42 {
		fmt.Fprintf(os.Stderr, "client: expected generation=42, got %d\n", status.Generation)
		ok = false
	}

	item, found := cache.Lookup(1001, "docker-abc123")
	if !found {
		fmt.Fprintf(os.Stderr, "client: item 1001 not found\n")
		ok = false
	} else {
		if item.Hash != 1001 {
			fmt.Fprintf(os.Stderr, "client: item hash: got %d\n", item.Hash)
			ok = false
		}
		if item.Name != "docker-abc123" {
			fmt.Fprintf(os.Stderr, "client: item name mismatch\n")
			ok = false
		}
		if item.Path != "/sys/fs/cgroup/docker/abc123" {
			fmt.Fprintf(os.Stderr, "client: item path mismatch\n")
			ok = false
		}
	}

	_, notFound := cache.Lookup(9999, "nonexistent")
	if notFound {
		fmt.Fprintf(os.Stderr, "client: nonexistent item should not be found\n")
		ok = false
	}

	cache.Close()

	if ok {
		fmt.Println("PASS")
		return 0
	}
	fmt.Println("FAIL")
	return 1
}

func main() {
	if len(os.Args) != 4 {
		fmt.Fprintf(os.Stderr, "Usage: %s <server|client> <run_dir> <service_name>\n", os.Args[0])
		os.Exit(1)
	}

	mode := os.Args[1]
	runDir := os.Args[2]
	service := os.Args[3]

	var rc int
	switch mode {
	case "server":
		rc = runServer(runDir, service)
	case "client":
		rc = runClient(runDir, service)
	default:
		fmt.Fprintf(os.Stderr, "Unknown mode: %s\n", mode)
		rc = 1
	}
	os.Exit(rc)
}
