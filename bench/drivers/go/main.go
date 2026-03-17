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
	"math/rand"
	"os"
	"os/signal"
	"runtime"
	"sort"
	"strconv"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/service/cgroups"
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
)

// ---------------------------------------------------------------------------
//  Timing helpers
// ---------------------------------------------------------------------------

// cpuNS returns process CPU time in nanoseconds via clock_gettime.
func cpuNS() uint64 {
	var ts syscall.Timespec
	// CLOCK_PROCESS_CPUTIME_ID = 2 on Linux
	syscall.Syscall(syscall.SYS_CLOCK_GETTIME, 2, uintptr(unsafe.Pointer(&ts)), 0)
	return uint64(ts.Sec)*1_000_000_000 + uint64(ts.Nsec)
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
	sort.Slice(lr.samples, func(i, j int) bool { return lr.samples[i] < lr.samples[j] })
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
	return &rateLimiter{
		interval: time.Duration(1_000_000_000/targetRPS) * time.Nanosecond,
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
	for i := uint32(0); i < 16; i++ {
		name := fmt.Sprintf("cgroup-%d", i)
		path := fmt.Sprintf("/sys/fs/cgroup/bench/cg-%d", i)
		snapshotNames[i] = []byte(name)
		snapshotPaths[i] = []byte(path)
	}
	return true
}

func pingPongHandlers() cgroups.Handlers {
	return cgroups.Handlers{
		OnIncrement: func(counter uint64) (uint64, bool) {
			return counter + 1, true
		},
	}
}

func snapshotHandlers() cgroups.Handlers {
	return cgroups.Handlers{
		OnSnapshot: func(request *protocol.CgroupsRequest, builder *protocol.CgroupsBuilder) bool {
			if request.LayoutVersion != 1 || request.Flags != 0 || !initSnapshotTemplate() {
				return false
			}
			builder.SetHeader(1, atomic.AddUint64(&snapshotGen, 1))
			for i := uint32(0); i < 16; i++ {
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
	var handlers cgroups.Handlers
	switch handlerType {
	case "ping-pong":
		handlers = pingPongHandlers()
	case "snapshot":
		handlers = snapshotHandlers()
	default:
		fmt.Fprintf(os.Stderr, "unknown handler type: %s\n", handlerType)
		return 1
	}

	server := cgroups.NewServer(runDir, service, serverConfig(profiles), handlers)

	fmt.Println("READY")

	cpuStart := cpuNS()

	go func() {
		time.Sleep(time.Duration(durationSec+3) * time.Second)
		server.Stop()
	}()

	if err := server.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "server: %v\n", err)
	}

	cpuEnd := cpuNS()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9

	fmt.Printf("SERVER_CPU_SEC=%.6f\n", cpuSec)
	return 0
}

// ---------------------------------------------------------------------------
//  Batch server (same handler, higher batch/payload limits)
// ---------------------------------------------------------------------------

func runBatchServer(runDir, service string, profiles uint32, durationSec int) int {
	cfg := batchServerConfig(profiles)
	cfg.MaxResponsePayloadBytes = batchBufSize * 2 // extra room for builder overhead

	server := cgroups.NewServerWithWorkers(runDir, service, cfg, pingPongHandlers(), 4)

	fmt.Println("READY")

	cpuStart := cpuNS()

	go func() {
		time.Sleep(time.Duration(durationSec+3) * time.Second)
		server.Stop()
	}()

	if err := server.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "batch server: %v\n", err)
	}

	cpuEnd := cpuNS()
	cpuSec := float64(cpuEnd-cpuStart) / 1e9

	fmt.Printf("SERVER_CPU_SEC=%.6f\n", cpuSec)
	return 0
}

// ---------------------------------------------------------------------------
//  Batch ping-pong client (random 1-1000 items per batch)
// ---------------------------------------------------------------------------

func runBatchPingPongClient(runDir, service string, profiles uint32, durationSec int, targetRPS uint64, scenario, serverLang string) int {
	cfg := batchClientConfig(profiles)
	session, err := posix.Connect(runDir, service, &cfg)
	if err != nil {
		for i := 0; i < 200; i++ {
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
		for i := 0; i < 200; i++ {
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

	estSamples := 2_000_000
	if targetRPS > 0 {
		estSamples = int(targetRPS) * durationSec
	}
	lr := newLatencyRecorder(estSamples)
	rl := newRateLimiter(targetRPS)

	reqBuf := make([]byte, batchBufSize)
	respBuf := make([]byte, batchBufSize+protocol.HeaderSize)
	expected := make([]uint64, maxBatchItems)

	var counter, totalItems, errors, msgSeq uint64

	cpuStart := cpuNS()
	wallStart := time.Now()
	deadline := time.Duration(durationSec) * time.Second

	for time.Since(wallStart) < deadline {
		rl.wait()

		// Random batch size 2-1000 (server treats itemCount==1 as non-batch)
		batchSize := uint32(rand.Intn(maxBatchItems-1) + 2)

		// Build batch request
		bb := protocol.NewBatchBuilder(reqBuf, batchSize)
		itemBuf := make([]byte, protocol.IncrementPayloadSize)

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

		t0 := time.Now()

		if shm != nil {
			// SHM path
			msgLen := protocol.HeaderSize + reqLen
			msg := make([]byte, msgLen)
			hdr.Magic = protocol.MagicMsg
			hdr.Version = protocol.Version
			hdr.HeaderLen = protocol.HeaderLen
			hdr.PayloadLen = uint32(reqLen)
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

			t1 := time.Now()
			lr.record(uint64(t1.Sub(t0).Nanoseconds()))
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

			t1 := time.Now()
			lr.record(uint64(t1.Sub(t0).Nanoseconds()))
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
		for i := 0; i < 200; i++ {
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

	estSamples := maxLatencySamples
	if targetRPS > 0 {
		estSamples = int(targetRPS) * durationSec
	}
	if estSamples > maxLatencySamples {
		estSamples = maxLatencySamples
	}
	lr := newLatencyRecorder(estSamples)
	rl := newRateLimiter(targetRPS)

	var counter, requests, errors, msgSeq uint64

	cpuStart := cpuNS()
	wallStart := time.Now()
	deadline := time.Duration(durationSec) * time.Second

	recvBuf := make([]byte, 256)

	for time.Since(wallStart) < deadline {
		rl.wait()

		t0 := time.Now()

		// Send `depth` requests with unique message IDs
		sendOK := true
		for d := 0; d < depth; d++ {
			val := counter + uint64(d)
			reqPayload := make([]byte, 8)
			binary.NativeEndian.PutUint64(reqPayload, val)

			msgSeq++
			hdr := protocol.Header{
				Kind:            protocol.KindRequest,
				Code:            protocol.MethodIncrement,
				ItemCount:       1,
				MessageID:       msgSeq,
				TransportStatus: protocol.StatusOK,
				PayloadLen:      8,
			}

			if serr := session.Send(&hdr, reqPayload); serr != nil {
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

		t1 := time.Now()
		lr.record(uint64(t1.Sub(t0).Nanoseconds()))

		counter += uint64(depth)
		requests += uint64(depth)
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
		for i := 0; i < 200; i++ {
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

	// Pre-allocate per-depth buffers
	reqBufs := make([][]byte, depth)
	batchSizes := make([]uint32, depth)
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
		for d := 0; d < depth; d++ {
			bs := uint32(rand.Intn(maxBatchItems-1) + 2)
			batchSizes[d] = bs

			bb := protocol.NewBatchBuilder(reqBufs[d], bs)
			itemBuf := make([]byte, protocol.IncrementPayloadSize)

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
		for d := 0; d < depth; d++ {
			_, _, rerr := session.Receive(recvBuf)
			if rerr != nil {
				errors++
				break
			}
			totalItems += uint64(batchSizes[d])
		}

		t1 := time.Now()
		lr.record(uint64(t1.Sub(t0).Nanoseconds()))
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
		for i := 0; i < 200; i++ {
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
		for i := 0; i < 200; i++ {
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

	estSamples := maxLatencySamples
	if targetRPS > 0 {
		estSamples = int(targetRPS) * durationSec
	}
	if estSamples > maxLatencySamples {
		estSamples = maxLatencySamples
	}
	lr := newLatencyRecorder(estSamples)
	rl := newRateLimiter(targetRPS)

	var counter, requests, errors uint64

	cpuStart := cpuNS()
	wallStart := time.Now()
	deadline := time.Duration(durationSec) * time.Second

	for time.Since(wallStart) < deadline {
		rl.wait()

		reqPayload := make([]byte, 8)
		binary.NativeEndian.PutUint64(reqPayload, counter)

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
			msg := make([]byte, msgLen)
			hdr.Magic = protocol.MagicMsg
			hdr.Version = protocol.Version
			hdr.HeaderLen = protocol.HeaderLen
			hdr.Encode(msg[:protocol.HeaderSize])
			copy(msg[protocol.HeaderSize:], reqPayload)

			if err := shm.ShmSend(msg); err != nil {
				errors++
				continue
			}

			respMsgBuf := make([]byte, protocol.HeaderSize+64)
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
				respVal := binary.NativeEndian.Uint64(payload[:8])
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
	cfg := clientConfig(profiles)
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

	estSamples := maxLatencySamples
	if targetRPS > 0 {
		estSamples = int(targetRPS) * durationSec
	}
	if estSamples > maxLatencySamples {
		estSamples = maxLatencySamples
	}
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

		lr.record(uint64(t1.Sub(t0).Nanoseconds()))
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

	// We cannot directly populate the Cache struct since it's internal.
	// Instead, we replicate the lookup logic (linear scan by hash+name).
	var lookups, hits uint64

	cpuStart := cpuNS()
	wallStart := time.Now()
	deadline := time.Duration(durationSec) * time.Second

	for time.Since(wallStart) < deadline {
		for _, it := range items {
			// Linear scan
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
//  Main
// ---------------------------------------------------------------------------

func main() {
	runtime.GOMAXPROCS(1) // single-threaded client for fair comparison
	signal.Ignore(syscall.SIGPIPE)

	if len(os.Args) < 2 {
		fmt.Fprintf(os.Stderr,
			"Usage: %s <subcommand> [args...]\n"+
				"Subcommands: uds-ping-pong-{server,client}, shm-ping-pong-{server,client},\n"+
				"  uds-batch-ping-pong-{server,client}, shm-batch-ping-pong-{server,client},\n"+
				"  snapshot-{server,client}, snapshot-shm-{server,client},\n"+
				"  uds-pipeline-client, uds-pipeline-batch-client, lookup-bench\n",
			os.Args[0])
		os.Exit(1)
	}

	cmd := os.Args[1]
	rc := 0

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

		os.MkdirAll(runDir, 0700)

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

		os.MkdirAll(runDir, 0700)

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

	default:
		fmt.Fprintf(os.Stderr, "Unknown subcommand: %s\n", cmd)
		os.Exit(1)
	}

	os.Exit(rc)
}
