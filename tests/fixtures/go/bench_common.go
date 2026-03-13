package main

import (
	"fmt"
	"sort"
	"time"
)

type benchResult struct {
	durationSec    int
	targetRPS      int
	requests       uint64
	responses      uint64
	mismatches     uint64
	throughputRPS  float64
	p50US          float64
	p95US          float64
	p99US          float64
	clientCPUCores float64
}

func percentileMicros(latNs []int64, pct float64) float64 {
	if len(latNs) == 0 {
		return 0
	}

	sorted := append([]int64(nil), latNs...)
	sort.Slice(sorted, func(i, j int) bool {
		return sorted[i] < sorted[j]
	})

	index := int((pct / 100.0) * float64(len(sorted)-1))
	if index < 0 {
		index = 0
	}
	if index >= len(sorted) {
		index = len(sorted) - 1
	}
	return float64(sorted[index]) / 1000.0
}

func waitForBenchmarkSlot(start, end time.Time, targetRPS int, requestsSent uint64) bool {
	if targetRPS <= 0 {
		return time.Now().Before(end)
	}

	now := time.Now()
	if !now.Before(end) {
		return false
	}

	targetElapsedNs := (requestsSent * uint64(time.Second)) / uint64(targetRPS)
	dueAt := start.Add(time.Duration(targetElapsedNs))
	if now.Before(dueAt) {
		time.Sleep(dueAt.Sub(now))
	}

	return time.Now().Before(end)
}

func printBenchHeader() {
	fmt.Println("mode,duration_sec,target_rps,requests,responses,mismatches,throughput_rps,p50_us,p95_us,p99_us,client_cpu_cores")
}

func printBenchRow(mode string, result benchResult) {
	fmt.Printf(
		"%s,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f\n",
		mode,
		result.durationSec,
		result.targetRPS,
		result.requests,
		result.responses,
		result.mismatches,
		result.throughputRPS,
		result.p50US,
		result.p95US,
		result.p99US,
		result.clientCPUCores,
	)
}
