//go:build windows

// bench_windows - Windows benchmark driver for netipc (Go).
//
// Exercises Named Pipe and Win SHM transports. Measures throughput,
// latency (p50/p95/p99), and CPU.
//
// Same output format as the C driver.
// Pure Go — no cgo. Works with CGO_ENABLED=0.
package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"runtime"
	"sort"
	"strconv"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/service/cgroups"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"
)

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

const (
	authTokenWin         = uint64(0xBE4C400000C0FFEE)
	responseBufSizeWin   = 65536
	maxLatencySamplesWin = 10_000_000
	defaultDurationWin   = 30
	profileNP            = protocol.ProfileBaseline
	profileWinSHM        = protocol.ProfileBaseline | 0x02 // PROFILE_SHM_HYBRID
)

// ---------------------------------------------------------------------------
//  Timing helpers
// ---------------------------------------------------------------------------

var qpcFreq int64

func init() {
	var freq int64
	syscall.Syscall(procQueryPerformanceFrequency.Addr(), 1, uintptr(unsafe.Pointer(&freq)), 0, 0)
	qpcFreq = freq
}

var (
	kernel32                       = syscall.NewLazyDLL("kernel32.dll")
	procQueryPerformanceFrequency  = kernel32.NewProc("QueryPerformanceFrequency")
	procQueryPerformanceCounter    = kernel32.NewProc("QueryPerformanceCounter")
	procGetProcessTimes            = kernel32.NewProc("GetProcessTimes")
)

func nowNS() uint64 {
	var counter int64
	syscall.Syscall(procQueryPerformanceCounter.Addr(), 1, uintptr(unsafe.Pointer(&counter)), 0, 0)
	return uint64(counter) * 1_000_000_000 / uint64(qpcFreq)
}

func cpuNSWin() uint64 {
	var creation, exit, kernel, user syscall.Filetime
	h, _ := syscall.GetCurrentProcess()
	syscall.Syscall6(procGetProcessTimes.Addr(), 5,
		uintptr(h),
		uintptr(unsafe.Pointer(&creation)),
		uintptr(unsafe.Pointer(&exit)),
		uintptr(unsafe.Pointer(&kernel)),
		uintptr(unsafe.Pointer(&user)),
		0)
	k := uint64(kernel.HighDateTime)<<32 | uint64(kernel.LowDateTime)
	u := uint64(user.HighDateTime)<<32 | uint64(user.LowDateTime)
	return (k + u) * 100 // FILETIME is 100ns intervals
}

// ---------------------------------------------------------------------------
//  Latency recorder
// ---------------------------------------------------------------------------

type latencyRecorderWin struct {
	samples []uint64
	cap     int
}

func newLatencyRecorderWin(cap int) *latencyRecorderWin {
	if cap > maxLatencySamplesWin {
		cap = maxLatencySamplesWin
	}
	return &latencyRecorderWin{
		samples: make([]uint64, 0, cap),
		cap:     cap,
	}
}

func (lr *latencyRecorderWin) record(ns uint64) {
	if len(lr.samples) < lr.cap {
		lr.samples = append(lr.samples, ns)
	}
}

func (lr *latencyRecorderWin) percentile(pct float64) uint64 {
	if len(lr.samples) == 0 {
		return 0
	}
	sort.Slice(lr.samples, func(i, j int) bool { return lr.samples[i] < lr.samples[j] })
	idx := int(pct / 100.0 * float64(len(lr.samples)-1))
	if idx >= len(lr.samples) {
		idx = len(lr.samples) - 1
	}
	return lr.samples[idx]
}

// ---------------------------------------------------------------------------
//  Rate limiter
// ---------------------------------------------------------------------------

type rateLimiterWin struct {
	interval time.Duration
	next     time.Time
	limited  bool
}

func newRateLimiterWin(targetRPS uint64) *rateLimiterWin {
	if targetRPS == 0 {
		return &rateLimiterWin{limited: false}
	}
	return &rateLimiterWin{
		interval: time.Duration(1_000_000_000/targetRPS) * time.Nanosecond,
		next:     time.Now(),
		limited:  true,
	}
}

func (rl *rateLimiterWin) wait() {
	if !rl.limited {
		return
	}
	now := time.Now()
	if now.Before(rl.next) {
		time.Sleep(rl.next.Sub(now))
	}
	rl.next = rl.next.Add(rl.interval)
}

// ---------------------------------------------------------------------------
//  Config helpers
// ---------------------------------------------------------------------------

func serverConfigWin(profiles uint32) windows.ServerConfig {
	return windows.ServerConfig{
		SupportedProfiles:       profiles,
		PreferredProfiles:       profiles,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    1,
		MaxResponsePayloadBytes: responseBufSizeWin,
		MaxResponseBatchItems:   1,
		AuthToken:               authTokenWin,
	}
}

func clientConfigWin(profiles uint32) windows.ClientConfig {
	return windows.ClientConfig{
		SupportedProfiles:       profiles,
		PreferredProfiles:       profiles,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    1,
		MaxResponsePayloadBytes: responseBufSizeWin,
		MaxResponseBatchItems:   1,
		AuthToken:               authTokenWin,
	}
}

// ---------------------------------------------------------------------------
//  Ping-pong handler
// ---------------------------------------------------------------------------

func pingPongHandlerWin(methodCode uint16, request []byte) ([]byte, bool) {
	if methodCode != protocol.MethodIncrement {
		return nil, false
	}
	if len(request) < 8 {
		return nil, false
	}
	counter := binary.LittleEndian.Uint64(request[:8])
	counter++
	resp := make([]byte, 8)
	binary.LittleEndian.PutUint64(resp, counter)
	return resp, true
}

// ---------------------------------------------------------------------------
//  Snapshot handler
// ---------------------------------------------------------------------------

var snapshotGenWin uint64

func snapshotHandlerWin(methodCode uint16, request []byte) ([]byte, bool) {
	if methodCode != protocol.MethodCgroupsSnapshot {
		return nil, false
	}
	if _, err := protocol.DecodeCgroupsRequest(request); err != nil {
		return nil, false
	}

	gen := atomic.AddUint64(&snapshotGenWin, 1)
	buf := make([]byte, responseBufSizeWin)
	builder := protocol.NewCgroupsBuilder(buf, 16, 1, gen)

	for i := uint32(0); i < 16; i++ {
		name := fmt.Sprintf("cgroup-%d", i)
		path := fmt.Sprintf("/sys/fs/cgroup/bench/cg-%d", i)
		if err := builder.Add(1000+i, 0, i%2, []byte(name), []byte(path)); err != nil {
			return nil, false
		}
	}

	total := builder.Finish()
	return buf[:total], true
}

// ---------------------------------------------------------------------------
//  Server
// ---------------------------------------------------------------------------

func runServerWin(runDir, service string, profiles uint32, durationSec int, handlerType string) int {
	var handler cgroups.HandlerFunc
	switch handlerType {
	case "ping-pong":
		handler = pingPongHandlerWin
	case "snapshot":
		handler = snapshotHandlerWin
	default:
		fmt.Fprintf(os.Stderr, "unknown handler type: %s\n", handlerType)
		return 1
	}

	server := cgroups.NewServer(runDir, service, serverConfigWin(profiles), handler)

	fmt.Println("READY")

	cpuStart := cpuNSWin()

	go func() {
		time.Sleep(time.Duration(durationSec+3) * time.Second)
		server.Stop()
	}()

	if err := server.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "server: %v\n", err)
	}

	cpuEnd := cpuNSWin()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9

	fmt.Printf("SERVER_CPU_SEC=%.6f\n", cpuSec)
	return 0
}

// ---------------------------------------------------------------------------
//  Ping-pong client
// ---------------------------------------------------------------------------

func runPingPongClientWin(runDir, service string, profiles uint32, durationSec int, targetRPS uint64, scenario, serverLang string) int {
	cfg := clientConfigWin(profiles)
	var session *windows.Session
	var err error

	for i := 0; i < 200; i++ {
		session, err = windows.Connect(runDir, service, &cfg)
		if err == nil {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "client: connect failed: %v\n", err)
		return 1
	}
	defer session.Close()

	// Win SHM upgrade if negotiated
	var shm *windows.WinShmContext
	if session.SelectedProfile&0x02 != 0 { // PROFILE_SHM_HYBRID
		for i := 0; i < 200; i++ {
			shmCtx, serr := windows.WinShmClientAttach(runDir, service, authTokenWin, session.SessionID, 0x02)
			if serr == nil {
				shm = shmCtx
				break
			}
			time.Sleep(5 * time.Millisecond)
		}
	}
	if shm != nil {
		defer shm.WinShmClose()
	}

	estSamples := maxLatencySamplesWin
	if targetRPS > 0 {
		estSamples = int(targetRPS) * durationSec
	}
	if estSamples > maxLatencySamplesWin {
		estSamples = maxLatencySamplesWin
	}
	lr := newLatencyRecorderWin(estSamples)
	rl := newRateLimiterWin(targetRPS)

	var counter, requests, errors uint64

	cpuStart := cpuNSWin()
	wallStart := time.Now()
	deadline := time.Duration(durationSec) * time.Second

	for time.Since(wallStart) < deadline {
		rl.wait()

		reqPayload := make([]byte, 8)
		binary.LittleEndian.PutUint64(reqPayload, counter)

		hdr := protocol.Header{
			Kind:            protocol.KindRequest,
			Code:            protocol.MethodIncrement,
			Flags:           0,
			ItemCount:       1,
			MessageID:       counter + 1,
			TransportStatus: protocol.StatusOK,
			PayloadLen:      8,
		}

		t0 := time.Now()

		if shm != nil {
			// Win SHM path
			msgLen := protocol.HeaderSize + 8
			msg := make([]byte, msgLen)
			hdr.Magic = protocol.MagicMsg
			hdr.Version = protocol.Version
			hdr.HeaderLen = protocol.HeaderLen
			hdr.Encode(msg[:protocol.HeaderSize])
			copy(msg[protocol.HeaderSize:], reqPayload)

			if err := shm.WinShmSend(msg); err != nil {
				errors++
				continue
			}

			respBuf := make([]byte, protocol.HeaderSize+64)
			respLen, err := shm.WinShmReceive(respBuf, 30000)
			if err != nil {
				errors++
				continue
			}
			if respLen >= protocol.HeaderSize+8 {
				respVal := binary.LittleEndian.Uint64(respBuf[protocol.HeaderSize : protocol.HeaderSize+8])
				if respVal != counter+1 {
					fmt.Fprintf(os.Stderr, "counter chain broken: expected %d, got %d\n", counter+1, respVal)
					errors++
				}
			}
		} else {
			// Named Pipe path
			if err := session.Send(&hdr, reqPayload); err != nil {
				errors++
				continue
			}

			recvBuf := make([]byte, 256)
			_, payload, err := session.Receive(recvBuf)
			if err != nil {
				errors++
				continue
			}
			if len(payload) >= 8 {
				respVal := binary.LittleEndian.Uint64(payload[:8])
				if respVal != counter+1 {
					fmt.Fprintf(os.Stderr, "counter chain broken: expected %d, got %d\n", counter+1, respVal)
					errors++
				}
			}
		}

		t1 := time.Now()
		lr.record(uint64(t1.Sub(t0).Nanoseconds()))

		counter++
		requests++
	}

	wallSec := time.Since(wallStart).Seconds()
	cpuEnd := cpuNSWin()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9
	throughput := float64(requests) / wallSec
	cpuPct := (cpuSec / wallSec) * 100.0

	p50 := lr.percentile(50.0) / 1000
	p95 := lr.percentile(95.0) / 1000
	p99 := lr.percentile(99.0) / 1000

	fmt.Printf("%s,go,%s,%.0f,%d,%d,%d,%.1f,0.0,%.1f\n",
		scenario, serverLang, throughput, p50, p95, p99, cpuPct, cpuPct)

	if errors > 0 {
		fmt.Fprintf(os.Stderr, "client: %d errors\n", errors)
		return 1
	}
	return 0
}

// ---------------------------------------------------------------------------
//  Snapshot client
// ---------------------------------------------------------------------------

func runSnapshotClientWin(runDir, service string, profiles uint32, durationSec int, targetRPS uint64, scenario, serverLang string) int {
	cfg := clientConfigWin(profiles)
	client := cgroups.NewClient(runDir, service, cfg)

	for i := 0; i < 200; i++ {
		client.Refresh()
		if client.Ready() {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}

	if !client.Ready() {
		fmt.Fprintf(os.Stderr, "client: not ready after retries\n")
		return 1
	}

	estSamples := maxLatencySamplesWin
	if targetRPS > 0 {
		estSamples = int(targetRPS) * durationSec
	}
	if estSamples > maxLatencySamplesWin {
		estSamples = maxLatencySamplesWin
	}
	lr := newLatencyRecorderWin(estSamples)
	rl := newRateLimiterWin(targetRPS)

	var requests, errors uint64

	cpuStart := cpuNSWin()
	wallStart := time.Now()
	deadline := time.Duration(durationSec) * time.Second
	respBuf := make([]byte, responseBufSizeWin)

	for time.Since(wallStart) < deadline {
		rl.wait()

		t0 := time.Now()

		view, err := client.CallSnapshot(respBuf)
		t1 := time.Now()

		if err != nil {
			errors++
			client.Refresh()
			continue
		}

		if view.ItemCount != 16 {
			fmt.Fprintf(os.Stderr, "snapshot: expected 16 items, got %d\n", view.ItemCount)
			errors++
		}

		lr.record(uint64(t1.Sub(t0).Nanoseconds()))
		requests++
	}

	wallSec := time.Since(wallStart).Seconds()
	cpuEnd := cpuNSWin()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9
	throughput := float64(requests) / wallSec
	cpuPct := (cpuSec / wallSec) * 100.0

	p50 := lr.percentile(50.0) / 1000
	p95 := lr.percentile(95.0) / 1000
	p99 := lr.percentile(99.0) / 1000

	fmt.Printf("%s,go,%s,%.0f,%d,%d,%d,%.1f,0.0,%.1f\n",
		scenario, serverLang, throughput, p50, p95, p99, cpuPct, cpuPct)

	client.Close()

	if errors > 0 {
		fmt.Fprintf(os.Stderr, "client: %d errors\n", errors)
		return 1
	}
	return 0
}

// ---------------------------------------------------------------------------
//  Lookup benchmark
// ---------------------------------------------------------------------------

func runLookupBenchWin(durationSec int) int {
	type item struct {
		hash    uint32
		name    string
		options uint32
		enabled uint32
		path    string
	}

	items := make([]item, 16)
	for i := range items {
		items[i] = item{
			hash:    uint32(1000 + i),
			options: 0,
			enabled: uint32(i % 2),
			name:    fmt.Sprintf("cgroup-%d", i),
			path:    fmt.Sprintf("/sys/fs/cgroup/bench/cg-%d", i),
		}
	}

	cacheItems := make([]cgroups.CacheItem, 16)
	for i, it := range items {
		cacheItems[i] = cgroups.CacheItem{
			Hash:    it.hash,
			Options: it.options,
			Enabled: it.enabled,
			Name:    it.name,
			Path:    it.path,
		}
	}

	var lookups, hits uint64

	cpuStart := cpuNSWin()
	wallStart := time.Now()
	deadline := time.Duration(durationSec) * time.Second

	for time.Since(wallStart) < deadline {
		for _, it := range items {
			for j := range cacheItems {
				if cacheItems[j].Hash == it.hash && cacheItems[j].Name == it.name {
					hits++
					break
				}
			}
			lookups++
		}
	}

	wallSec := time.Since(wallStart).Seconds()
	cpuEnd := cpuNSWin()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9
	throughput := float64(lookups) / wallSec
	cpuPct := (cpuSec / wallSec) * 100.0

	fmt.Printf("lookup,go,go,%.0f,0,0,0,%.1f,0.0,%.1f\n", throughput, cpuPct, cpuPct)

	if hits != lookups {
		fmt.Fprintf(os.Stderr, "lookup: missed %d/%d\n", lookups-hits, lookups)
		return 1
	}
	return 0
}

// ---------------------------------------------------------------------------
//  Main
// ---------------------------------------------------------------------------

func main() {
	runtime.GOMAXPROCS(1)

	if len(os.Args) < 2 {
		fmt.Fprintf(os.Stderr,
			"Usage: %s <subcommand> [args...]\n"+
				"Subcommands: np-ping-pong-{server,client}, shm-ping-pong-{server,client},\n"+
				"snapshot-{server,client}, snapshot-shm-{server,client}, lookup-bench\n",
			os.Args[0])
		os.Exit(1)
	}

	cmd := os.Args[1]
	rc := 0

	switch cmd {
	case "np-ping-pong-server", "shm-ping-pong-server",
		"snapshot-server", "snapshot-shm-server":
		if len(os.Args) < 4 {
			fmt.Fprintf(os.Stderr, "Usage: %s %s <run_dir> <service> [duration_sec]\n", os.Args[0], cmd)
			os.Exit(1)
		}
		runDir := os.Args[2]
		service := os.Args[3]
		duration := defaultDurationWin
		if len(os.Args) >= 5 {
			if d, err := strconv.Atoi(os.Args[4]); err == nil {
				duration = d
			}
		}

		os.MkdirAll(runDir, 0700)

		var profiles uint32
		var handlerType string
		switch cmd {
		case "np-ping-pong-server":
			profiles = profileNP
			handlerType = "ping-pong"
		case "shm-ping-pong-server":
			profiles = profileWinSHM
			handlerType = "ping-pong"
		case "snapshot-server":
			profiles = profileNP
			handlerType = "snapshot"
		case "snapshot-shm-server":
			profiles = profileWinSHM
			handlerType = "snapshot"
		}

		rc = runServerWin(runDir, service, profiles, duration, handlerType)

	case "np-ping-pong-client", "shm-ping-pong-client":
		if len(os.Args) < 6 {
			fmt.Fprintf(os.Stderr, "Usage: %s %s <run_dir> <service> <duration_sec> <target_rps>\n", os.Args[0], cmd)
			os.Exit(1)
		}
		runDir := os.Args[2]
		service := os.Args[3]
		duration, _ := strconv.Atoi(os.Args[4])
		targetRPS, _ := strconv.ParseUint(os.Args[5], 10, 64)

		var profiles uint32
		var scenario string
		switch cmd {
		case "np-ping-pong-client":
			profiles = profileNP
			scenario = "np-ping-pong"
		case "shm-ping-pong-client":
			profiles = profileWinSHM
			scenario = "shm-ping-pong"
		}

		rc = runPingPongClientWin(runDir, service, profiles, duration, targetRPS, scenario, "go")

	case "snapshot-client", "snapshot-shm-client":
		if len(os.Args) < 6 {
			fmt.Fprintf(os.Stderr, "Usage: %s %s <run_dir> <service> <duration_sec> <target_rps>\n", os.Args[0], cmd)
			os.Exit(1)
		}
		runDir := os.Args[2]
		service := os.Args[3]
		duration, _ := strconv.Atoi(os.Args[4])
		targetRPS, _ := strconv.ParseUint(os.Args[5], 10, 64)

		var profiles uint32
		var scenario string
		switch cmd {
		case "snapshot-client":
			profiles = profileNP
			scenario = "snapshot-baseline"
		case "snapshot-shm-client":
			profiles = profileWinSHM
			scenario = "snapshot-shm"
		}

		rc = runSnapshotClientWin(runDir, service, profiles, duration, targetRPS, scenario, "go")

	case "lookup-bench":
		if len(os.Args) < 3 {
			fmt.Fprintf(os.Stderr, "Usage: %s lookup-bench <duration_sec>\n", os.Args[0])
			os.Exit(1)
		}
		duration, _ := strconv.Atoi(os.Args[2])
		rc = runLookupBenchWin(duration)

	default:
		fmt.Fprintf(os.Stderr, "Unknown subcommand: %s\n", cmd)
		os.Exit(1)
	}

	os.Exit(rc)
}
