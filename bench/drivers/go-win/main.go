package main

import (
	"fmt"
	"os"
	"sort"
	"strconv"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"
)

func usage(argv0 string) {
	fmt.Fprintf(os.Stderr, "usage:\n")
	fmt.Fprintf(os.Stderr, "  %s server-once <run_dir> <service>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s client-once <run_dir> <service> <value>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s server-loop <run_dir> <service> <max_requests|0>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s client-bench <run_dir> <service> <duration_sec> <target_rps>\n", argv0)
}

func parseU64(s string) uint64 {
	v, err := strconv.ParseUint(s, 10, 64)
	if err != nil {
		fmt.Fprintf(os.Stderr, "invalid u64 %q: %v\n", s, err)
		os.Exit(2)
	}
	return v
}

func winConfig(runDir, service string) windows.Config {
	config := windows.NewConfig(runDir, service)
	if v := os.Getenv("NETIPC_SUPPORTED_PROFILES"); v != "" {
		if n, err := strconv.ParseUint(v, 10, 32); err == nil {
			config.SupportedProfiles = uint32(n)
		}
	}
	if v := os.Getenv("NETIPC_PREFERRED_PROFILES"); v != "" {
		if n, err := strconv.ParseUint(v, 10, 32); err == nil {
			config.PreferredProfiles = uint32(n)
		}
	}
	if v := os.Getenv("NETIPC_AUTH_TOKEN"); v != "" {
		if n, err := strconv.ParseUint(v, 10, 64); err == nil {
			config.AuthToken = n
		}
	}
	if v := os.Getenv("NETIPC_SHM_SPIN_TRIES"); v != "" {
		if n, err := strconv.ParseUint(v, 10, 32); err == nil {
			config.SHMSpinTries = uint32(n)
		}
	}
	return config
}

func percentileMicros(latNs []int64, pct float64) float64 {
	if len(latNs) == 0 {
		return 0
	}
	if pct <= 0 {
		return float64(latNs[0]) / 1000.0
	}
	if pct >= 100 {
		return float64(latNs[len(latNs)-1]) / 1000.0
	}
	rank := int((pct / 100.0) * float64(len(latNs)-1))
	if rank < 0 {
		rank = 0
	}
	if rank >= len(latNs) {
		rank = len(latNs) - 1
	}
	return float64(latNs[rank]) / 1000.0
}

func sleepUntil(targetNs int64) {
	for {
		now := time.Now().UnixNano()
		if now >= targetNs {
			return
		}
		diff := targetNs - now
		sleepNs := diff
		if diff > 2_000_000 {
			sleepNs = diff - 200_000
		} else if diff > 200_000 {
			sleepNs = diff - 50_000
		}
		time.Sleep(time.Duration(sleepNs))
	}
}

func serverOnce(runDir, service string) error {
	server, err := windows.Listen(winConfig(runDir, service))
	if err != nil {
		return err
	}
	defer server.Close()

	if err := server.Accept(10 * time.Second); err != nil {
		return err
	}

	requestID, request, err := server.ReceiveIncrement(10 * time.Second)
	if err != nil {
		return err
	}

	response := protocol.IncrementResponse{
		Status: protocol.StatusOK,
		Value:  request.Value + 1,
	}
	if err := server.SendIncrement(requestID, response, 10*time.Second); err != nil {
		return err
	}

	fmt.Printf(
		"GO-WIN-SERVER request_id=%d value=%d response=%d profile=%d\n",
		requestID, request.Value, response.Value, server.NegotiatedProfile(),
	)
	return nil
}

func clientOnce(runDir, service string, value uint64) error {
	client, err := windows.Dial(winConfig(runDir, service), 10*time.Second)
	if err != nil {
		return err
	}
	defer client.Close()

	response, err := client.CallIncrement(protocol.IncrementRequest{Value: value}, 10*time.Second)
	if err != nil {
		return err
	}
	if response.Status != protocol.StatusOK || response.Value != value+1 {
		return fmt.Errorf("unexpected increment response: status=%d value=%d", response.Status, response.Value)
	}

	fmt.Printf(
		"GO-WIN-CLIENT request=%d response=%d profile=%d\n",
		value, response.Value, client.NegotiatedProfile(),
	)
	return nil
}

func serverLoop(runDir, service string, maxRequests uint64) error {
	server, err := windows.Listen(winConfig(runDir, service))
	if err != nil {
		return err
	}
	defer server.Close()

	if err := server.Accept(10 * time.Second); err != nil {
		return err
	}

	cpuStart := windows.SelfCPUSeconds()
	wallStart := time.Now()

	handled := uint64(0)
	for maxRequests == 0 || handled < maxRequests {
		requestID, request, err := server.ReceiveIncrement(0)
		if err != nil {
			// Accept broken pipe / timeout as graceful shutdown for bench
			break
		}
		response := protocol.IncrementResponse{
			Status: protocol.StatusOK,
			Value:  request.Value + 1,
		}
		if err := server.SendIncrement(requestID, response, 0); err != nil {
			break
		}
		handled++
	}

	elapsed := time.Since(wallStart).Seconds()
	if elapsed <= 0 {
		elapsed = 1e-9
	}
	cpuCores := (windows.SelfCPUSeconds() - cpuStart) / elapsed
	fmt.Fprintf(os.Stderr, "SERVER_CPU_CORES=%.3f\n", cpuCores)

	return nil
}

func clientBench(runDir, service string, durationSec, targetRPS int) error {
	if durationSec <= 0 {
		return fmt.Errorf("duration_sec must be > 0")
	}
	if targetRPS < 0 {
		return fmt.Errorf("target_rps must be >= 0")
	}

	client, err := windows.Dial(winConfig(runDir, service), 10*time.Second)
	if err != nil {
		return err
	}
	defer client.Close()

	startNs := time.Now().UnixNano()
	endNs := startNs + int64(durationSec)*int64(time.Second)
	cpuStart := windows.SelfCPUSeconds()

	latNs := make([]int64, 0, 1<<20)
	counter := uint64(1)
	requests := 0
	responses := 0
	mismatches := 0

	var intervalNs int64
	nextSendNs := startNs
	if targetRPS > 0 {
		intervalNs = int64(time.Second) / int64(targetRPS)
		if intervalNs <= 0 {
			intervalNs = 1
		}
	}

	for {
		nowNs := time.Now().UnixNano()
		if nowNs >= endNs {
			break
		}

		if targetRPS > 0 {
			sleepUntil(nextSendNs)
			nextSendNs += intervalNs
		}

		sendStart := time.Now().UnixNano()
		requests++

		response, err := client.CallIncrement(protocol.IncrementRequest{Value: counter}, 0)
		if err != nil {
			return err
		}
		if response.Status != protocol.StatusOK || response.Value != counter+1 {
			mismatches++
		}

		counter = response.Value
		responses++
		latNs = append(latNs, time.Now().UnixNano()-sendStart)
	}

	elapsedSec := float64(time.Now().UnixNano()-startNs) / 1e9
	if elapsedSec <= 0 {
		elapsedSec = 1e-9
	}
	cpuCores := (windows.SelfCPUSeconds() - cpuStart) / elapsedSec
	throughput := float64(responses) / elapsedSec

	sort.Slice(latNs, func(i, j int) bool { return latNs[i] < latNs[j] })
	p50 := percentileMicros(latNs, 50)
	p95 := percentileMicros(latNs, 95)
	p99 := percentileMicros(latNs, 99)

	mode := "go-named-pipe"
	if client.NegotiatedProfile() == windows.ProfileSHMHybrid {
		mode = "go-shm-hybrid"
	}

	fmt.Println("mode,duration_sec,target_rps,requests,responses,mismatches,throughput_rps,p50_us,p95_us,p99_us,client_cpu_cores")
	fmt.Printf(
		"%s,%d,%d,%d,%d,%d,%.2f,%.2f,%.2f,%.2f,%.3f\n",
		mode, durationSec, targetRPS, requests, responses, mismatches,
		throughput, p50, p95, p99, cpuCores,
	)

	return nil
}

func main() {
	args := os.Args
	if len(args) < 2 {
		usage(args[0])
		os.Exit(2)
	}

	var err error
	switch args[1] {
	case "server-once":
		if len(args) != 4 {
			usage(args[0])
			os.Exit(2)
		}
		err = serverOnce(args[2], args[3])
	case "client-once":
		if len(args) != 5 {
			usage(args[0])
			os.Exit(2)
		}
		err = clientOnce(args[2], args[3], parseU64(args[4]))
	case "server-loop":
		if len(args) != 5 {
			usage(args[0])
			os.Exit(2)
		}
		err = serverLoop(args[2], args[3], parseU64(args[4]))
	case "client-bench":
		if len(args) != 6 {
			usage(args[0])
			os.Exit(2)
		}
		durationSec, parseErr := strconv.Atoi(args[4])
		if parseErr != nil {
			fmt.Fprintf(os.Stderr, "invalid duration_sec %q: %v\n", args[4], parseErr)
			os.Exit(2)
		}
		targetRPS, parseErr := strconv.Atoi(args[5])
		if parseErr != nil {
			fmt.Fprintf(os.Stderr, "invalid target_rps %q: %v\n", args[5], parseErr)
			os.Exit(2)
		}
		err = clientBench(args[2], args[3], durationSec, targetRPS)
	default:
		usage(args[0])
		os.Exit(2)
	}

	if err != nil {
		fmt.Fprintf(os.Stderr, "netipc-live-go-win failed: %v\n", err)
		os.Exit(1)
	}
}
