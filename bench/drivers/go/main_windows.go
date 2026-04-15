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
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"math/rand"
	"os"
	"os/exec"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/service/cgroups"
	rawsvc "github.com/netdata/plugin-ipc/go/pkg/netipc/service/raw"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"
)

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

const (
	authTokenWin         = uint64(0xBE4C400000C0FFEE)
	responseBufSizeWin   = 65536
	maxLatencySamplesWin = 500_000
	latencySampleStride  = 64
	latencySampleSlack   = 4096
	defaultDurationWin   = 30
	profileNP            = protocol.ProfileBaseline
	profileWinSHM        = protocol.ProfileBaseline | 0x02 // PROFILE_SHM_HYBRID

	// Batch scenario limits (mirrors C driver / POSIX Go driver).
	maxBatchItemsWin = 1000
	batchBufSizeWin  = maxBatchItemsWin*48 + 4096 // 52096

	clientShmAttachRetryIntervalWin = 5 * time.Millisecond
	clientShmAttachRetryTimeoutWin  = 5 * time.Second
)

type cacheBucketWin struct {
	index int
	used  bool
}

func cacheHashNameWin(name string) uint32 {
	h := uint32(5381)
	for i := 0; i < len(name); i++ {
		h = ((h << 5) + h) + uint32(name[i])
	}
	return h
}

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
	kernel32                      = syscall.NewLazyDLL("kernel32.dll")
	procQueryPerformanceFrequency = kernel32.NewProc("QueryPerformanceFrequency")
	procQueryPerformanceCounter   = kernel32.NewProc("QueryPerformanceCounter")
	procGetProcessTimes           = kernel32.NewProc("GetProcessTimes")
	procGetTickCount64            = kernel32.NewProc("GetTickCount64")
	procGetProcessAffinityMask    = kernel32.NewProc("GetProcessAffinityMask")
	procGetCurrentThread          = kernel32.NewProc("GetCurrentThread")
	procSetProcessAffinityMask    = kernel32.NewProc("SetProcessAffinityMask")
	procSetThreadAffinityMask     = kernel32.NewProc("SetThreadAffinityMask")
	procSetPriorityClass          = kernel32.NewProc("SetPriorityClass")
	procSetThreadPriority         = kernel32.NewProc("SetThreadPriority")
)

const (
	highPriorityClassWin  = 0x00000080
	threadPriorityHighest = 2
)

func nowNS() uint64 {
	var counter int64
	syscall.Syscall(procQueryPerformanceCounter.Addr(), 1, uintptr(unsafe.Pointer(&counter)), 0, 0)
	if qpcFreq <= 0 || counter <= 0 {
		return 0
	}
	c := uint64(counter)
	f := uint64(qpcFreq)
	secs := c / f
	rem := c % f
	return secs*1_000_000_000 + (rem*1_000_000_000)/f
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

func tickMSWin() uint64 {
	ret, _, _ := syscall.Syscall(procGetTickCount64.Addr(), 0, 0, 0, 0)
	return uint64(ret)
}

func elevateBenchPriorityWin() {
	process, _ := syscall.GetCurrentProcess()
	if r1, _, err := syscall.Syscall(procSetPriorityClass.Addr(), 2,
		uintptr(process), uintptr(highPriorityClassWin), 0); r1 == 0 {
		fmt.Fprintf(os.Stderr, "warn: SetPriorityClass(HIGH_PRIORITY_CLASS) failed: %v\n", err)
	}

	thread, _, _ := syscall.Syscall(procGetCurrentThread.Addr(), 0, 0, 0, 0)
	if r1, _, err := syscall.Syscall(procSetThreadPriority.Addr(), 2,
		thread, uintptr(threadPriorityHighest), 0); r1 == 0 {
		fmt.Fprintf(os.Stderr, "warn: SetThreadPriority(THREAD_PRIORITY_HIGHEST) failed: %v\n", err)
	}
}

func benchAffinitySlotWin(cmd string) int {
	if raw := os.Getenv("NIPC_BENCH_AFFINITY_SLOT"); raw != "" {
		if slot, err := strconv.Atoi(raw); err == nil && slot >= 0 {
			return slot
		}
	}

	if cmd == "lookup-bench" {
		return 0
	}
	if strings.Contains(cmd, "client") {
		return 1
	}
	return 0
}

func selectBenchAffinityMaskWin(processMask uintptr, slot int) uintptr {
	if processMask == 0 {
		return 0
	}
	if slot < 0 {
		slot = 0
	}

	available := make([]uintptr, 0, unsafe.Sizeof(processMask)*8)
	for bit := uintptr(1); bit != 0; bit <<= 1 {
		if processMask&bit != 0 {
			available = append(available, bit)
		}
	}
	if len(available) == 0 {
		return 0
	}
	if slot >= len(available) {
		slot = len(available) - 1
	}

	return available[len(available)-1-slot]
}

func pinBenchAffinityWin(cmd string) {
	process, _ := syscall.GetCurrentProcess()

	var processMask uintptr
	var systemMask uintptr
	if r1, _, err := syscall.Syscall(procGetProcessAffinityMask.Addr(), 3,
		uintptr(process),
		uintptr(unsafe.Pointer(&processMask)),
		uintptr(unsafe.Pointer(&systemMask))); r1 == 0 {
		fmt.Fprintf(os.Stderr, "warn: GetProcessAffinityMask failed: %v\n", err)
		return
	}

	if processMask == 0 {
		return
	}

	pinMask := selectBenchAffinityMaskWin(processMask, benchAffinitySlotWin(cmd))
	if pinMask == 0 {
		return
	}

	if r1, _, err := syscall.Syscall(procSetProcessAffinityMask.Addr(), 2,
		uintptr(process), pinMask, 0); r1 == 0 {
		fmt.Fprintf(os.Stderr, "warn: SetProcessAffinityMask failed: %v\n", err)
	}

	thread, _, _ := syscall.Syscall(procGetCurrentThread.Addr(), 0, 0, 0, 0)
	if r1, _, err := syscall.Syscall(procSetThreadAffinityMask.Addr(), 2,
		thread, pinMask, 0); r1 == 0 {
		fmt.Fprintf(os.Stderr, "warn: SetThreadAffinityMask failed: %v\n", err)
	}
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

func latencySparseCapWin(targetRPS uint64, durationSec int) int {
	if targetRPS == 0 {
		return maxLatencySamplesWin
	}
	estimated := int((targetRPS+latencySampleStride-1)/latencySampleStride) * durationSec
	estimated += latencySampleSlack
	if estimated > maxLatencySamplesWin {
		estimated = maxLatencySamplesWin
	}
	return estimated
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

func latencyUSWin(ns uint64) float64 {
	return float64(ns) / 1000.0
}

func sampleStartWin(counter uint64) uint64 {
	if counter&63 == 0 {
		return nowNS()
	}
	return 0
}

func sampleFinishWin(lr *latencyRecorderWin, startNS uint64) {
	if startNS != 0 {
		lr.record(nowNS() - startNS)
	}
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
	for now.Before(rl.next) {
		remaining := rl.next.Sub(now)
		if remaining >= 2*time.Millisecond {
			time.Sleep(remaining - time.Millisecond)
		} else if remaining >= 100*time.Microsecond {
			runtime.Gosched()
		} else {
			for time.Now().Before(rl.next) {
			}
			break
		}
		now = time.Now()
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

func batchServerConfigWin(profiles uint32) windows.ServerConfig {
	return windows.ServerConfig{
		SupportedProfiles:       profiles,
		PreferredProfiles:       profiles,
		MaxRequestPayloadBytes:  batchBufSizeWin,
		MaxRequestBatchItems:    maxBatchItemsWin,
		MaxResponsePayloadBytes: batchBufSizeWin,
		MaxResponseBatchItems:   maxBatchItemsWin,
		AuthToken:               authTokenWin,
	}
}

func batchClientConfigWin(profiles uint32) windows.ClientConfig {
	return windows.ClientConfig{
		SupportedProfiles:       profiles,
		PreferredProfiles:       profiles,
		MaxRequestPayloadBytes:  batchBufSizeWin,
		MaxRequestBatchItems:    maxBatchItemsWin,
		MaxResponsePayloadBytes: batchBufSizeWin,
		MaxResponseBatchItems:   maxBatchItemsWin,
		AuthToken:               authTokenWin,
	}
}

func typedServerConfigWin(profiles uint32) cgroups.ServerConfig {
	return cgroups.ServerConfig{
		SupportedProfiles:       profiles,
		PreferredProfiles:       profiles,
		MaxRequestBatchItems:    1,
		MaxResponsePayloadBytes: responseBufSizeWin,
		AuthToken:               authTokenWin,
	}
}

func typedClientConfigWin(profiles uint32) cgroups.ClientConfig {
	return cgroups.ClientConfig{
		SupportedProfiles:       profiles,
		PreferredProfiles:       profiles,
		MaxRequestBatchItems:    1,
		MaxResponsePayloadBytes: responseBufSizeWin,
		AuthToken:               authTokenWin,
	}
}

func attachBenchShmWin(runDir, service string, session *windows.Session) (*windows.WinShmContext, error) {
	if session.SelectedProfile != windows.WinShmProfileHybrid &&
		session.SelectedProfile != windows.WinShmProfileBusywait {
		return nil, fmt.Errorf("expected Windows SHM profile, negotiated %d", session.SelectedProfile)
	}

	deadline := time.Now().Add(clientShmAttachRetryTimeoutWin)
	var lastErr error
	for {
		shm, err := windows.WinShmClientAttach(
			runDir,
			service,
			authTokenWin,
			session.SessionID,
			session.SelectedProfile,
		)
		if err == nil {
			return shm, nil
		}

		lastErr = err
		if !time.Now().Before(deadline) {
			break
		}
		time.Sleep(clientShmAttachRetryIntervalWin)
	}

	return nil, fmt.Errorf("Windows SHM attach failed after %s: %w", clientShmAttachRetryTimeoutWin, lastErr)
}

// ---------------------------------------------------------------------------
//  Ping-pong handler
// ---------------------------------------------------------------------------

func pingPongDispatchWin() rawsvc.DispatchHandler {
	return rawsvc.IncrementDispatch(func(counter uint64) (uint64, bool) {
		return counter + 1, true
	})
}

// ---------------------------------------------------------------------------
//  Snapshot handler
// ---------------------------------------------------------------------------

var snapshotGenWin uint64
var snapshotNamesWin [][]byte
var snapshotPathsWin [][]byte

func initSnapshotTemplateWin() bool {
	if snapshotNamesWin != nil {
		return true
	}

	snapshotNamesWin = make([][]byte, 16)
	snapshotPathsWin = make([][]byte, 16)
	for i := uint32(0); i < 16; i++ {
		name := fmt.Sprintf("cgroup-%d", i)
		path := fmt.Sprintf("/sys/fs/cgroup/bench/cg-%d", i)
		snapshotNamesWin[i] = []byte(name)
		snapshotPathsWin[i] = []byte(path)
	}
	return true
}

func snapshotHandlerWin() cgroups.Handler {
	return cgroups.Handler{
		Handle: func(request *protocol.CgroupsRequest, builder *protocol.CgroupsBuilder) bool {
			if request.LayoutVersion != 1 || request.Flags != 0 || !initSnapshotTemplateWin() {
				return false
			}
			builder.SetHeader(1, atomic.AddUint64(&snapshotGenWin, 1))
			for i := uint32(0); i < 16; i++ {
				if err := builder.Add(1000+i, 0, i%2, snapshotNamesWin[i], snapshotPathsWin[i]); err != nil {
					return false
				}
			}
			return true
		},
		SnapshotMaxItems: 16,
	}
}

// ---------------------------------------------------------------------------
//  Server
// ---------------------------------------------------------------------------

func runServerWin(runDir, service string, profiles uint32, durationSec int, handlerType string) int {
	switch handlerType {
	case "ping-pong":
		server := rawsvc.NewServer(
			runDir, service, serverConfigWin(profiles), protocol.MethodIncrement, pingPongDispatchWin(),
		)

		// Benchmark only steady-state work, not one-off startup garbage.
		runtime.GC()
		serverErr := make(chan error, 1)
		go func() {
			serverErr <- server.Run()
		}()

		if err := warmServerWin(runDir, service); err != nil {
			server.Stop()
			if runErr := <-serverErr; runErr != nil {
				fmt.Fprintf(os.Stderr, "server: %v\n", runErr)
			}
			fmt.Fprintf(os.Stderr, "server warmup: %v\n", err)
			return 1
		}

		// Clear allocations created by the hidden warmup before the measured window.
		runtime.GC()

		go func() {
			time.Sleep(time.Duration(durationSec+3) * time.Second)
			server.Stop()
		}()

		fmt.Println("READY")

		cpuStart := cpuNSWin()

		if err := <-serverErr; err != nil {
			fmt.Fprintf(os.Stderr, "server: %v\n", err)
		}

		cpuEnd := cpuNSWin()
		cpuSec := float64(cpuEnd-cpuStart) / 1e9

		fmt.Printf("SERVER_CPU_SEC=%.6f\n", cpuSec)
		return 0
	case "snapshot":
		server := cgroups.NewServer(runDir, service, typedServerConfigWin(profiles), snapshotHandlerWin())

		// Benchmark only steady-state work, not one-off startup garbage.
		runtime.GC()
		serverErr := make(chan error, 1)
		go func() {
			serverErr <- server.Run()
		}()

		if err := warmServerWin(runDir, service); err != nil {
			server.Stop()
			if runErr := <-serverErr; runErr != nil {
				fmt.Fprintf(os.Stderr, "server: %v\n", runErr)
			}
			fmt.Fprintf(os.Stderr, "server warmup: %v\n", err)
			return 1
		}

		// Clear allocations created by the hidden warmup before the measured window.
		runtime.GC()

		go func() {
			time.Sleep(time.Duration(durationSec+3) * time.Second)
			server.Stop()
		}()

		fmt.Println("READY")

		cpuStart := cpuNSWin()

		if err := <-serverErr; err != nil {
			fmt.Fprintf(os.Stderr, "server: %v\n", err)
		}

		cpuEnd := cpuNSWin()
		cpuSec := float64(cpuEnd-cpuStart) / 1e9

		fmt.Printf("SERVER_CPU_SEC=%.6f\n", cpuSec)
		return 0
	default:
		fmt.Fprintf(os.Stderr, "unknown handler type: %s\n", handlerType)
		return 1
	}
}

// ---------------------------------------------------------------------------
//  Hidden warmup client helper
// ---------------------------------------------------------------------------

func warmServerWin(runDir, service string) error {
	warmupBin := os.Getenv("NIPC_BENCH_SERVER_WARMUP_BIN")
	warmupSubcmd := os.Getenv("NIPC_BENCH_SERVER_WARMUP_SUBCMD")
	warmupDuration := os.Getenv("NIPC_BENCH_SERVER_WARMUP_DURATION_SEC")
	warmupTarget := os.Getenv("NIPC_BENCH_SERVER_WARMUP_TARGET_RPS")
	warmupDepth := os.Getenv("NIPC_BENCH_SERVER_WARMUP_DEPTH")

	if warmupBin == "" || warmupSubcmd == "" || warmupDuration == "" {
		return nil
	}

	args := []string{warmupSubcmd, runDir, service, warmupDuration}
	if warmupTarget != "" {
		args = append(args, warmupTarget)
	}
	if warmupDepth != "" {
		args = append(args, warmupDepth)
	}

	cmd := exec.Command(warmupBin, args...)
	cmd.Stdout = io.Discard

	var stderr bytes.Buffer
	cmd.Stderr = &stderr

	if err := cmd.Run(); err != nil {
		msg := strings.TrimSpace(stderr.String())
		if msg != "" {
			return fmt.Errorf("server warmup client failed: %w: %s", err, msg)
		}
		return fmt.Errorf("server warmup client failed: %w", err)
	}

	return nil
}

// ---------------------------------------------------------------------------
//  Batch server (same handler, higher batch/payload limits)
// ---------------------------------------------------------------------------

func runBatchServerWin(runDir, service string, profiles uint32, durationSec int) int {
	cfg := batchServerConfigWin(profiles)
	cfg.MaxResponsePayloadBytes = batchBufSizeWin * 2 // extra room for builder overhead

	server := rawsvc.NewServer(
		runDir, service, cfg, protocol.MethodIncrement, pingPongDispatchWin(),
	)

	// Benchmark only steady-state work, not one-off startup garbage.
	runtime.GC()
	serverErr := make(chan error, 1)
	go func() {
		serverErr <- server.Run()
	}()

	if err := warmServerWin(runDir, service); err != nil {
		server.Stop()
		if runErr := <-serverErr; runErr != nil {
			fmt.Fprintf(os.Stderr, "batch server: %v\n", runErr)
		}
		fmt.Fprintf(os.Stderr, "batch server warmup: %v\n", err)
		return 1
	}

	// Clear allocations created by the hidden warmup before the measured window.
	runtime.GC()

	go func() {
		time.Sleep(time.Duration(durationSec+3) * time.Second)
		server.Stop()
	}()

	fmt.Println("READY")

	cpuStart := cpuNSWin()

	if err := <-serverErr; err != nil {
		fmt.Fprintf(os.Stderr, "batch server: %v\n", err)
	}

	cpuEnd := cpuNSWin()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9

	fmt.Printf("SERVER_CPU_SEC=%.6f\n", cpuSec)
	return 0
}

// ---------------------------------------------------------------------------
//  Batch ping-pong client (random 2-1000 items per batch)
// ---------------------------------------------------------------------------

func warmBatchPingPongClientWin(session *windows.Session, shm *windows.WinShmContext, reqBuf, respBuf, msgBuf []byte, expected []uint64, itemBuf []byte) error {
	const warmBatchSize = 8

	var bb protocol.BatchBuilder
	bb.Reset(reqBuf, warmBatchSize)

	for i := uint32(0); i < warmBatchSize; i++ {
		protocol.IncrementEncode(uint64(i), itemBuf)
		expected[i] = uint64(i) + 1
		if err := bb.Add(itemBuf); err != nil {
			return fmt.Errorf("warmup build: %w", err)
		}
	}

	reqLen, _ := bb.Finish()
	hdr := protocol.Header{
		Kind:            protocol.KindRequest,
		Code:            protocol.MethodIncrement,
		Flags:           protocol.FlagBatch,
		ItemCount:       warmBatchSize,
		MessageID:       1,
		TransportStatus: protocol.StatusOK,
	}

	if shm != nil {
		msgLen := protocol.HeaderSize + reqLen
		msg := msgBuf[:msgLen]
		hdr.Magic = protocol.MagicMsg
		hdr.Version = protocol.Version
		hdr.HeaderLen = protocol.HeaderLen
		hdr.PayloadLen = uint32(reqLen)
		hdr.Encode(msg[:protocol.HeaderSize])
		copy(msg[protocol.HeaderSize:], reqBuf[:reqLen])

		if err := shm.WinShmSend(msg); err != nil {
			return fmt.Errorf("warmup send: %w", err)
		}

		respLen, err := shm.WinShmReceive(respBuf, 30000)
		if err != nil {
			return fmt.Errorf("warmup receive: %w", err)
		}
		if respLen < protocol.HeaderSize {
			return fmt.Errorf("warmup short shm response")
		}

		respHdr, err := protocol.DecodeHeader(respBuf[:protocol.HeaderSize])
		if err != nil {
			return fmt.Errorf("warmup decode header: %w", err)
		}
		payload := respBuf[protocol.HeaderSize:respLen]
		if respHdr.ItemCount != warmBatchSize {
			return fmt.Errorf("warmup item count mismatch: got %d want %d", respHdr.ItemCount, warmBatchSize)
		}

		for i := uint32(0); i < warmBatchSize; i++ {
			itemPayload, itemErr := protocol.BatchItemGet(payload, respHdr.ItemCount, i)
			if itemErr != nil {
				return fmt.Errorf("warmup batch item %d: %w", i, itemErr)
			}
			value, decodeErr := protocol.IncrementDecode(itemPayload)
			if decodeErr != nil {
				return fmt.Errorf("warmup decode item %d: %w", i, decodeErr)
			}
			if value != expected[i] {
				return fmt.Errorf("warmup item %d mismatch: got %d want %d", i, value, expected[i])
			}
		}

		return nil
	}

	if err := session.Send(&hdr, reqBuf[:reqLen]); err != nil {
		return fmt.Errorf("warmup send: %w", err)
	}

	respHdr, payload, err := session.Receive(respBuf)
	if err != nil {
		return fmt.Errorf("warmup receive: %w", err)
	}
	if respHdr.ItemCount != warmBatchSize {
		return fmt.Errorf("warmup item count mismatch: got %d want %d", respHdr.ItemCount, warmBatchSize)
	}

	for i := uint32(0); i < warmBatchSize; i++ {
		itemPayload, itemErr := protocol.BatchItemGet(payload, respHdr.ItemCount, i)
		if itemErr != nil {
			return fmt.Errorf("warmup batch item %d: %w", i, itemErr)
		}
		value, decodeErr := protocol.IncrementDecode(itemPayload)
		if decodeErr != nil {
			return fmt.Errorf("warmup decode item %d: %w", i, decodeErr)
		}
		if value != expected[i] {
			return fmt.Errorf("warmup item %d mismatch: got %d want %d", i, value, expected[i])
		}
	}

	return nil
}

func runBatchPingPongClientWin(runDir, service string, profiles uint32, durationSec int, targetRPS uint64, scenario, serverLang string) int {
	cfg := batchClientConfigWin(profiles)
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
		fmt.Fprintf(os.Stderr, "batch client: connect failed: %v\n", err)
		return 1
	}
	defer session.Close()

	var shm *windows.WinShmContext
	if profiles&(windows.WinShmProfileHybrid|windows.WinShmProfileBusywait) != 0 {
		shm, err = attachBenchShmWin(runDir, service, session)
		if err != nil {
			fmt.Fprintf(os.Stderr, "batch client: %v\n", err)
			return 1
		}
	}
	if shm != nil {
		defer shm.WinShmClose()
	}

	lr := newLatencyRecorderWin(latencySparseCapWin(targetRPS, durationSec))
	rl := newRateLimiterWin(targetRPS)

	reqBuf := make([]byte, batchBufSizeWin)
	respBuf := make([]byte, batchBufSizeWin+protocol.HeaderSize)
	msgBuf := make([]byte, batchBufSizeWin+protocol.HeaderSize)
	expected := make([]uint64, maxBatchItemsWin)
	itemBuf := make([]byte, protocol.IncrementPayloadSize)

	var counter, totalItems, errors, msgSeq uint64

	if err := warmBatchPingPongClientWin(session, shm, reqBuf, respBuf, msgBuf, expected, itemBuf); err != nil {
		fmt.Fprintf(os.Stderr, "batch client warmup: %v\n", err)
		return 1
	}

	runtime.GC()

	cpuStart := cpuNSWin()
	wallStart := nowNS()
	tickDeadline := tickMSWin() + uint64(durationSec)*1000

	for tickMSWin() < tickDeadline {
		rl.wait()

		// Random batch size 2-1000 (server treats itemCount==1 as non-batch)
		batchSize := uint32(rand.Intn(maxBatchItemsWin-1) + 2)

		// Reuse a stack builder to avoid a heap object per request.
		var bb protocol.BatchBuilder
		bb.Reset(reqBuf, batchSize)

		buildOK := true
		for i := uint32(0); i < batchSize; i++ {
			val := counter + uint64(i)
			protocol.IncrementEncode(val, itemBuf)
			expected[i] = val + 1

			if berr := bb.Add(itemBuf); berr != nil {
				errors++
				buildOK = false
				break
			}
		}
		if !buildOK {
			continue
		}

		reqLen, _ := bb.Finish()

		msgSeq++
		hdr := protocol.Header{
			Kind:            protocol.KindRequest,
			Code:            protocol.MethodIncrement,
			Flags:           protocol.FlagBatch,
			ItemCount:       batchSize,
			MessageID:       msgSeq,
			TransportStatus: protocol.StatusOK,
		}

		t0 := sampleStartWin(totalItems)

		if shm != nil {
			// SHM path
			msgLen := protocol.HeaderSize + reqLen
			msg := msgBuf[:msgLen]
			hdr.Magic = protocol.MagicMsg
			hdr.Version = protocol.Version
			hdr.HeaderLen = protocol.HeaderLen
			hdr.PayloadLen = uint32(reqLen)
			hdr.Encode(msg[:protocol.HeaderSize])
			copy(msg[protocol.HeaderSize:], reqBuf[:reqLen])

			if serr := shm.WinShmSend(msg); serr != nil {
				errors++
				continue
			}

			shmRespLen, serr := shm.WinShmReceive(respBuf, 30000)
			if serr != nil {
				errors++
				continue
			}

			if shmRespLen < protocol.HeaderSize {
				errors++
				continue
			}

			respHdr, herr := protocol.DecodeHeader(respBuf[:protocol.HeaderSize])
			if herr != nil {
				errors++
				continue
			}

			if respHdr.Kind != protocol.KindResponse ||
				respHdr.Code != protocol.MethodIncrement ||
				respHdr.ItemCount != batchSize {
				errors++
				continue
			}

			respPayload := respBuf[protocol.HeaderSize : protocol.HeaderSize+int(respHdr.PayloadLen)]

			batchOK := true
			if batchSize == 1 {
				respVal, derr := protocol.IncrementDecode(respPayload)
				if derr != nil {
					errors++
					batchOK = false
				} else if respVal != expected[0] {
					errors++
					batchOK = false
				}
			} else {
				for i := uint32(0); i < batchSize; i++ {
					item, gerr := protocol.BatchItemGet(respPayload, batchSize, i)
					if gerr != nil {
						errors++
						batchOK = false
						break
					}
					respVal, derr := protocol.IncrementDecode(item)
					if derr != nil {
						errors++
						batchOK = false
						break
					}
					if respVal != expected[i] {
						errors++
						batchOK = false
						break
					}
				}
			}

			sampleFinishWin(lr, t0)
			_ = batchOK
			totalItems += uint64(batchSize)
		} else {
			// Named Pipe path
			if serr := session.Send(&hdr, reqBuf[:reqLen]); serr != nil {
				errors++
				continue
			}

			respHdr, payload, rerr := session.Receive(respBuf)
			if rerr != nil {
				errors++
				continue
			}

			if respHdr.Kind != protocol.KindResponse ||
				respHdr.Code != protocol.MethodIncrement ||
				respHdr.ItemCount != batchSize {
				errors++
				continue
			}

			batchOK := true
			if batchSize == 1 {
				respVal, derr := protocol.IncrementDecode(payload)
				if derr != nil {
					errors++
					batchOK = false
				} else if respVal != expected[0] {
					errors++
					batchOK = false
				}
			} else {
				for i := uint32(0); i < batchSize; i++ {
					item, gerr := protocol.BatchItemGet(payload, batchSize, i)
					if gerr != nil {
						errors++
						batchOK = false
						break
					}
					respVal, derr := protocol.IncrementDecode(item)
					if derr != nil {
						errors++
						batchOK = false
						break
					}
					if respVal != expected[i] {
						errors++
						batchOK = false
						break
					}
				}
			}

			sampleFinishWin(lr, t0)
			_ = batchOK
			totalItems += uint64(batchSize)
		}

		counter += uint64(batchSize)
	}

	wallSec := float64(nowNS()-wallStart) / 1e9
	cpuEnd := cpuNSWin()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9
	throughput := float64(totalItems) / wallSec
	cpuPct := (cpuSec / wallSec) * 100.0

	p50 := latencyUSWin(lr.percentile(50.0))
	p95 := latencyUSWin(lr.percentile(95.0))
	p99 := latencyUSWin(lr.percentile(99.0))

	fmt.Printf("%s,go,%s,%.0f,%.3f,%.3f,%.3f,%.1f,0.0,%.1f\n",
		scenario, serverLang, throughput, p50, p95, p99, cpuPct, cpuPct)

	if errors > 0 {
		fmt.Fprintf(os.Stderr, "batch client: %d errors\n", errors)
		return 1
	}
	return 0
}

// ---------------------------------------------------------------------------
//  Pipeline client (sends depth requests, then reads depth responses)
// ---------------------------------------------------------------------------

func runPipelineClientWin(runDir, service string, durationSec int, targetRPS uint64, depth int, serverLang string) int {
	cfg := clientConfigWin(profileNP)
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
		fmt.Fprintf(os.Stderr, "pipeline client: connect failed: %v\n", err)
		return 1
	}
	defer session.Close()

	lr := newLatencyRecorderWin(latencySparseCapWin(targetRPS, durationSec))
	rl := newRateLimiterWin(targetRPS)

	var counter, requests, errors, msgSeq uint64

	cpuStart := cpuNSWin()
	wallStart := nowNS()
	tickDeadline := tickMSWin() + uint64(durationSec)*1000

	recvBuf := make([]byte, 256)
	var reqPayload [8]byte

	if err := warmPipelineClientWin(session, recvBuf, &reqPayload, depth); err != nil {
		fmt.Fprintf(os.Stderr, "pipeline warmup: %v\n", err)
		return 1
	}

	runtime.GC()

	for tickMSWin() < tickDeadline {
		rl.wait()

		t0 := sampleStartWin(requests)

		// Send `depth` requests with unique message IDs
		sendOK := true
		for d := 0; d < depth; d++ {
			val := counter + uint64(d)
			binary.NativeEndian.PutUint64(reqPayload[:], val)

			msgSeq++
			hdr := protocol.Header{
				Kind:            protocol.KindRequest,
				Code:            protocol.MethodIncrement,
				ItemCount:       1,
				MessageID:       msgSeq,
				TransportStatus: protocol.StatusOK,
				PayloadLen:      8,
			}

			if serr := session.Send(&hdr, reqPayload[:]); serr != nil {
				sendOK = false
				errors++
				break
			}
		}

		if !sendOK {
			continue
		}

		// Receive `depth` responses
		for d := 0; d < depth; d++ {
			_, payload, rerr := session.Receive(recvBuf)
			if rerr != nil {
				errors++
				break
			}

			if len(payload) >= 8 {
				respVal := binary.NativeEndian.Uint64(payload[:8])
				expected := counter + uint64(d) + 1
				if respVal != expected {
					fmt.Fprintf(os.Stderr, "pipeline chain broken at depth %d: expected %d, got %d\n",
						d, expected, respVal)
					errors++
				}
			}
		}

		sampleFinishWin(lr, t0)

		counter += uint64(depth)
		requests += uint64(depth)
	}

	wallSec := float64(nowNS()-wallStart) / 1e9
	cpuEnd := cpuNSWin()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9
	throughput := float64(requests) / wallSec
	cpuPct := (cpuSec / wallSec) * 100.0

	p50 := latencyUSWin(lr.percentile(50.0))
	p95 := latencyUSWin(lr.percentile(95.0))
	p99 := latencyUSWin(lr.percentile(99.0))

	scenario := fmt.Sprintf("np-pipeline-d%d", depth)
	fmt.Printf("%s,go,%s,%.0f,%.3f,%.3f,%.3f,%.1f,0.0,%.1f\n",
		scenario, serverLang, throughput, p50, p95, p99, cpuPct, cpuPct)

	if errors > 0 {
		fmt.Fprintf(os.Stderr, "pipeline client: %d errors\n", errors)
		return 1
	}
	return 0
}

func warmPipelineClientWin(session *windows.Session, recvBuf []byte, reqPayload *[8]byte, depth int) error {
	var msgSeq uint64

	for d := 0; d < depth; d++ {
		binary.NativeEndian.PutUint64(reqPayload[:], uint64(d))

		msgSeq++
		hdr := protocol.Header{
			Kind:            protocol.KindRequest,
			Code:            protocol.MethodIncrement,
			ItemCount:       1,
			MessageID:       msgSeq,
			TransportStatus: protocol.StatusOK,
			PayloadLen:      8,
		}

		if err := session.Send(&hdr, reqPayload[:]); err != nil {
			return fmt.Errorf("pipeline warmup send: %w", err)
		}
	}

	for d := 0; d < depth; d++ {
		_, payload, err := session.Receive(recvBuf)
		if err != nil {
			return fmt.Errorf("pipeline warmup recv: %w", err)
		}
		if len(payload) < 8 {
			return fmt.Errorf("pipeline warmup short payload")
		}

		respVal := binary.NativeEndian.Uint64(payload[:8])
		expected := uint64(d + 1)
		if respVal != expected {
			return fmt.Errorf(
				"pipeline warmup chain broken at depth %d: expected %d, got %d",
				d,
				expected,
				respVal,
			)
		}
	}

	return nil
}

// ---------------------------------------------------------------------------
//  Pipeline+batch client (sends depth batch msgs, reads depth responses)
// ---------------------------------------------------------------------------

type pipelineBatchSlotWin struct {
	messageID     uint64
	batchSize     uint32
	inflightBytes uint64
	active        bool
}

func warmPipelineBatchClientWin(session *windows.Session, reqBufs [][]byte, recvBuf []byte, itemBuf []byte, depth int) error {
	slots := make([]pipelineBatchSlotWin, depth)
	var counter, msgSeq uint64
	inflightBudget := uint64(session.PacketSize) * 2

	sent := 0
	received := 0
	outstanding := 0
	inflightBytes := uint64(0)

	for received < depth {
		if sent < depth {
			slot := sent
			bs := uint32(maxBatchItemsWin)

			var bb protocol.BatchBuilder
			bb.Reset(reqBufs[slot], bs)

			for i := uint32(0); i < bs; i++ {
				protocol.IncrementEncode(counter+uint64(i), itemBuf)
				if err := bb.Add(itemBuf); err != nil {
					return fmt.Errorf("pipeline-batch warmup build: %w", err)
				}
			}

			reqLen, _ := bb.Finish()

			msgSeq++
			hdr := protocol.Header{
				Kind:            protocol.KindRequest,
				Code:            protocol.MethodIncrement,
				Flags:           protocol.FlagBatch,
				ItemCount:       bs,
				MessageID:       msgSeq,
				TransportStatus: protocol.StatusOK,
			}

			slotBytes := uint64((protocol.HeaderSize + reqLen) * 2)
			for outstanding > 0 && inflightBytes+slotBytes > inflightBudget {
				respHdr, _, err := session.Receive(recvBuf)
				if err != nil {
					return fmt.Errorf("pipeline-batch warmup recv: %w", err)
				}

				matched := -1
				for i := range slots {
					if slots[i].active && slots[i].messageID == respHdr.MessageID {
						matched = i
						break
					}
				}
				if matched < 0 {
					return fmt.Errorf("pipeline-batch warmup unknown response id %d", respHdr.MessageID)
				}

				inflightBytes -= slots[matched].inflightBytes
				slots[matched].active = false
				outstanding--
				received++
			}

			if err := session.Send(&hdr, reqBufs[slot][:reqLen]); err != nil {
				return fmt.Errorf("pipeline-batch warmup send: %w", err)
			}

			slots[slot] = pipelineBatchSlotWin{
				messageID:     hdr.MessageID,
				batchSize:     bs,
				inflightBytes: slotBytes,
				active:        true,
			}
			inflightBytes += slotBytes
			outstanding++
			counter += uint64(bs)
			sent++
			continue
		}

		respHdr, _, err := session.Receive(recvBuf)
		if err != nil {
			return fmt.Errorf("pipeline-batch warmup recv: %w", err)
		}

		matched := -1
		for i := range slots {
			if slots[i].active && slots[i].messageID == respHdr.MessageID {
				matched = i
				break
			}
		}
		if matched < 0 {
			return fmt.Errorf("pipeline-batch warmup unknown response id %d", respHdr.MessageID)
		}

		inflightBytes -= slots[matched].inflightBytes
		slots[matched].active = false
		outstanding--
		received++
	}

	return nil
}

func runPipelineBatchClientWin(runDir, service string, durationSec int, targetRPS uint64, depth int, serverLang string) int {
	if depth < 1 {
		depth = 1
	}
	if depth > 128 {
		depth = 128
	}

	cfg := batchClientConfigWin(profileNP)
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
		fmt.Fprintf(os.Stderr, "pipeline-batch client: connect failed: %v\n", err)
		return 1
	}
	defer session.Close()

	lr := newLatencyRecorderWin(latencySparseCapWin(targetRPS, durationSec))
	rl := newRateLimiterWin(targetRPS)

	// Pre-allocate per-depth buffers
	reqBufs := make([][]byte, depth)
	slots := make([]pipelineBatchSlotWin, depth)
	itemBuf := make([]byte, protocol.IncrementPayloadSize)
	for i := range reqBufs {
		reqBufs[i] = make([]byte, batchBufSizeWin)
	}
	recvBuf := make([]byte, batchBufSizeWin+protocol.HeaderSize)

	var counter, totalItems, errors, msgSeq uint64
	inflightBudget := uint64(session.PacketSize) * 2

	if err := warmPipelineBatchClientWin(session, reqBufs, recvBuf, itemBuf, depth); err != nil {
		fmt.Fprintf(os.Stderr, "pipeline-batch warmup: %v\n", err)
		return 1
	}

	runtime.GC()

	cpuStart := cpuNSWin()
	wallStart := nowNS()
	tickDeadline := tickMSWin() + uint64(durationSec)*1000

	for tickMSWin() < tickDeadline {
		rl.wait()

		t0 := sampleStartWin(totalItems)
		for i := range slots {
			slots[i] = pipelineBatchSlotWin{}
		}

		sent := 0
		received := 0
		outstanding := 0
		inflightBytes := uint64(0)
		cycleOK := true

		for received < depth {
			if sent < depth {
				slot := sent
				bs := uint32(rand.Intn(maxBatchItemsWin-1) + 2)

				var bb protocol.BatchBuilder
				bb.Reset(reqBufs[slot], bs)

				for i := uint32(0); i < bs; i++ {
					protocol.IncrementEncode(counter+uint64(i), itemBuf)
					bb.Add(itemBuf)
				}

				reqLen, _ := bb.Finish()

				msgSeq++
				hdr := protocol.Header{
					Kind:            protocol.KindRequest,
					Code:            protocol.MethodIncrement,
					Flags:           protocol.FlagBatch,
					ItemCount:       bs,
					MessageID:       msgSeq,
					TransportStatus: protocol.StatusOK,
				}

				slotBytes := uint64((protocol.HeaderSize + reqLen) * 2)

				for outstanding > 0 && inflightBytes+slotBytes > inflightBudget {
					respHdr, _, rerr := session.Receive(recvBuf)
					if rerr != nil {
						errors++
						cycleOK = false
						break
					}

					matched := -1
					for i := range slots {
						if slots[i].active && slots[i].messageID == respHdr.MessageID {
							matched = i
							break
						}
					}
					if matched < 0 {
						fmt.Fprintf(os.Stderr, "pipeline-batch client: unknown response id %d\n", respHdr.MessageID)
						errors++
						cycleOK = false
						break
					}

					inflightBytes -= slots[matched].inflightBytes
					totalItems += uint64(slots[matched].batchSize)
					slots[matched].active = false
					outstanding--
					received++
				}
				if !cycleOK {
					break
				}

				if serr := session.Send(&hdr, reqBufs[slot][:reqLen]); serr != nil {
					errors++
					cycleOK = false
					break
				}

				slots[slot] = pipelineBatchSlotWin{
					messageID:     hdr.MessageID,
					batchSize:     bs,
					inflightBytes: slotBytes,
					active:        true,
				}
				inflightBytes += slotBytes
				outstanding++
				counter += uint64(bs)
				sent++
				continue
			}

			respHdr, _, rerr := session.Receive(recvBuf)
			if rerr != nil {
				errors++
				cycleOK = false
				break
			}

			matched := -1
			for i := range slots {
				if slots[i].active && slots[i].messageID == respHdr.MessageID {
					matched = i
					break
				}
			}
			if matched < 0 {
				fmt.Fprintf(os.Stderr, "pipeline-batch client: unknown response id %d\n", respHdr.MessageID)
				errors++
				cycleOK = false
				break
			}

			inflightBytes -= slots[matched].inflightBytes
			totalItems += uint64(slots[matched].batchSize)
			slots[matched].active = false
			outstanding--
			received++
		}

		if !cycleOK {
			break
		}

		sampleFinishWin(lr, t0)
	}

	wallSec := float64(nowNS()-wallStart) / 1e9
	cpuEnd := cpuNSWin()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9
	throughput := float64(totalItems) / wallSec
	cpuPct := (cpuSec / wallSec) * 100.0

	p50 := latencyUSWin(lr.percentile(50.0))
	p95 := latencyUSWin(lr.percentile(95.0))
	p99 := latencyUSWin(lr.percentile(99.0))

	scenario := fmt.Sprintf("np-pipeline-batch-d%d", depth)
	fmt.Printf("%s,go,%s,%.0f,%.3f,%.3f,%.3f,%.1f,0.0,%.1f\n",
		scenario, serverLang, throughput, p50, p95, p99, cpuPct, cpuPct)

	if errors > 0 {
		fmt.Fprintf(os.Stderr, "pipeline-batch client: %d errors\n", errors)
		return 1
	}
	return 0
}

// ---------------------------------------------------------------------------
//  Ping-pong client
// ---------------------------------------------------------------------------

func warmPingPongClientWin(session *windows.Session, shm *windows.WinShmContext, msgBuf, respBuf, recvBuf []byte) error {
	var reqPayload [8]byte
	binary.NativeEndian.PutUint64(reqPayload[:], 0)

	hdr := protocol.Header{
		Kind:            protocol.KindRequest,
		Code:            protocol.MethodIncrement,
		Flags:           0,
		ItemCount:       1,
		MessageID:       1,
		TransportStatus: protocol.StatusOK,
		PayloadLen:      8,
	}

	if shm != nil {
		msgLen := protocol.HeaderSize + len(reqPayload)
		msg := msgBuf[:msgLen]
		hdr.Magic = protocol.MagicMsg
		hdr.Version = protocol.Version
		hdr.HeaderLen = protocol.HeaderLen
		hdr.Encode(msg[:protocol.HeaderSize])
		copy(msg[protocol.HeaderSize:], reqPayload[:])

		if err := shm.WinShmSend(msg); err != nil {
			return fmt.Errorf("warmup send: %w", err)
		}

		respLen, err := shm.WinShmReceive(respBuf, 30000)
		if err != nil {
			return fmt.Errorf("warmup receive: %w", err)
		}
		if respLen < protocol.HeaderSize+8 {
			return fmt.Errorf("warmup short shm response")
		}

		value := binary.NativeEndian.Uint64(respBuf[protocol.HeaderSize : protocol.HeaderSize+8])
		if value != 1 {
			return fmt.Errorf("warmup counter mismatch: got %d want 1", value)
		}
		return nil
	}

	if err := session.Send(&hdr, reqPayload[:]); err != nil {
		return fmt.Errorf("warmup send: %w", err)
	}

	_, payload, err := session.Receive(recvBuf)
	if err != nil {
		return fmt.Errorf("warmup receive: %w", err)
	}
	if len(payload) < 8 {
		return fmt.Errorf("warmup short response")
	}

	value := binary.NativeEndian.Uint64(payload[:8])
	if value != 1 {
		return fmt.Errorf("warmup counter mismatch: got %d want 1", value)
	}

	return nil
}

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

	var shm *windows.WinShmContext
	if profiles&(windows.WinShmProfileHybrid|windows.WinShmProfileBusywait) != 0 {
		shm, err = attachBenchShmWin(runDir, service, session)
		if err != nil {
			fmt.Fprintf(os.Stderr, "client: %v\n", err)
			return 1
		}
	}
	if shm != nil {
		defer shm.WinShmClose()
	}

	lr := newLatencyRecorderWin(latencySparseCapWin(targetRPS, durationSec))
	rl := newRateLimiterWin(targetRPS)

	var counter, requests, errors uint64
	var reqPayload [8]byte
	msgBuf := make([]byte, protocol.HeaderSize+8)
	respBuf := make([]byte, protocol.HeaderSize+64)
	recvBuf := make([]byte, 256)

	if err := warmPingPongClientWin(session, shm, msgBuf, respBuf, recvBuf); err != nil {
		fmt.Fprintf(os.Stderr, "client warmup: %v\n", err)
		return 1
	}

	runtime.GC()

	cpuStart := cpuNSWin()
	wallStart := nowNS()
	tickDeadline := tickMSWin() + uint64(durationSec)*1000

	for tickMSWin() < tickDeadline {
		rl.wait()

		binary.NativeEndian.PutUint64(reqPayload[:], counter)

		hdr := protocol.Header{
			Kind:            protocol.KindRequest,
			Code:            protocol.MethodIncrement,
			Flags:           0,
			ItemCount:       1,
			MessageID:       counter + 1,
			TransportStatus: protocol.StatusOK,
			PayloadLen:      8,
		}

		t0 := sampleStartWin(requests)

		if shm != nil {
			// Win SHM path
			msgLen := protocol.HeaderSize + 8
			msg := msgBuf[:msgLen]
			hdr.Magic = protocol.MagicMsg
			hdr.Version = protocol.Version
			hdr.HeaderLen = protocol.HeaderLen
			hdr.Encode(msg[:protocol.HeaderSize])
			copy(msg[protocol.HeaderSize:], reqPayload[:])

			if err := shm.WinShmSend(msg); err != nil {
				errors++
				continue
			}

			respLen, err := shm.WinShmReceive(respBuf, 30000)
			if err != nil {
				errors++
				continue
			}
			if respLen >= protocol.HeaderSize+8 {
				respVal := binary.NativeEndian.Uint64(respBuf[protocol.HeaderSize : protocol.HeaderSize+8])
				if respVal != counter+1 {
					fmt.Fprintf(os.Stderr, "counter chain broken: expected %d, got %d\n", counter+1, respVal)
					errors++
				}
			}
		} else {
			// Named Pipe path
			if err := session.Send(&hdr, reqPayload[:]); err != nil {
				errors++
				continue
			}

			_, payload, err := session.Receive(recvBuf)
			if err != nil {
				errors++
				continue
			}
			if len(payload) >= 8 {
				respVal := binary.NativeEndian.Uint64(payload[:8])
				if respVal != counter+1 {
					fmt.Fprintf(os.Stderr, "counter chain broken: expected %d, got %d\n", counter+1, respVal)
					errors++
				}
			}
		}

		sampleFinishWin(lr, t0)

		counter++
		requests++
	}

	wallSec := float64(nowNS()-wallStart) / 1e9
	cpuEnd := cpuNSWin()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9
	throughput := float64(requests) / wallSec
	cpuPct := (cpuSec / wallSec) * 100.0

	p50 := latencyUSWin(lr.percentile(50.0))
	p95 := latencyUSWin(lr.percentile(95.0))
	p99 := latencyUSWin(lr.percentile(99.0))

	fmt.Printf("%s,go,%s,%.0f,%.3f,%.3f,%.3f,%.1f,0.0,%.1f\n",
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
	client := cgroups.NewClient(runDir, service, typedClientConfigWin(profiles))

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

	lr := newLatencyRecorderWin(latencySparseCapWin(targetRPS, durationSec))
	rl := newRateLimiterWin(targetRPS)

	var requests, errors uint64

	cpuStart := cpuNSWin()
	wallStart := nowNS()
	tickDeadline := tickMSWin() + uint64(durationSec)*1000
	for tickMSWin() < tickDeadline {
		rl.wait()

		t0 := sampleStartWin(requests)

		view, err := client.CallSnapshot()

		if err != nil {
			errors++
			client.Refresh()
			continue
		}

		if view.ItemCount != 16 {
			fmt.Fprintf(os.Stderr, "snapshot: expected 16 items, got %d\n", view.ItemCount)
			errors++
		}

		sampleFinishWin(lr, t0)
		requests++
	}

	wallSec := float64(nowNS()-wallStart) / 1e9
	cpuEnd := cpuNSWin()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9
	throughput := float64(requests) / wallSec
	cpuPct := (cpuSec / wallSec) * 100.0

	p50 := latencyUSWin(lr.percentile(50.0))
	p95 := latencyUSWin(lr.percentile(95.0))
	p99 := latencyUSWin(lr.percentile(99.0))

	fmt.Printf("%s,go,%s,%.0f,%.3f,%.3f,%.3f,%.1f,0.0,%.1f\n",
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

	lookupIndex := make([]cacheBucketWin, 32)
	mask := uint32(len(lookupIndex) - 1)
	for i := range cacheItems {
		slot := (cacheItems[i].Hash ^ cacheHashNameWin(cacheItems[i].Name)) & mask
		for lookupIndex[slot].used {
			slot = (slot + 1) & mask
		}
		lookupIndex[slot] = cacheBucketWin{index: i, used: true}
	}

	var lookups, hits uint64

	cpuStart := cpuNSWin()
	wallStart := nowNS()
	tickDeadline := tickMSWin() + uint64(durationSec)*1000

	for tickMSWin() < tickDeadline {
		for _, it := range items {
			slot := (it.hash ^ cacheHashNameWin(it.name)) & mask
			found := false
			for lookupIndex[slot].used {
				bucketItem := cacheItems[lookupIndex[slot].index]
				if bucketItem.Hash == it.hash && bucketItem.Name == it.name {
					found = true
					break
				}
				slot = (slot + 1) & mask
			}
			if found {
				hits++
			}
			lookups++
		}
	}

	wallSec := float64(nowNS()-wallStart) / 1e9
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
	elevateBenchPriorityWin()

	if len(os.Args) < 2 {
		fmt.Fprintf(os.Stderr,
			"Usage: %s <subcommand> [args...]\n"+
				"Subcommands: np-ping-pong-{server,client}, shm-ping-pong-{server,client},\n"+
				"  np-batch-ping-pong-{server,client}, shm-batch-ping-pong-{server,client},\n"+
				"  snapshot-{server,client}, snapshot-shm-{server,client},\n"+
				"  np-pipeline-client, np-pipeline-batch-client, lookup-bench\n",
			os.Args[0])
		os.Exit(1)
	}

	cmd := os.Args[1]
	pinBenchAffinityWin(cmd)
	rc := 0

	switch cmd {
	case "np-ping-pong-client", "shm-ping-pong-client",
		"np-batch-ping-pong-client", "shm-batch-ping-pong-client",
		"snapshot-client", "snapshot-shm-client",
		"np-pipeline-client", "np-pipeline-batch-client",
		"lookup-bench":
		// Keep benchmark clients single-threaded for fair cross-language
		// comparison, but leave servers at the runtime default.
		runtime.GOMAXPROCS(1)
	}

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

	case "np-batch-ping-pong-server", "shm-batch-ping-pong-server":
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
		switch cmd {
		case "np-batch-ping-pong-server":
			profiles = profileNP
		case "shm-batch-ping-pong-server":
			profiles = profileWinSHM
		}

		rc = runBatchServerWin(runDir, service, profiles, duration)

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

	case "np-batch-ping-pong-client", "shm-batch-ping-pong-client":
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
		case "np-batch-ping-pong-client":
			profiles = profileNP
			scenario = "np-batch-ping-pong"
		case "shm-batch-ping-pong-client":
			profiles = profileWinSHM
			scenario = "shm-batch-ping-pong"
		}

		rc = runBatchPingPongClientWin(runDir, service, profiles, duration, targetRPS, scenario, "go")

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

	case "np-pipeline-client":
		if len(os.Args) < 7 {
			fmt.Fprintf(os.Stderr, "Usage: %s %s <run_dir> <service> <duration_sec> <target_rps> <depth>\n", os.Args[0], cmd)
			os.Exit(1)
		}
		runDir := os.Args[2]
		service := os.Args[3]
		duration, _ := strconv.Atoi(os.Args[4])
		targetRPS, _ := strconv.ParseUint(os.Args[5], 10, 64)
		depth, _ := strconv.Atoi(os.Args[6])
		if depth < 1 {
			depth = 1
		}

		rc = runPipelineClientWin(runDir, service, duration, targetRPS, depth, "go")

	case "np-pipeline-batch-client":
		if len(os.Args) < 7 {
			fmt.Fprintf(os.Stderr, "Usage: %s %s <run_dir> <service> <duration_sec> <target_rps> <depth>\n", os.Args[0], cmd)
			os.Exit(1)
		}
		runDir := os.Args[2]
		service := os.Args[3]
		duration, _ := strconv.Atoi(os.Args[4])
		targetRPS, _ := strconv.ParseUint(os.Args[5], 10, 64)
		depth, _ := strconv.Atoi(os.Args[6])
		if depth < 1 {
			depth = 1
		}

		rc = runPipelineBatchClientWin(runDir, service, duration, targetRPS, depth, "go")

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
