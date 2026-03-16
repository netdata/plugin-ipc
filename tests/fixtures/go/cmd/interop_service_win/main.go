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

// detectProfiles reads NIPC_PROFILE env var: "shm" enables SHM_HYBRID|BASELINE,
// default BASELINE only.
func detectProfiles() uint32 {
	if os.Getenv("NIPC_PROFILE") == "shm" {
		return windows.WinShmProfileHybrid | protocol.ProfileBaseline
	}
	return protocol.ProfileBaseline
}

func serverConfig() windows.ServerConfig {
	profiles := detectProfiles()
	return windows.ServerConfig{
		SupportedProfiles:       profiles,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    16,
		MaxResponsePayloadBytes: responseBufSize,
		MaxResponseBatchItems:   16,
		AuthToken:               authToken,
	}
}

func clientConfig() windows.ClientConfig {
	profiles := detectProfiles()
	return windows.ClientConfig{
		SupportedProfiles:       profiles,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    16,
		MaxResponsePayloadBytes: responseBufSize,
		MaxResponseBatchItems:   16,
		AuthToken:               authToken,
	}
}

// handleCgroups builds a snapshot with 3 test items.
func handleCgroups(request []byte) ([]byte, bool) {
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

// testHandler dispatches INCREMENT, CGROUPS_SNAPSHOT, and STRING_REVERSE.
func testHandler(methodCode uint16, request []byte) ([]byte, bool) {
	switch methodCode {
	case protocol.MethodIncrement:
		resp := make([]byte, protocol.IncrementPayloadSize)
		n, ok := protocol.DispatchIncrement(request, resp, func(v uint64) (uint64, bool) {
			return v + 1, true
		})
		if !ok {
			return nil, false
		}
		return resp[:n], true

	case protocol.MethodCgroupsSnapshot:
		return handleCgroups(request)

	case protocol.MethodStringReverse:
		resp := make([]byte, responseBufSize)
		n, ok := protocol.DispatchStringReverse(request, resp, func(s string) (string, bool) {
			runes := []rune(s)
			for i, j := 0, len(runes)-1; i < j; i, j = i+1, j-1 {
				runes[i], runes[j] = runes[j], runes[i]
			}
			return string(runes), true
		})
		if !ok {
			return nil, false
		}
		return resp[:n], true

	default:
		return nil, false
	}
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
	ok := true

	// --- Test INCREMENT: 42 -> 43 ---
	incResult, err := client.CallIncrement(42, respBuf)
	if err != nil {
		fmt.Fprintf(os.Stderr, "client: increment call failed: %v\n", err)
		ok = false
	} else if incResult != 43 {
		fmt.Fprintf(os.Stderr, "client: increment expected 43, got %d\n", incResult)
		ok = false
	}

	// --- Test CGROUPS_SNAPSHOT: 3 items ---
	view, err := client.CallSnapshot(respBuf)
	if err != nil {
		fmt.Fprintf(os.Stderr, "client: cgroups call failed: %v\n", err)
		ok = false
	} else {
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
	}

	// --- Test INCREMENT batch: [10,20,30] -> [11,21,31] ---
	batchResults, err := client.CallIncrementBatch([]uint64{10, 20, 30}, respBuf)
	if err != nil {
		fmt.Fprintf(os.Stderr, "client: increment batch call failed: %v\n", err)
		ok = false
	} else {
		expected := []uint64{11, 21, 31}
		if len(batchResults) != len(expected) {
			fmt.Fprintf(os.Stderr, "client: batch expected %d results, got %d\n",
				len(expected), len(batchResults))
			ok = false
		} else {
			for i, v := range batchResults {
				if v != expected[i] {
					fmt.Fprintf(os.Stderr, "client: batch[%d] expected %d, got %d\n",
						i, expected[i], v)
					ok = false
				}
			}
		}
	}

	// --- Test STRING_REVERSE: "hello" -> "olleh" ---
	srView, err := client.CallStringReverse("hello", respBuf)
	if err != nil {
		fmt.Fprintf(os.Stderr, "client: string_reverse call failed: %v\n", err)
		ok = false
	} else if srView.Str != "olleh" {
		fmt.Fprintf(os.Stderr, "client: string_reverse expected \"olleh\", got %q\n", srView.Str)
		ok = false
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
