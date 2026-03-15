//go:build windows

// L2 cross-language interop binary (Windows).
package main

import (
	"fmt"
	"os"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/service/cgroups"
	windows "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"
)

const (
	authToken       = uint64(0xDEADBEEFCAFEBABE)
	responseBufSize = 65536
)

func serverConfig() windows.ServerConfig {
	return windows.ServerConfig{
		SupportedProfiles:       protocol.ProfileBaseline,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    1,
		MaxResponsePayloadBytes: responseBufSize,
		MaxResponseBatchItems:   1,
		AuthToken:               authToken,
	}
}

func clientConfig() windows.ClientConfig {
	return windows.ClientConfig{
		SupportedProfiles:       protocol.ProfileBaseline,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    1,
		MaxResponsePayloadBytes: responseBufSize,
		MaxResponseBatchItems:   1,
		AuthToken:               authToken,
	}
}

func testHandler(methodCode uint16, request []byte) ([]byte, bool) {
	if methodCode != protocol.MethodCgroupsSnapshot {
		return nil, false
	}
	if _, err := protocol.DecodeCgroupsRequest(request); err != nil {
		return nil, false
	}

	buf := make([]byte, responseBufSize)
	builder := protocol.NewCgroupsBuilder(buf, 3, 1, 42)

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
			return nil, false
		}
	}

	total := builder.Finish()
	return buf[:total], true
}

func runServer(runDir, service string) int {
	server := cgroups.NewServer(runDir, service, serverConfig(), testHandler)

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
	client := cgroups.NewClient(runDir, service, clientConfig())
	client.Refresh()

	if !client.Ready() {
		fmt.Fprintf(os.Stderr, "client: not ready\n")
		return 1
	}

	respBuf := make([]byte, responseBufSize)
	view, err := client.CallSnapshot(respBuf)
	if err != nil {
		fmt.Fprintf(os.Stderr, "client: call failed: %v\n", err)
		fmt.Println("FAIL")
		return 1
	}

	ok := true
	if view.ItemCount != 3 {
		fmt.Fprintf(os.Stderr, "client: expected 3 items, got %d\n", view.ItemCount)
		ok = false
	}
	if view.SystemdEnabled != 1 {
		fmt.Fprintf(os.Stderr, "client: expected systemd_enabled=1, got %d\n", view.SystemdEnabled)
		ok = false
	}
	if view.Generation != 42 {
		fmt.Fprintf(os.Stderr, "client: expected generation=42, got %d\n", view.Generation)
		ok = false
	}

	item0, ierr := view.Item(0)
	if ierr != nil {
		fmt.Fprintf(os.Stderr, "client: item 0 error: %v\n", ierr)
		ok = false
	} else {
		if item0.Hash != 1001 {
			fmt.Fprintf(os.Stderr, "client: item 0 hash: got %d\n", item0.Hash)
			ok = false
		}
		if item0.Name.String() != "docker-abc123" {
			fmt.Fprintf(os.Stderr, "client: item 0 name: got %q\n", item0.Name.String())
			ok = false
		}
	}

	client.Close()

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
