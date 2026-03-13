package main

import (
	"errors"
	"fmt"
	"os"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/service/cgroupssnapshot"
)

func runCgroupsServerBench(serviceNamespace, serviceName string, maxRequests uint64, authToken uint64) {
	start := time.Now()
	cpuStart := selfCPUSeconds()
	runCgroupsServerLoop(serviceNamespace, serviceName, maxRequests, authToken)
	elapsedSec := time.Since(start).Seconds()
	serverCPUCores := 0.0
	if elapsedSec > 0 {
		serverCPUCores = (selfCPUSeconds() - cpuStart) / elapsedSec
	}
	fmt.Fprintf(os.Stderr, "SERVER_CPU_CORES=%.3f\n", serverCPUCores)
}

func validateLookup(client *cgroupssnapshot.Client, lookupHash uint32, lookupName string) error {
	item := client.Lookup(lookupHash, lookupName)
	if item == nil {
		return errors.New("expected lookup hit")
	}
	if item.Hash != lookupHash || item.Name != lookupName {
		return fmt.Errorf("unexpected lookup result: hash=%d name=%s", item.Hash, item.Name)
	}
	return nil
}

func clientRefreshBench(serviceNamespace, serviceName string, durationSec, targetRPS int, lookupHash uint32, lookupName string, authToken uint64) {
	if durationSec <= 0 {
		fmt.Fprintf(os.Stderr, "client-refresh-bench failed: duration_sec must be > 0\n")
		os.Exit(2)
	}
	if targetRPS < 0 {
		fmt.Fprintf(os.Stderr, "client-refresh-bench failed: target_rps must be >= 0\n")
		os.Exit(2)
	}

	config := cgroupssnapshot.NewConfig(serviceNamespace, serviceName)
	applyCgroupsClientProfiles(&config)
	config.AuthToken = authToken
	client, err := cgroupssnapshot.NewClient(config)
	if err != nil {
		fmt.Fprintf(os.Stderr, "client-refresh-bench failed: %v\n", err)
		os.Exit(1)
	}
	defer client.Close()

	start := time.Now()
	end := start.Add(time.Duration(durationSec) * time.Second)
	cpuStart := selfCPUSeconds()
	latNs := make([]int64, 0, 1024)
	requests := uint64(0)
	responses := uint64(0)

	for waitForBenchmarkSlot(start, end, targetRPS, requests) {
		requests++
		callStart := time.Now()
		if err := client.Refresh(10 * time.Second); err != nil {
			fmt.Fprintf(os.Stderr, "client-refresh-bench failed: %v\n", err)
			os.Exit(1)
		}
		if err := validateLookup(client, lookupHash, lookupName); err != nil {
			fmt.Fprintf(os.Stderr, "client-refresh-bench failed: %v\n", err)
			os.Exit(1)
		}
		latNs = append(latNs, time.Since(callStart).Nanoseconds())
		responses++
	}

	elapsedSec := time.Since(start).Seconds()
	result := benchResult{
		durationSec:    durationSec,
		targetRPS:      targetRPS,
		requests:       requests,
		responses:      responses,
		mismatches:     0,
		throughputRPS:  0,
		p50US:          percentileMicros(latNs, 50),
		p95US:          percentileMicros(latNs, 95),
		p99US:          percentileMicros(latNs, 99),
		clientCPUCores: 0,
	}
	if elapsedSec > 0 {
		result.throughputRPS = float64(responses) / elapsedSec
		result.clientCPUCores = (selfCPUSeconds() - cpuStart) / elapsedSec
	}

	printBenchHeader()
	printBenchRow("go-cgroups-refresh", result)
}

func clientLookupBench(serviceNamespace, serviceName string, durationSec, targetRPS int, lookupHash uint32, lookupName string, authToken uint64) {
	if durationSec <= 0 {
		fmt.Fprintf(os.Stderr, "client-lookup-bench failed: duration_sec must be > 0\n")
		os.Exit(2)
	}
	if targetRPS < 0 {
		fmt.Fprintf(os.Stderr, "client-lookup-bench failed: target_rps must be >= 0\n")
		os.Exit(2)
	}

	config := cgroupssnapshot.NewConfig(serviceNamespace, serviceName)
	applyCgroupsClientProfiles(&config)
	config.AuthToken = authToken
	client, err := cgroupssnapshot.NewClient(config)
	if err != nil {
		fmt.Fprintf(os.Stderr, "client-lookup-bench failed: %v\n", err)
		os.Exit(1)
	}
	defer client.Close()
	if err := client.Refresh(10 * time.Second); err != nil {
		fmt.Fprintf(os.Stderr, "client-lookup-bench failed: %v\n", err)
		os.Exit(1)
	}
	if err := validateLookup(client, lookupHash, lookupName); err != nil {
		fmt.Fprintf(os.Stderr, "client-lookup-bench failed: %v\n", err)
		os.Exit(1)
	}

	start := time.Now()
	end := start.Add(time.Duration(durationSec) * time.Second)
	cpuStart := selfCPUSeconds()
	latNs := make([]int64, 0, 1024)
	requests := uint64(0)
	responses := uint64(0)

	for waitForBenchmarkSlot(start, end, targetRPS, requests) {
		requests++
		callStart := time.Now()
		item := client.Lookup(lookupHash, lookupName)
		if item == nil || item.Hash != lookupHash || item.Name != lookupName {
			fmt.Fprintf(os.Stderr, "client-lookup-bench failed: expected lookup hit\n")
			os.Exit(1)
		}
		latNs = append(latNs, time.Since(callStart).Nanoseconds())
		responses++
	}

	elapsedSec := time.Since(start).Seconds()
	result := benchResult{
		durationSec:    durationSec,
		targetRPS:      targetRPS,
		requests:       requests,
		responses:      responses,
		mismatches:     0,
		throughputRPS:  0,
		p50US:          percentileMicros(latNs, 50),
		p95US:          percentileMicros(latNs, 95),
		p99US:          percentileMicros(latNs, 99),
		clientCPUCores: 0,
	}
	if elapsedSec > 0 {
		result.throughputRPS = float64(responses) / elapsedSec
		result.clientCPUCores = (selfCPUSeconds() - cpuStart) / elapsedSec
	}

	printBenchHeader()
	printBenchRow("go-cgroups-lookup", result)
}
