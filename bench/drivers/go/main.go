//go:build unix

// bench_posix - POSIX benchmark driver for netipc (Go).
//
// Exercises the public L1/L2/L3 API surface. Measures throughput,
// latency (p50/p95/p99), and CPU.
//
// Same subcommands and output format as the C driver.
// Pure Go — no cgo. Works with CGO_ENABLED=0.
package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"os/signal"
	"runtime"
	"slices"
	"strconv"
	"strings"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	cgroups "github.com/netdata/plugin-ipc/go/pkg/netipc/service/cgroups_snapshot"
	rawsvc "github.com/netdata/plugin-ipc/go/pkg/netipc/service/raw"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix"
)

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

const (
	authToken         = uint64(0xBE4C400000C0FFEE)
	responseBufSize   = 65536
	maxLatencySamples = 10_000_000
	defaultDuration   = 30
	profileUDS        = protocol.ProfileBaseline
	profileSHM        = protocol.ProfileBaseline | protocol.ProfileSHMHybrid

	// Batch scenario limits (mirrors C driver).
	maxBatchItems = 1000
	batchBufSize  = maxBatchItems*48 + 4096 // 52096

	lookupMethodMaxItems        = 32768
	lookupMethodBufBytesPerItem = 256
)

// ---------------------------------------------------------------------------
//  Timing helpers
// ---------------------------------------------------------------------------

// cpuNS returns process CPU time in nanoseconds via clock_gettime.
func cpuNS() uint64 {
	var ts syscall.Timespec
	// CLOCK_PROCESS_CPUTIME_ID = 2 on Linux
	_, _, errno := syscall.Syscall(syscall.SYS_CLOCK_GETTIME, 2, uintptr(unsafe.Pointer(&ts)), 0) // #nosec G103 -- clock_gettime requires passing a Timespec pointer.
	if errno != 0 {
		return 0
	}
	if ts.Sec < 0 || ts.Nsec < 0 {
		return 0
	}
	sec := uint64(ts.Sec)   // #nosec G115 -- clock_gettime returned a non-negative value.
	nsec := uint64(ts.Nsec) // #nosec G115 -- clock_gettime returned a non-negative value.
	return sec*1_000_000_000 + nsec
}

func u32FromNonNegativeInt(value int) (uint32, bool) {
	if value < 0 || uint64(value) > uint64(^uint32(0)) {
		return 0, false
	}
	return uint32(value), true // #nosec G115 -- value is bounded by the uint32 maximum above.
}

func u64FromNonNegativeInt(value int) (uint64, bool) {
	if value < 0 {
		return 0, false
	}
	return uint64(value), true // #nosec G115 -- value is checked non-negative above.
}

func durationNanos(d time.Duration) uint64 {
	if d <= 0 {
		return 0
	}
	return uint64(d.Nanoseconds()) // #nosec G115 -- d is checked non-negative above.
}

func recordLatency(lr *latencyRecorder, start, end time.Time) {
	lr.record(durationNanos(end.Sub(start)))
}

func estimateSamples(defaultSamples int, targetRPS uint64, durationSec int) int {
	if targetRPS == 0 || durationSec <= 0 {
		return defaultSamples
	}
	duration := uint64(durationSec) // #nosec G115 -- durationSec is checked positive above.
	if targetRPS > uint64(maxLatencySamples)/duration {
		return maxLatencySamples
	}
	samples := targetRPS * duration
	return int(samples) // #nosec G115 -- samples is bounded by maxLatencySamples above.
}

type xorshift32 struct {
	state uint32
}

func newXorShift32() xorshift32 {
	return xorshift32{state: 12345}
}

func (rng *xorshift32) next() uint32 {
	rng.state ^= rng.state << 13
	rng.state ^= rng.state >> 17
	rng.state ^= rng.state << 5
	return rng.state
}

func randomBatchSize(rng *xorshift32) uint32 {
	return (rng.next() % (maxBatchItems - 1)) + 2
}

// ---------------------------------------------------------------------------
//  Latency recorder
// ---------------------------------------------------------------------------

type latencyRecorder struct {
	samples []uint64 // nanoseconds
	cap     int
}

func newLatencyRecorder(cap int) *latencyRecorder {
	if cap > maxLatencySamples {
		cap = maxLatencySamples
	}
	return &latencyRecorder{
		samples: make([]uint64, 0, cap),
		cap:     cap,
	}
}

func (lr *latencyRecorder) record(ns uint64) {
	if len(lr.samples) < lr.cap {
		lr.samples = append(lr.samples, ns)
	}
}

func (lr *latencyRecorder) percentile(pct float64) uint64 {
	if len(lr.samples) == 0 {
		return 0
	}
	slices.Sort(lr.samples)
	idx := int(pct / 100.0 * float64(len(lr.samples)-1))
	if idx >= len(lr.samples) {
		idx = len(lr.samples) - 1
	}
	return lr.samples[idx]
}

// ---------------------------------------------------------------------------
//  Rate limiter (adaptive sleep, no busy-wait)
// ---------------------------------------------------------------------------

type rateLimiter struct {
	interval time.Duration
	next     time.Time
	limited  bool
}

func newRateLimiter(targetRPS uint64) *rateLimiter {
	if targetRPS == 0 {
		return &rateLimiter{limited: false}
	}
	intervalNs := 1_000_000_000 / targetRPS
	return &rateLimiter{
		interval: time.Duration(intervalNs) * time.Nanosecond, // #nosec G115 -- intervalNs is bounded by 1e9.
		next:     time.Now(),
		limited:  true,
	}
}

func (rl *rateLimiter) wait() {
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

func serverConfig(profiles uint32) posix.ServerConfig {
	return posix.ServerConfig{
		SupportedProfiles:       profiles,
		PreferredProfiles:       profiles,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    1,
		MaxResponsePayloadBytes: responseBufSize,
		MaxResponseBatchItems:   1,
		AuthToken:               authToken,
		Backlog:                 4,
	}
}

func clientConfig(profiles uint32) posix.ClientConfig {
	return posix.ClientConfig{
		SupportedProfiles:       profiles,
		PreferredProfiles:       profiles,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    1,
		MaxResponsePayloadBytes: responseBufSize,
		MaxResponseBatchItems:   1,
		AuthToken:               authToken,
	}
}

func batchServerConfig(profiles uint32) posix.ServerConfig {
	return posix.ServerConfig{
		SupportedProfiles:       profiles,
		PreferredProfiles:       profiles,
		MaxRequestPayloadBytes:  batchBufSize,
		MaxRequestBatchItems:    maxBatchItems,
		MaxResponsePayloadBytes: batchBufSize,
		MaxResponseBatchItems:   maxBatchItems,
		AuthToken:               authToken,
		Backlog:                 4,
	}
}

func batchClientConfig(profiles uint32) posix.ClientConfig {
	return posix.ClientConfig{
		SupportedProfiles:       profiles,
		PreferredProfiles:       profiles,
		MaxRequestPayloadBytes:  batchBufSize,
		MaxRequestBatchItems:    maxBatchItems,
		MaxResponsePayloadBytes: batchBufSize,
		MaxResponseBatchItems:   maxBatchItems,
		AuthToken:               authToken,
	}
}

func typedServerConfig(profiles uint32) cgroups.ServerConfig {
	return cgroups.ServerConfig{
		SupportedProfiles:       profiles,
		PreferredProfiles:       profiles,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    1,
		MaxResponsePayloadBytes: responseBufSize,
		AuthToken:               authToken,
	}
}

func typedClientConfig(profiles uint32) cgroups.ClientConfig {
	return cgroups.ClientConfig{
		SupportedProfiles:       profiles,
		PreferredProfiles:       profiles,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    1,
		MaxResponsePayloadBytes: responseBufSize,
		AuthToken:               authToken,
	}
}

func stopServerAfter(runDir, service string, durationSec int, stop func()) {
	go func() {
		time.Sleep(time.Duration(durationSec+3) * time.Second)
		stop()

		session, err := posix.Connect(runDir, service, &posix.ClientConfig{
			SupportedProfiles:       protocol.ProfileBaseline,
			PreferredProfiles:       protocol.ProfileBaseline,
			MaxRequestPayloadBytes:  4096,
			MaxRequestBatchItems:    1,
			MaxResponsePayloadBytes: responseBufSize,
			MaxResponseBatchItems:   1,
			AuthToken:               authToken,
		})
		if err == nil {
			session.Close()
		}
	}()
}

// ---------------------------------------------------------------------------
//  Snapshot handler (16 cgroup items)
// ---------------------------------------------------------------------------

var snapshotGen uint64
var snapshotNames [][]byte
var snapshotPaths [][]byte

func initSnapshotTemplate() bool {
	if snapshotNames != nil {
		return true
	}

	snapshotNames = make([][]byte, 16)
	snapshotPaths = make([][]byte, 16)
	for i := range uint32(16) {
		name := fmt.Sprintf("cgroup-%d", i)
		path := fmt.Sprintf("/sys/fs/cgroup/bench/cg-%d", i)
		snapshotNames[i] = []byte(name)
		snapshotPaths[i] = []byte(path)
	}
	return true
}

func pingPongDispatch() rawsvc.DispatchHandler {
	return rawsvc.IncrementDispatch(func(counter uint64) (uint64, bool) {
		return counter + 1, true
	})
}

func snapshotHandler() cgroups.Handler {
	return cgroups.Handler{
		Handle: func(request *protocol.CgroupsRequest, builder *protocol.CgroupsBuilder) bool {
			if request.LayoutVersion != 1 || request.Flags != 0 || !initSnapshotTemplate() {
				return false
			}
			builder.SetHeader(1, atomic.AddUint64(&snapshotGen, 1))
			for i := range uint32(16) {
				if err := builder.Add(1000+i, 0, i%2, snapshotNames[i], snapshotPaths[i]); err != nil {
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

func runServer(runDir, service string, profiles uint32, durationSec int, handlerType string) int {
	switch handlerType {
	case "ping-pong":
		server := rawsvc.NewServer(
			runDir, service, serverConfig(profiles), protocol.MethodIncrement, pingPongDispatch(),
		)

		// Benchmark only steady-state work, not one-off startup garbage.
		runtime.GC()
		fmt.Println("READY")

		cpuStart := cpuNS()

		stopServerAfter(runDir, service, durationSec, server.Stop)

		if err := server.Run(); err != nil {
			fmt.Fprintf(os.Stderr, "server: %v\n", err)
		}

		cpuEnd := cpuNS()
		cpuSec := float64(cpuEnd-cpuStart) / 1e9

		fmt.Printf("SERVER_CPU_SEC=%.6f\n", cpuSec)
		return 0
	case "snapshot":
		server := cgroups.NewServer(runDir, service, typedServerConfig(profiles), snapshotHandler())

		// Benchmark only steady-state work, not one-off startup garbage.
		runtime.GC()
		fmt.Println("READY")

		cpuStart := cpuNS()

		stopServerAfter(runDir, service, durationSec, server.Stop)

		if err := server.Run(); err != nil {
			fmt.Fprintf(os.Stderr, "server: %v\n", err)
		}

		cpuEnd := cpuNS()
		cpuSec := float64(cpuEnd-cpuStart) / 1e9

		fmt.Printf("SERVER_CPU_SEC=%.6f\n", cpuSec)
		return 0
	default:
		fmt.Fprintf(os.Stderr, "unknown handler type: %s\n", handlerType)
		return 1
	}
}

// ---------------------------------------------------------------------------
//  Batch server (same handler, higher batch/payload limits)
// ---------------------------------------------------------------------------

func runBatchServer(runDir, service string, profiles uint32, durationSec int) int {
	cfg := batchServerConfig(profiles)
	cfg.MaxResponsePayloadBytes = batchBufSize * 2 // extra room for builder overhead

	server := rawsvc.NewServerWithWorkers(
		runDir, service, cfg, protocol.MethodIncrement, pingPongDispatch(), 4,
	)

	// Benchmark only steady-state work, not one-off startup garbage.
	runtime.GC()
	fmt.Println("READY")

	cpuStart := cpuNS()

	stopServerAfter(runDir, service, durationSec, server.Stop)

	if err := server.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "batch server: %v\n", err)
	}

	cpuEnd := cpuNS()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9

	fmt.Printf("SERVER_CPU_SEC=%.6f\n", cpuSec)
	return 0
}

// ---------------------------------------------------------------------------
//  Batch ping-pong client (random 2-1000 items per batch)
// ---------------------------------------------------------------------------

func runBatchPingPongClient(runDir, service string, profiles uint32, durationSec int, targetRPS uint64, scenario, serverLang string) int {
	cfg := batchClientConfig(profiles)
	session, err := posix.Connect(runDir, service, &cfg)
	if err != nil {
		for range 200 {
			time.Sleep(10 * time.Millisecond)
			session, err = posix.Connect(runDir, service, &cfg)
			if err == nil {
				break
			}
		}
		if err != nil {
			fmt.Fprintf(os.Stderr, "batch client: connect failed: %v\n", err)
			return 1
		}
	}
	defer session.Close()

	// SHM upgrade if negotiated
	var shm *posix.ShmContext
	if session.SelectedProfile == protocol.ProfileSHMHybrid ||
		session.SelectedProfile == protocol.ProfileSHMFutex {
		for range 200 {
			shmCtx, serr := posix.ShmClientAttach(runDir, service, session.SessionID)
			if serr == nil {
				shm = shmCtx
				break
			}
			time.Sleep(5 * time.Millisecond)
		}
	}
	if (session.SelectedProfile == protocol.ProfileSHMHybrid ||
		session.SelectedProfile == protocol.ProfileSHMFutex) && shm == nil {
		fmt.Fprintln(os.Stderr, "batch client: shm attach failed after retries")
		return 1
	}
	if shm != nil {
		defer shm.ShmClose()
	}

	estSamples := estimateSamples(2_000_000, targetRPS, durationSec)
	lr := newLatencyRecorder(estSamples)
	rl := newRateLimiter(targetRPS)
	rng := newXorShift32()

	reqBuf := make([]byte, batchBufSize)
	respBuf := make([]byte, batchBufSize+protocol.HeaderSize)
	expected := make([]uint64, maxBatchItems)
	itemBuf := make([]byte, protocol.IncrementPayloadSize)
	msgBuf := make([]byte, batchBufSize+protocol.HeaderSize)

	var counter, totalItems, errors, msgSeq uint64

	cpuStart := cpuNS()
	wallStart := time.Now()
	deadline := time.Duration(durationSec) * time.Second

	for time.Since(wallStart) < deadline {
		rl.wait()

		// Random batch size 2-1000 (server treats itemCount==1 as non-batch)
		batchSize := randomBatchSize(&rng)

		// Reuse a stack builder to avoid a heap object per request.
		var bb protocol.BatchBuilder
		bb.Reset(reqBuf, batchSize)

		buildOK := true
		for i := range batchSize {
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

		t0 := time.Now()

		if shm != nil {
			// SHM path
			msgLen := protocol.HeaderSize + reqLen
			msg := msgBuf[:msgLen]
			hdr.Magic = protocol.MagicMsg
			hdr.Version = protocol.Version
			hdr.HeaderLen = protocol.HeaderLen
			payloadLen, ok := u32FromNonNegativeInt(reqLen)
			if !ok {
				errors++
				continue
			}
			hdr.PayloadLen = payloadLen
			hdr.Encode(msg[:protocol.HeaderSize])
			copy(msg[protocol.HeaderSize:], reqBuf[:reqLen])

			if serr := shm.ShmSend(msg); serr != nil {
				errors++
				continue
			}

			shmRespLen, serr := shm.ShmReceive(respBuf, 30000)
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
				// Server returns single-item response (no batch encoding)
				respVal, derr := protocol.IncrementDecode(respPayload)
				if derr != nil {
					errors++
					batchOK = false
				} else if respVal != expected[0] {
					errors++
					batchOK = false
				}
			} else {
				for i := range batchSize {
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

			t1 := time.Now()
			recordLatency(lr, t0, t1)
			_ = batchOK
			totalItems += uint64(batchSize)
		} else {
			// UDS path
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
				// Server returns single-item response (no batch encoding)
				respVal, derr := protocol.IncrementDecode(payload)
				if derr != nil {
					errors++
					batchOK = false
				} else if respVal != expected[0] {
					errors++
					batchOK = false
				}
			} else {
				for i := range batchSize {
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

			t1 := time.Now()
			recordLatency(lr, t0, t1)
			_ = batchOK
			totalItems += uint64(batchSize)
		}

		counter += uint64(batchSize)
	}

	wallSec := time.Since(wallStart).Seconds()
	cpuEnd := cpuNS()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9
	throughput := float64(totalItems) / wallSec
	cpuPct := (cpuSec / wallSec) * 100.0

	p50 := lr.percentile(50.0) / 1000
	p95 := lr.percentile(95.0) / 1000
	p99 := lr.percentile(99.0) / 1000

	fmt.Printf("%s,go,%s,%.0f,%d,%d,%d,%.1f,0.0,%.1f\n",
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

func runPipelineClient(runDir, service string, durationSec int, targetRPS uint64, depth int, serverLang string) int {
	cfg := clientConfig(profileUDS)
	session, err := posix.Connect(runDir, service, &cfg)
	if err != nil {
		for range 200 {
			time.Sleep(10 * time.Millisecond)
			session, err = posix.Connect(runDir, service, &cfg)
			if err == nil {
				break
			}
		}
		if err != nil {
			fmt.Fprintf(os.Stderr, "pipeline client: connect failed: %v\n", err)
			return 1
		}
	}
	defer session.Close()

	estSamples := estimateSamples(maxLatencySamples, targetRPS, durationSec)
	lr := newLatencyRecorder(estSamples)
	rl := newRateLimiter(targetRPS)

	var counter, requests, errors, msgSeq uint64

	cpuStart := cpuNS()
	wallStart := time.Now()
	deadline := time.Duration(durationSec) * time.Second

	recvBuf := make([]byte, 256)
	var reqPayload [8]byte

	for time.Since(wallStart) < deadline {
		rl.wait()

		t0 := time.Now()

		// Send `depth` requests with unique message IDs
		sendOK := true
		for d := range depth {
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
		for d := range depth {
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

		t1 := time.Now()
		recordLatency(lr, t0, t1)

		depth64, ok := u64FromNonNegativeInt(depth)
		if !ok {
			errors++
			continue
		}
		counter += depth64
		requests += depth64
	}

	wallSec := time.Since(wallStart).Seconds()
	cpuEnd := cpuNS()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9
	throughput := float64(requests) / wallSec
	cpuPct := (cpuSec / wallSec) * 100.0

	p50 := lr.percentile(50.0) / 1000
	p95 := lr.percentile(95.0) / 1000
	p99 := lr.percentile(99.0) / 1000

	scenario := fmt.Sprintf("uds-pipeline-d%d", depth)
	fmt.Printf("%s,go,%s,%.0f,%d,%d,%d,%.1f,0.0,%.1f\n",
		scenario, serverLang, throughput, p50, p95, p99, cpuPct, cpuPct)

	if errors > 0 {
		fmt.Fprintf(os.Stderr, "pipeline client: %d errors\n", errors)
		return 1
	}
	return 0
}

// ---------------------------------------------------------------------------
//  Pipeline+batch client (sends depth batch msgs, reads depth responses)
// ---------------------------------------------------------------------------

func runPipelineBatchClient(runDir, service string, durationSec int, targetRPS uint64, depth int, serverLang string) int {
	cfg := batchClientConfig(profileUDS)
	session, err := posix.Connect(runDir, service, &cfg)
	if err != nil {
		for range 200 {
			time.Sleep(10 * time.Millisecond)
			session, err = posix.Connect(runDir, service, &cfg)
			if err == nil {
				break
			}
		}
		if err != nil {
			fmt.Fprintf(os.Stderr, "pipeline-batch client: connect failed: %v\n", err)
			return 1
		}
	}
	defer session.Close()

	lr := newLatencyRecorder(2_000_000)
	rl := newRateLimiter(targetRPS)
	rng := newXorShift32()

	// Pre-allocate per-depth buffers
	reqBufs := make([][]byte, depth)
	batchSizes := make([]uint32, depth)
	itemBuf := make([]byte, protocol.IncrementPayloadSize)
	for i := range reqBufs {
		reqBufs[i] = make([]byte, batchBufSize)
	}
	recvBuf := make([]byte, batchBufSize+protocol.HeaderSize)

	var counter, totalItems, errors, msgSeq uint64

	cpuStart := cpuNS()
	wallStart := time.Now()
	deadline := time.Duration(durationSec) * time.Second

	for time.Since(wallStart) < deadline {
		rl.wait()

		t0 := time.Now()

		// Build and send `depth` batch requests
		sendOK := true
		for d := range depth {
			bs := randomBatchSize(&rng)
			batchSizes[d] = bs

			var bb protocol.BatchBuilder
			bb.Reset(reqBufs[d], bs)

			for i := range bs {
				protocol.IncrementEncode(counter+uint64(i), itemBuf)
				if err := bb.Add(itemBuf); err != nil {
					sendOK = false
					errors++
					break
				}
			}
			if !sendOK {
				break
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

			if serr := session.Send(&hdr, reqBufs[d][:reqLen]); serr != nil {
				sendOK = false
				errors++
				break
			}

			counter += uint64(bs)
		}

		if !sendOK {
			continue
		}

		// Receive `depth` batch responses
		for d := range depth {
			_, _, rerr := session.Receive(recvBuf)
			if rerr != nil {
				errors++
				break
			}
			totalItems += uint64(batchSizes[d])
		}

		t1 := time.Now()
		recordLatency(lr, t0, t1)
	}

	wallSec := time.Since(wallStart).Seconds()
	cpuEnd := cpuNS()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9
	throughput := float64(totalItems) / wallSec
	cpuPct := (cpuSec / wallSec) * 100.0

	p50 := lr.percentile(50.0) / 1000
	p95 := lr.percentile(95.0) / 1000
	p99 := lr.percentile(99.0) / 1000

	scenario := fmt.Sprintf("uds-pipeline-batch-d%d", depth)
	fmt.Printf("%s,go,%s,%.0f,%d,%d,%d,%.1f,0.0,%.1f\n",
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

func runPingPongClient(runDir, service string, profiles uint32, durationSec int, targetRPS uint64, scenario, serverLang string) int {
	// Direct L1 connection for INCREMENT
	cfg := clientConfig(profiles)
	session, err := posix.Connect(runDir, service, &cfg)
	if err != nil {
		// Retry
		for range 200 {
			time.Sleep(10 * time.Millisecond)
			session, err = posix.Connect(runDir, service, &cfg)
			if err == nil {
				break
			}
		}
		if err != nil {
			fmt.Fprintf(os.Stderr, "client: connect failed: %v\n", err)
			return 1
		}
	}
	defer session.Close()

	// SHM upgrade if negotiated
	var shm *posix.ShmContext
	if session.SelectedProfile == protocol.ProfileSHMHybrid ||
		session.SelectedProfile == protocol.ProfileSHMFutex {
		for range 200 {
			shmCtx, serr := posix.ShmClientAttach(runDir, service, session.SessionID)
			if serr == nil {
				shm = shmCtx
				break
			}
			time.Sleep(5 * time.Millisecond)
		}
	}
	if (session.SelectedProfile == protocol.ProfileSHMHybrid ||
		session.SelectedProfile == protocol.ProfileSHMFutex) && shm == nil {
		fmt.Fprintln(os.Stderr, "client: shm attach failed after retries")
		return 1
	}
	if shm != nil {
		defer shm.ShmClose()
	}

	estSamples := estimateSamples(maxLatencySamples, targetRPS, durationSec)
	lr := newLatencyRecorder(estSamples)
	rl := newRateLimiter(targetRPS)

	var counter, requests, errors uint64
	var reqPayload [8]byte
	msgBuf := make([]byte, protocol.HeaderSize+8)
	respMsgBuf := make([]byte, protocol.HeaderSize+64)
	recvBuf := make([]byte, 256)

	cpuStart := cpuNS()
	wallStart := time.Now()
	deadline := time.Duration(durationSec) * time.Second

	for time.Since(wallStart) < deadline {
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

		t0 := time.Now()

		if shm != nil {
			// SHM path
			msgLen := protocol.HeaderSize + 8
			msg := msgBuf[:msgLen]
			hdr.Magic = protocol.MagicMsg
			hdr.Version = protocol.Version
			hdr.HeaderLen = protocol.HeaderLen
			hdr.Encode(msg[:protocol.HeaderSize])
			copy(msg[protocol.HeaderSize:], reqPayload[:])

			if err := shm.ShmSend(msg); err != nil {
				errors++
				continue
			}

			respMsgLen, err := shm.ShmReceive(respMsgBuf, 30000)
			if err != nil {
				errors++
				continue
			}
			if respMsgLen >= protocol.HeaderSize+8 {
				respVal := binary.NativeEndian.Uint64(respMsgBuf[protocol.HeaderSize : protocol.HeaderSize+8])
				if respVal != counter+1 {
					fmt.Fprintf(os.Stderr, "counter chain broken: expected %d, got %d\n", counter+1, respVal)
					errors++
				}
			}
		} else {
			// UDS path
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

		t1 := time.Now()
		recordLatency(lr, t0, t1)

		counter++
		requests++
	}

	wallSec := time.Since(wallStart).Seconds()
	cpuEnd := cpuNS()
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
	}

	if errors > 0 {
		return 1
	}
	return 0
}

// ---------------------------------------------------------------------------
//  Snapshot client (L2 typed call)
// ---------------------------------------------------------------------------

func runSnapshotClient(runDir, service string, profiles uint32, durationSec int, targetRPS uint64, scenario, serverLang string) int {
	client := cgroups.NewClient(runDir, service, typedClientConfig(profiles))

	for range 200 {
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

	estSamples := estimateSamples(maxLatencySamples, targetRPS, durationSec)
	lr := newLatencyRecorder(estSamples)
	rl := newRateLimiter(targetRPS)

	var requests, errors uint64

	cpuStart := cpuNS()
	wallStart := time.Now()
	deadline := time.Duration(durationSec) * time.Second
	for time.Since(wallStart) < deadline {
		rl.wait()

		t0 := time.Now()

		view, err := client.CallSnapshot()
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

		recordLatency(lr, t0, t1)
		requests++
	}

	wallSec := time.Since(wallStart).Seconds()
	cpuEnd := cpuNS()
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
//  Lookup benchmark (L3 cache, no transport)
// ---------------------------------------------------------------------------

func runLookupBench(durationSec int) int {
	// Build a synthetic cache with 16 items
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

	// Build cache items matching the Cache's internal format
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

	cache := rawsvc.NewCache("/tmp", "bench_lookup", posix.ClientConfig{
		SupportedProfiles:       protocol.ProfileBaseline,
		PreferredProfiles:       0,
		MaxRequestBatchItems:    1,
		MaxResponsePayloadBytes: responseBufSize,
		MaxResponseBatchItems:   1,
		AuthToken:               authToken,
	})
	if !cache.SeedForTests(cacheItems, 1, 1) {
		fmt.Fprintln(os.Stderr, "cache seed failed")
		return 1
	}

	var lookups, hits uint64

	cpuStart := cpuNS()
	wallStart := time.Now()
	deadline := time.Duration(durationSec) * time.Second

	for time.Since(wallStart) < deadline {
		guard := cache.ReadLock()
		for _, it := range items {
			if guard.Get(it.hash, it.name) != nil {
				hits++
			}
			lookups++
		}
		guard.Unlock()
	}

	wallSec := time.Since(wallStart).Seconds()
	cpuEnd := cpuNS()
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
//  Lookup method benchmark (codec + dispatch, no transport)
// ---------------------------------------------------------------------------

type lookupVariant int

const (
	lookupKnown lookupVariant = iota + 1
	lookupUnknown
	lookupMixed
)

func lookupVariantIsKnown(variant lookupVariant, index int) bool {
	switch variant {
	case lookupKnown:
		return true
	case lookupUnknown:
		return false
	default:
		return index%2 == 0
	}
}

func parseLookupMethodScenario(scenario string) (isApps bool, variant lookupVariant, itemCount int, ok bool) {
	switch {
	case strings.HasPrefix(scenario, "apps-lookup-"):
		isApps = true
	case strings.HasPrefix(scenario, "cgroups-lookup-"):
		isApps = false
	default:
		return false, 0, 0, false
	}
	switch {
	case strings.Contains(scenario, "-known-"):
		variant = lookupKnown
	case strings.Contains(scenario, "-unknown-"):
		variant = lookupUnknown
	case strings.Contains(scenario, "-mixed-"):
		variant = lookupMixed
	default:
		return false, 0, 0, false
	}
	switch {
	case strings.HasSuffix(scenario, "-32768"):
		itemCount = 32768
	case strings.HasSuffix(scenario, "-8192"):
		itemCount = 8192
	case strings.HasSuffix(scenario, "-256"):
		itemCount = 256
	case strings.HasSuffix(scenario, "-16"):
		itemCount = 16
	case strings.HasSuffix(scenario, "-1"):
		itemCount = 1
	default:
		return false, 0, 0, false
	}
	return isApps, variant, itemCount, true
}

func lookupMethodBufferSize(itemCount int) int {
	scaled := itemCount*lookupMethodBufBytesPerItem + 4096
	if scaled < responseBufSize {
		return responseBufSize
	}
	return scaled
}

func runLookupMethodBench(durationSec int, scenario string, targetRPS uint64) int {
	isApps, variant, itemCount, ok := parseLookupMethodScenario(scenario)
	if !ok {
		fmt.Fprintf(os.Stderr, "lookup-method-bench: invalid scenario %q\n", scenario)
		return 1
	}
	if itemCount <= 0 || itemCount > lookupMethodMaxItems {
		fmt.Fprintf(os.Stderr, "lookup-method-bench: unsupported item count %d\n", itemCount)
		return 1
	}

	paths := make([][]byte, itemCount)
	pids := make([]uint32, itemCount)
	for i := 0; i < itemCount; i++ {
		paths[i] = fmt.Appendf(nil, "/sys/fs/cgroup/bench/cg-%03d", i)
		pids[i] = uint32(1000 + i) // #nosec G115 -- itemCount is bounded to lookupMethodMaxItems.
	}

	ioBufSize := lookupMethodBufferSize(itemCount)
	reqBuf := make([]byte, ioBufSize)
	respBuf := make([]byte, ioBufSize)
	lr := newLatencyRecorder(2_000_000)
	rl := newRateLimiter(targetRPS)

	var requests, errors uint64
	cpuStart := cpuNS()
	wallStart := time.Now()
	deadline := time.Duration(durationSec) * time.Second

	for time.Since(wallStart) < deadline {
		rl.wait()
		t0 := time.Now()

		var err error
		var respLen int
		if isApps {
			var reqLen int
			reqLen, err = protocol.EncodeAppsLookupRequest(pids, reqBuf)
			if err == nil {
				respLen, err = protocol.DispatchAppsLookup(reqBuf[:reqLen], respBuf, func(req *protocol.AppsLookupRequestView, builder *protocol.AppsLookupBuilder) bool {
					for i := uint32(0); i < req.ItemCount; i++ {
						pid, ierr := req.Item(i)
						if ierr != nil {
							return false
						}
						if lookupVariantIsKnown(variant, int(i)) {
							if berr := builder.Add(
								protocol.PidLookupKnown,
								protocol.AppsCgroupKnown,
								protocol.OrchestratorDocker,
								pid, 1, 1000, 42,
								[]byte("bench"),
								[]byte("/sys/fs/cgroup/bench"),
								[]byte("bench-container"),
								[]struct{ Key, Value []byte }{{Key: []byte("image"), Value: []byte("bench:latest")}},
							); berr != nil {
								return false
							}
						} else if berr := builder.Add(
							protocol.PidLookupUnknown,
							protocol.AppsCgroupKnown,
							0,
							pid, 0, protocol.NipcUIDUnset, 0,
							nil, nil, nil, nil,
						); berr != nil {
							return false
						}
					}
					return true
				})
				if err == nil {
					var view *protocol.AppsLookupResponseView
					view, err = protocol.DecodeAppsLookupResponse(respBuf[:respLen])
					if err == nil && int(view.ItemCount) != itemCount {
						err = protocol.ErrBadItemCount
					}
				}
			}
		} else {
			var reqLen int
			reqLen, err = protocol.EncodeCgroupsLookupRequest(paths, reqBuf)
			if err == nil {
				respLen, err = protocol.DispatchCgroupsLookup(reqBuf[:reqLen], respBuf, func(req *protocol.CgroupsLookupRequestView, builder *protocol.CgroupsLookupBuilder) bool {
					for i := uint32(0); i < req.ItemCount; i++ {
						if lookupVariantIsKnown(variant, int(i)) {
							if berr := builder.AddRequestItem(
								req,
								i,
								protocol.CgroupLookupKnown,
								protocol.OrchestratorK8s,
								[]byte("bench-pod"),
								[]struct{ Key, Value []byte }{
									{Key: []byte("namespace"), Value: []byte("bench")},
									{Key: []byte("image"), Value: []byte("bench:latest")},
								},
							); berr != nil {
								return false
							}
						} else if berr := builder.AddRequestItem(
							req,
							i,
							protocol.CgroupLookupUnknownRetryLater,
							0,
							nil,
							nil,
						); berr != nil {
							return false
						}
					}
					return true
				})
				if err == nil {
					var view *protocol.CgroupsLookupResponseView
					view, err = protocol.DecodeCgroupsLookupResponse(respBuf[:respLen])
					if err == nil && int(view.ItemCount) != itemCount {
						err = protocol.ErrBadItemCount
					}
				}
			}
		}

		if err != nil {
			errors++
			continue
		}

		recordLatency(lr, t0, time.Now())
		requests++
	}

	wallSec := time.Since(wallStart).Seconds()
	cpuEnd := cpuNS()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9
	throughput := float64(requests) / wallSec
	cpuPct := (cpuSec / wallSec) * 100.0

	p50 := lr.percentile(50.0) / 1000
	p95 := lr.percentile(95.0) / 1000
	p99 := lr.percentile(99.0) / 1000

	fmt.Printf("%s,go,go,%.0f,%d,%d,%d,%.1f,0.0,%.1f\n",
		scenario, throughput, p50, p95, p99, cpuPct, cpuPct)

	if errors > 0 {
		fmt.Fprintf(os.Stderr, "lookup-method-bench: %d errors\n", errors)
		return 1
	}
	return 0
}

// ---------------------------------------------------------------------------
//  Main
// ---------------------------------------------------------------------------

func main() {
	signal.Ignore(syscall.SIGPIPE)

	if len(os.Args) < 2 {
		fmt.Fprintf(os.Stderr,
			"Usage: %s <subcommand> [args...]\n"+
				"Subcommands: uds-ping-pong-{server,client}, shm-ping-pong-{server,client},\n"+
				"  uds-batch-ping-pong-{server,client}, shm-batch-ping-pong-{server,client},\n"+
				"  snapshot-{server,client}, snapshot-shm-{server,client},\n"+
				"  uds-pipeline-client, uds-pipeline-batch-client, lookup-bench, lookup-method-bench\n",
			os.Args[0])
		os.Exit(1)
	}

	cmd := os.Args[1]
	rc := 0

	switch cmd {
	case "uds-ping-pong-client", "shm-ping-pong-client",
		"uds-batch-ping-pong-client", "shm-batch-ping-pong-client",
		"snapshot-client", "snapshot-shm-client",
		"uds-pipeline-client", "uds-pipeline-batch-client",
		"lookup-bench", "lookup-method-bench":
		// Keep benchmark clients single-threaded for fair cross-language
		// comparison, but leave servers at the runtime default.
		runtime.GOMAXPROCS(1)
	}

	switch cmd {
	case "uds-ping-pong-server", "shm-ping-pong-server",
		"snapshot-server", "snapshot-shm-server":
		if len(os.Args) < 4 {
			fmt.Fprintf(os.Stderr, "Usage: %s %s <run_dir> <service> [duration_sec]\n", os.Args[0], cmd)
			os.Exit(1)
		}
		runDir := os.Args[2]
		service := os.Args[3]
		duration := defaultDuration
		if len(os.Args) >= 5 {
			if d, err := strconv.Atoi(os.Args[4]); err == nil {
				duration = d
			}
		}

		if err := os.MkdirAll(runDir, 0700); err != nil { // #nosec G703 -- benchmark driver uses caller-provided temporary run directory.
			fmt.Fprintf(os.Stderr, "mkdir %s: %v\n", runDir, err)
			os.Exit(1)
		}

		var profiles uint32
		var handlerType string
		switch cmd {
		case "uds-ping-pong-server":
			profiles = profileUDS
			handlerType = "ping-pong"
		case "shm-ping-pong-server":
			profiles = profileSHM
			handlerType = "ping-pong"
		case "snapshot-server":
			profiles = profileUDS
			handlerType = "snapshot"
		case "snapshot-shm-server":
			profiles = profileSHM
			handlerType = "snapshot"
		}

		rc = runServer(runDir, service, profiles, duration, handlerType)

	case "uds-batch-ping-pong-server", "shm-batch-ping-pong-server":
		if len(os.Args) < 4 {
			fmt.Fprintf(os.Stderr, "Usage: %s %s <run_dir> <service> [duration_sec]\n", os.Args[0], cmd)
			os.Exit(1)
		}
		runDir := os.Args[2]
		service := os.Args[3]
		duration := defaultDuration
		if len(os.Args) >= 5 {
			if d, err := strconv.Atoi(os.Args[4]); err == nil {
				duration = d
			}
		}

		if err := os.MkdirAll(runDir, 0700); err != nil { // #nosec G703 -- benchmark driver uses caller-provided temporary run directory.
			fmt.Fprintf(os.Stderr, "mkdir %s: %v\n", runDir, err)
			os.Exit(1)
		}

		var profiles uint32
		switch cmd {
		case "uds-batch-ping-pong-server":
			profiles = profileUDS
		case "shm-batch-ping-pong-server":
			profiles = profileSHM
		}

		rc = runBatchServer(runDir, service, profiles, duration)

	case "uds-ping-pong-client", "shm-ping-pong-client":
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
		case "uds-ping-pong-client":
			profiles = profileUDS
			scenario = "uds-ping-pong"
		case "shm-ping-pong-client":
			profiles = profileSHM
			scenario = "shm-ping-pong"
		}

		rc = runPingPongClient(runDir, service, profiles, duration, targetRPS, scenario, "go")

	case "uds-batch-ping-pong-client", "shm-batch-ping-pong-client":
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
		case "uds-batch-ping-pong-client":
			profiles = profileUDS
			scenario = "uds-batch-ping-pong"
		case "shm-batch-ping-pong-client":
			profiles = profileSHM
			scenario = "shm-batch-ping-pong"
		}

		rc = runBatchPingPongClient(runDir, service, profiles, duration, targetRPS, scenario, "go")

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
			profiles = profileUDS
			scenario = "snapshot-baseline"
		case "snapshot-shm-client":
			profiles = profileSHM
			scenario = "snapshot-shm"
		}

		rc = runSnapshotClient(runDir, service, profiles, duration, targetRPS, scenario, "go")

	case "uds-pipeline-client":
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

		rc = runPipelineClient(runDir, service, duration, targetRPS, depth, "go")

	case "uds-pipeline-batch-client":
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

		rc = runPipelineBatchClient(runDir, service, duration, targetRPS, depth, "go")

	case "lookup-bench":
		if len(os.Args) < 3 {
			fmt.Fprintf(os.Stderr, "Usage: %s lookup-bench <duration_sec>\n", os.Args[0])
			os.Exit(1)
		}
		duration, _ := strconv.Atoi(os.Args[2])
		rc = runLookupBench(duration)

	case "lookup-method-bench":
		if len(os.Args) < 5 {
			fmt.Fprintf(os.Stderr, "Usage: %s lookup-method-bench <duration_sec> <scenario> <target_rps>\n", os.Args[0])
			os.Exit(1)
		}
		duration, _ := strconv.Atoi(os.Args[2])
		targetRPS, _ := strconv.ParseUint(os.Args[4], 10, 64)
		rc = runLookupMethodBench(duration, os.Args[3], targetRPS)

	default:
		fmt.Fprintf(os.Stderr, "Unknown subcommand: %s\n", cmd)
		os.Exit(1)
	}

	os.Exit(rc)
}
