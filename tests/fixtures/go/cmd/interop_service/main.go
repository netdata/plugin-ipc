//go:build unix

// L2 cross-language interop binary for the cgroups-snapshot service kind.
//
// Usage:
//
//	interop_service server <run_dir> <service_name>
//	  Starts a managed server for cgroups-snapshot only, prints READY,
//	  handles clients, exits after ~10s.
//
//	interop_service client <run_dir> <service_name>
//	  Connects, calls snapshot, verifies 3 items, prints PASS/FAIL.
package main

import (
	"fmt"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	appslookup "github.com/netdata/plugin-ipc/go/pkg/netipc/service/apps_lookup"
	cgroupslookup "github.com/netdata/plugin-ipc/go/pkg/netipc/service/cgroups_lookup"
	cgroups "github.com/netdata/plugin-ipc/go/pkg/netipc/service/cgroups_snapshot"
)

const (
	authToken                      = uint64(0xDEADBEEFCAFEBABE)
	responseBufSize                = 65536
	lookupScaleItemsDefault        = 8192
	lookupScaleRequestPayloadBytes = 8192
	lookupScaleCallTimeoutMs       = 120000
	lookupMixedItems               = 5
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
		PreferredProfiles:       profiles,
		MaxRequestBatchItems:    16,
		MaxResponsePayloadBytes: responseBufSize,
		AuthToken:               authToken,
	}
}

func clientConfig() cgroups.ClientConfig {
	profiles := detectProfiles()
	return cgroups.ClientConfig{
		SupportedProfiles:       profiles,
		PreferredProfiles:       profiles,
		MaxRequestBatchItems:    16,
		MaxResponsePayloadBytes: responseBufSize,
		AuthToken:               authToken,
	}
}

func lookupItemCount() int {
	raw := os.Getenv("NIPC_LOOKUP_SCALE_ITEMS")
	if raw == "" {
		return lookupScaleItemsDefault
	}
	value, err := strconv.Atoi(raw)
	if err != nil || value <= 0 || value > 65536 {
		return lookupScaleItemsDefault
	}
	return value
}

func lookupCountU32(value int) uint32 {
	return uint32(value) // #nosec G115 -- interop scale item counts are bounded to 1..65536.
}

func appsLookupServerConfig() appslookup.ServerConfig {
	profiles := detectProfiles()
	return appslookup.ServerConfig{
		SupportedProfiles:       profiles,
		PreferredProfiles:       profiles,
		MaxRequestPayloadBytes:  lookupScaleRequestPayloadBytes,
		MaxRequestBatchItems:    16,
		MaxResponsePayloadBytes: responseBufSize,
		AuthToken:               authToken,
	}
}

func appsLookupClientConfig(itemCount int) appslookup.ClientConfig {
	profiles := detectProfiles()
	return appslookup.ClientConfig{
		SupportedProfiles:             profiles,
		PreferredProfiles:             profiles,
		MaxRequestPayloadBytes:        lookupScaleRequestPayloadBytes,
		MaxRequestBatchItems:          16,
		MaxResponsePayloadBytes:       responseBufSize,
		CallTimeoutMs:                 lookupScaleCallTimeoutMs,
		AuthToken:                     authToken,
		MaxLogicalLookupItems:         lookupCountU32(itemCount),
		MaxLogicalLookupSubcalls:      4096,
		MaxLogicalLookupResponseBytes: 64 * 1024 * 1024,
	}
}

func cgroupsLookupServerConfig() cgroupslookup.ServerConfig {
	profiles := detectProfiles()
	return cgroupslookup.ServerConfig{
		SupportedProfiles:       profiles,
		PreferredProfiles:       profiles,
		MaxRequestPayloadBytes:  lookupScaleRequestPayloadBytes,
		MaxRequestBatchItems:    16,
		MaxResponsePayloadBytes: responseBufSize,
		AuthToken:               authToken,
	}
}

func cgroupsLookupClientConfig(itemCount int) cgroupslookup.ClientConfig {
	profiles := detectProfiles()
	return cgroupslookup.ClientConfig{
		SupportedProfiles:             profiles,
		PreferredProfiles:             profiles,
		MaxRequestPayloadBytes:        lookupScaleRequestPayloadBytes,
		MaxRequestBatchItems:          16,
		MaxResponsePayloadBytes:       responseBufSize,
		CallTimeoutMs:                 lookupScaleCallTimeoutMs,
		AuthToken:                     authToken,
		MaxLogicalLookupItems:         lookupCountU32(itemCount),
		MaxLogicalLookupSubcalls:      4096,
		MaxLogicalLookupResponseBytes: 64 * 1024 * 1024,
	}
}

func appsLookupHandler() appslookup.Handler {
	return appslookup.Handler{
		Handle: func(req *protocol.AppsLookupRequestView, builder *protocol.AppsLookupBuilder) bool {
			builder.SetGeneration(9)
			for i := uint32(0); i < req.ItemCount; i++ {
				pid, err := req.Item(i)
				if err != nil {
					return false
				}
				if err := builder.Add(
					protocol.PidLookupKnown,
					protocol.AppsCgroupKnown,
					protocol.OrchestratorDocker,
					pid,
					1,
					1000,
					42,
					[]byte("ok"),
					[]byte("/ok"),
					[]byte("name"),
					nil,
				); err != nil {
					return false
				}
			}
			return true
		},
	}
}

func cgroupsLookupHandler() cgroupslookup.Handler {
	return cgroupslookup.Handler{
		Handle: func(req *protocol.CgroupsLookupRequestView, builder *protocol.CgroupsLookupBuilder) bool {
			builder.SetGeneration(7)
			for i := uint32(0); i < req.ItemCount; i++ {
				path, err := req.Item(i)
				if err != nil {
					return false
				}
				if err := builder.Add(protocol.CgroupLookupKnown, protocol.OrchestratorK8s, path.Bytes(), []byte("ok"), nil); err != nil {
					return false
				}
			}
			return true
		},
	}
}

func appsLookupMixedHandler() appslookup.Handler {
	return appslookup.Handler{
		Handle: func(req *protocol.AppsLookupRequestView, builder *protocol.AppsLookupBuilder) bool {
			builder.SetGeneration(19)
			for i := uint32(0); i < req.ItemCount; i++ {
				pid, err := req.Item(i)
				if err != nil {
					return false
				}
				labels := []struct{ Key, Value []byte }{{Key: []byte("role"), Value: []byte("api")}}
				switch pid {
				case 1001:
					err = builder.Add(protocol.PidLookupKnown, protocol.AppsCgroupKnown, protocol.OrchestratorDocker, pid, 1, 1000, 42, []byte("known"), []byte("/cg/known"), []byte("pod-a"), labels)
				case 1002:
					err = builder.Add(protocol.PidLookupKnown, protocol.AppsCgroupHostRoot, 0, pid, 1, 1001, 43, []byte("host"), nil, nil, nil)
				case 1003:
					err = builder.Add(protocol.PidLookupUnknown, 0, 0, pid, 0, protocol.NipcUIDUnset, 0, nil, nil, nil, nil)
				case 1004:
					err = builder.Add(protocol.PidLookupOversizedItem, 0, 0, pid, 0, protocol.NipcUIDUnset, 0, nil, nil, nil, nil)
				default:
					err = builder.Add(protocol.PidLookupKnown, protocol.AppsCgroupUnknownRetryLater, 0, pid, 1, 1002, 44, []byte("retry"), nil, nil, nil)
				}
				if err != nil {
					return false
				}
			}
			return true
		},
	}
}

func cgroupsLookupMixedHandler() cgroupslookup.Handler {
	return cgroupslookup.Handler{
		Handle: func(req *protocol.CgroupsLookupRequestView, builder *protocol.CgroupsLookupBuilder) bool {
			builder.SetGeneration(17)
			for i := uint32(0); i < req.ItemCount; i++ {
				path, err := req.Item(i)
				if err != nil {
					return false
				}
				labels := []struct{ Key, Value []byte }{{Key: []byte("role"), Value: []byte("db")}}
				switch path.String() {
				case "/known":
					err = builder.Add(protocol.CgroupLookupKnown, protocol.OrchestratorK8s, path.Bytes(), []byte("pod-a"), labels)
				case "/retry":
					err = builder.Add(protocol.CgroupLookupUnknownRetryLater, 0, path.Bytes(), nil, nil)
				case "/permanent":
					err = builder.Add(protocol.CgroupLookupUnknownPermanent, 0, path.Bytes(), nil, nil)
				case "/oversized":
					err = builder.Add(protocol.CgroupLookupOversizedItem, 0, path.Bytes(), nil, nil)
				default:
					err = builder.Add(protocol.CgroupLookupKnown, protocol.OrchestratorDocker, path.Bytes(), []byte("pod-b"), nil)
				}
				if err != nil {
					return false
				}
			}
			return true
		},
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

func waitForSocket(runDir, service string, timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	path := fmt.Sprintf("%s/%s.sock", runDir, service)
	for time.Now().Before(deadline) {
		if info, err := os.Stat(path); err == nil && info.Mode()&os.ModeSocket != 0 { // #nosec G703 -- interop fixture probes caller-provided temporary socket path.
			return true
		}
		time.Sleep(10 * time.Millisecond)
	}
	return false
}

func runServer(runDir, service string) int {
	server := cgroups.NewServer(runDir, service, serverConfig(), testHandler())

	// Stop after 10s timeout (interop test should finish sooner)
	go func() {
		time.Sleep(10 * time.Second)
		server.Stop()
	}()

	serverErr := make(chan error, 1)
	go func() {
		serverErr <- server.Run()
	}()

	if !waitForSocket(runDir, service, 5*time.Second) {
		server.Stop()
		if err := <-serverErr; err != nil {
			fmt.Fprintf(os.Stderr, "server: %v\n", err)
		} else {
			fmt.Fprintf(os.Stderr, "server: socket not ready\n")
		}
		return 1
	}

	fmt.Println("READY")

	if err := <-serverErr; err != nil {
		fmt.Fprintf(os.Stderr, "server: %v\n", err)
		return 1
	}
	return 0
}

type stoppableServer interface {
	Run() error
	Stop()
}

func runLookupServer(server stoppableServer, runDir, service string) int {
	go func() {
		time.Sleep(10 * time.Second)
		server.Stop()
	}()

	serverErr := make(chan error, 1)
	go func() {
		serverErr <- server.Run()
	}()

	if !waitForSocket(runDir, service, 5*time.Second) {
		server.Stop()
		if err := <-serverErr; err != nil {
			fmt.Fprintf(os.Stderr, "server: %v\n", err)
		} else {
			fmt.Fprintf(os.Stderr, "server: socket not ready\n")
		}
		return 1
	}

	fmt.Println("READY")

	if err := <-serverErr; err != nil {
		fmt.Fprintf(os.Stderr, "server: %v\n", err)
		return 1
	}
	return 0
}

func runAppsServer(runDir, service string) int {
	return runLookupServer(
		appslookup.NewServerWithWorkers(runDir, service, appsLookupServerConfig(), appsLookupHandler(), 8),
		runDir,
		service,
	)
}

func runCgroupsLookupServer(runDir, service string) int {
	return runLookupServer(
		cgroupslookup.NewServerWithWorkers(runDir, service, cgroupsLookupServerConfig(), cgroupsLookupHandler(), 8),
		runDir,
		service,
	)
}

func runAppsMixedServer(runDir, service string) int {
	return runLookupServer(
		appslookup.NewServerWithWorkers(runDir, service, appsLookupServerConfig(), appsLookupMixedHandler(), 2),
		runDir,
		service,
	)
}

func runCgroupsMixedServer(runDir, service string) int {
	return runLookupServer(
		cgroupslookup.NewServerWithWorkers(runDir, service, cgroupsLookupServerConfig(), cgroupsLookupMixedHandler(), 2),
		runDir,
		service,
	)
}

func largeLookupPids(count int) []uint32 {
	pids := make([]uint32, count)
	var pid uint32 = 100000
	for i := range pids {
		pids[i] = pid
		pid++
	}
	return pids
}

func largeLookupPaths(count int) [][]byte {
	paths := make([][]byte, count)
	for i := range paths {
		paths[i] = []byte(fmt.Sprintf("/cg/%05d", i))
	}
	return paths
}

func waitAppsClientReady(client *appslookup.Client) bool {
	for i := 0; i < 200; i++ {
		client.Refresh()
		if client.Ready() {
			return true
		}
		time.Sleep(10 * time.Millisecond)
	}
	return false
}

func waitCgroupsLookupClientReady(client *cgroupslookup.Client) bool {
	for i := 0; i < 200; i++ {
		client.Refresh()
		if client.Ready() {
			return true
		}
		time.Sleep(10 * time.Millisecond)
	}
	return false
}

func runAppsClient(runDir, service string) int {
	itemCount := lookupItemCount()
	pids := largeLookupPids(itemCount)
	client := appslookup.NewClient(runDir, service, appsLookupClientConfig(itemCount))
	defer client.Close()
	if !waitAppsClientReady(client) {
		fmt.Fprintf(os.Stderr, "apps client: not ready\n")
		return 1
	}

	view, err := client.Call(pids)
	if err != nil {
		fmt.Fprintf(os.Stderr, "apps client: call failed: %v\n", err)
		fmt.Println("FAIL")
		return 1
	}
	if view.ItemCount != lookupCountU32(len(pids)) || view.Generation != 9 {
		fmt.Fprintf(os.Stderr, "apps client: bad header count=%d generation=%d\n", view.ItemCount, view.Generation)
		fmt.Println("FAIL")
		return 1
	}
	for i, expected := range pids {
		item, err := view.Item(lookupCountU32(i))
		if err != nil ||
			item.Status != protocol.PidLookupKnown ||
			item.Pid != expected ||
			item.Comm.String() != "ok" ||
			item.CgroupPath.String() != "/ok" {
			fmt.Fprintf(os.Stderr, "apps client: bad item %d\n", i)
			fmt.Println("FAIL")
			return 1
		}
	}

	fmt.Println("PASS")
	return 0
}

func runCgroupsLookupClient(runDir, service string) int {
	itemCount := lookupItemCount()
	paths := largeLookupPaths(itemCount)
	client := cgroupslookup.NewClient(runDir, service, cgroupsLookupClientConfig(itemCount))
	defer client.Close()
	if !waitCgroupsLookupClientReady(client) {
		fmt.Fprintf(os.Stderr, "cgroups lookup client: not ready\n")
		return 1
	}

	view, err := client.Call(paths)
	if err != nil {
		fmt.Fprintf(os.Stderr, "cgroups lookup client: call failed: %v\n", err)
		fmt.Println("FAIL")
		return 1
	}
	if view.ItemCount != lookupCountU32(len(paths)) || view.Generation != 7 {
		fmt.Fprintf(os.Stderr, "cgroups lookup client: bad header count=%d generation=%d\n", view.ItemCount, view.Generation)
		fmt.Println("FAIL")
		return 1
	}
	for i, expected := range paths {
		item, err := view.Item(lookupCountU32(i))
		if err != nil ||
			item.Status != protocol.CgroupLookupKnown ||
			item.Path.String() != string(expected) ||
			item.Name.String() != "ok" {
			fmt.Fprintf(os.Stderr, "cgroups lookup client: bad item %d\n", i)
			fmt.Println("FAIL")
			return 1
		}
	}

	fmt.Println("PASS")
	return 0
}

func runAppsMixedClient(runDir, service string) int {
	pids := []uint32{1001, 1002, 1003, 1004, 1005}
	client := appslookup.NewClient(runDir, service, appsLookupClientConfig(lookupMixedItems))
	defer client.Close()
	if !waitAppsClientReady(client) {
		fmt.Fprintf(os.Stderr, "apps mixed client: not ready\n")
		return 1
	}

	view, err := client.Call(pids)
	if err != nil {
		fmt.Fprintf(os.Stderr, "apps mixed client: call failed: %v\n", err)
		fmt.Println("FAIL")
		return 1
	}
	if view.ItemCount != lookupMixedItems || view.Generation != 19 {
		fmt.Fprintf(os.Stderr, "apps mixed client: bad header count=%d generation=%d\n", view.ItemCount, view.Generation)
		fmt.Println("FAIL")
		return 1
	}
	item0, err := view.Item(0)
	if err != nil || item0.Status != protocol.PidLookupKnown || item0.CgroupStatus != protocol.AppsCgroupKnown ||
		item0.Pid != 1001 || item0.Comm.String() != "known" || item0.CgroupPath.String() != "/cg/known" || item0.LabelCount != 1 {
		fmt.Fprintf(os.Stderr, "apps mixed client: bad item 0\n")
		fmt.Println("FAIL")
		return 1
	}
	label, err := item0.Label(0)
	if err != nil || label.Key.String() != "role" || label.Value.String() != "api" {
		fmt.Fprintf(os.Stderr, "apps mixed client: bad item 0 label\n")
		fmt.Println("FAIL")
		return 1
	}
	checks := []struct {
		index        uint32
		pid          uint32
		status       uint16
		cgroupStatus uint16
		comm         string
	}{
		{1, 1002, protocol.PidLookupKnown, protocol.AppsCgroupHostRoot, "host"},
		{2, 1003, protocol.PidLookupUnknown, 0, ""},
		{3, 1004, protocol.PidLookupOversizedItem, 0, ""},
		{4, 1005, protocol.PidLookupKnown, protocol.AppsCgroupUnknownRetryLater, "retry"},
	}
	for _, check := range checks {
		item, err := view.Item(check.index)
		if err != nil || item.Pid != check.pid || item.Status != check.status ||
			item.CgroupStatus != check.cgroupStatus || item.Comm.String() != check.comm {
			fmt.Fprintf(os.Stderr, "apps mixed client: bad item %d\n", check.index)
			fmt.Println("FAIL")
			return 1
		}
	}

	fmt.Println("PASS")
	return 0
}

func runCgroupsMixedClient(runDir, service string) int {
	paths := [][]byte{[]byte("/known"), []byte("/retry"), []byte("/permanent"), []byte("/oversized"), []byte("/known2")}
	client := cgroupslookup.NewClient(runDir, service, cgroupsLookupClientConfig(lookupMixedItems))
	defer client.Close()
	if !waitCgroupsLookupClientReady(client) {
		fmt.Fprintf(os.Stderr, "cgroups mixed client: not ready\n")
		return 1
	}

	view, err := client.Call(paths)
	if err != nil {
		fmt.Fprintf(os.Stderr, "cgroups mixed client: call failed: %v\n", err)
		fmt.Println("FAIL")
		return 1
	}
	if view.ItemCount != lookupMixedItems || view.Generation != 17 {
		fmt.Fprintf(os.Stderr, "cgroups mixed client: bad header count=%d generation=%d\n", view.ItemCount, view.Generation)
		fmt.Println("FAIL")
		return 1
	}
	item0, err := view.Item(0)
	if err != nil || item0.Status != protocol.CgroupLookupKnown ||
		item0.Path.String() != "/known" || item0.Name.String() != "pod-a" || item0.LabelCount != 1 {
		fmt.Fprintf(os.Stderr, "cgroups mixed client: bad item 0\n")
		fmt.Println("FAIL")
		return 1
	}
	label, err := item0.Label(0)
	if err != nil || label.Key.String() != "role" || label.Value.String() != "db" {
		fmt.Fprintf(os.Stderr, "cgroups mixed client: bad item 0 label\n")
		fmt.Println("FAIL")
		return 1
	}
	cases := []struct {
		index  uint32
		path   string
		status uint16
		name   string
	}{
		{1, "/retry", protocol.CgroupLookupUnknownRetryLater, ""},
		{2, "/permanent", protocol.CgroupLookupUnknownPermanent, ""},
		{3, "/oversized", protocol.CgroupLookupOversizedItem, ""},
		{4, "/known2", protocol.CgroupLookupKnown, "pod-b"},
	}
	for _, tc := range cases {
		item, err := view.Item(tc.index)
		if err != nil || item.Status != tc.status || item.Path.String() != tc.path || item.Name.String() != tc.name {
			fmt.Fprintf(os.Stderr, "cgroups mixed client: bad item %d\n", tc.index)
			fmt.Println("FAIL")
			return 1
		}
	}

	fmt.Println("PASS")
	return 0
}

func runClient(runDir, service string) int {
	client := cgroups.NewClient(runDir, service, clientConfig())
	for i := 0; i < 200; i++ {
		client.Refresh()
		if client.Ready() {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}

	if !client.Ready() {
		fmt.Fprintf(os.Stderr, "client: not ready\n")
		return 1
	}

	ok := true

	// --- Test CGROUPS_SNAPSHOT: 3 items ---
	view, err := client.CallSnapshot()
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

		// Verify first item
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
			if item0.Path.String() != "/sys/fs/cgroup/docker/abc123" {
				fmt.Fprintf(os.Stderr, "client: item 0 path: got %q\n", item0.Path.String())
				ok = false
			}
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
	signal.Ignore(syscall.SIGPIPE)

	if len(os.Args) != 4 {
		fmt.Fprintf(os.Stderr, "Usage: %s <server|client|apps-server|apps-client|cgroups-server|cgroups-client|apps-mixed-server|apps-mixed-client|cgroups-mixed-server|cgroups-mixed-client> <run_dir> <service_name>\n", os.Args[0])
		os.Exit(1)
	}

	mode := os.Args[1]
	runDir := os.Args[2]
	service := os.Args[3]

	if err := os.MkdirAll(runDir, 0700); err != nil { // #nosec G703 -- interop fixture uses caller-provided temporary run directory.
		fmt.Fprintf(os.Stderr, "mkdir %s: %v\n", runDir, err)
		os.Exit(1)
	}

	var rc int
	switch mode {
	case "server":
		rc = runServer(runDir, service)
	case "client":
		rc = runClient(runDir, service)
	case "apps-server":
		rc = runAppsServer(runDir, service)
	case "apps-client":
		rc = runAppsClient(runDir, service)
	case "cgroups-server":
		rc = runCgroupsLookupServer(runDir, service)
	case "cgroups-client":
		rc = runCgroupsLookupClient(runDir, service)
	case "apps-mixed-server":
		rc = runAppsMixedServer(runDir, service)
	case "apps-mixed-client":
		rc = runAppsMixedClient(runDir, service)
	case "cgroups-mixed-server":
		rc = runCgroupsMixedServer(runDir, service)
	case "cgroups-mixed-client":
		rc = runCgroupsMixedClient(runDir, service)
	default:
		fmt.Fprintf(os.Stderr, "Unknown mode: %s\n", mode)
		rc = 1
	}
	os.Exit(rc)
}
