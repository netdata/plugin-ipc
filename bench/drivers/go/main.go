package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"syscall"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix"
)

const (
	negMagic   uint32 = 0x4e48534b
	negVersion uint16 = 1
	negHello   uint16 = 1
	negAck     uint16 = 2

	negStatusOK      uint32 = 0
	negPayloadOffset        = 8
	negStatusOffset         = 48
)

const (
	negOffMagic   = 0
	negOffVersion = 4
	negOffType    = 6
)

type benchResult struct {
	durationSec    int
	targetRPS      int
	requests       int
	responses      int
	mismatches     int
	elapsedSec     float64
	throughputRPS  float64
	p50US          float64
	p95US          float64
	p99US          float64
	clientCPUCores float64
}

func usage(argv0 string) {
	fmt.Fprintf(os.Stderr, "usage:\n")
	fmt.Fprintf(os.Stderr, "  %s uds-server-once <run_dir> <service>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s uds-client-once <run_dir> <service> <value>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s uds-server-loop <run_dir> <service> <max_requests|0>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s uds-server-bench <run_dir> <service> <max_requests|0>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s uds-client-bench <run_dir> <service> <duration_sec> <target_rps>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s uds-bench <run_dir> <service> <duration_sec> <target_rps>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s uds-client-badhello <run_dir> <service>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s uds-client-rawhello <run_dir> <service> <supported_mask> <preferred_mask> <auth_token>\n", argv0)
}

func parseU64(s string) uint64 {
	v, err := strconv.ParseUint(s, 10, 64)
	if err != nil {
		fmt.Fprintf(os.Stderr, "invalid u64 %q: %v\n", s, err)
		os.Exit(2)
	}
	return v
}

func parseU32(s string) uint32 {
	v := parseU64(s)
	if v > uint64(^uint32(0)) {
		fmt.Fprintf(os.Stderr, "invalid u32 %q\n", s)
		os.Exit(2)
	}
	return uint32(v)
}

func parseEnvU32(name string, fallback uint32) uint32 {
	value := os.Getenv(name)
	if value == "" {
		return fallback
	}
	return parseU32(value)
}

func parseEnvU64(name string, fallback uint64) uint64 {
	value := os.Getenv(name)
	if value == "" {
		return fallback
	}
	return parseU64(value)
}

func protocolError(message string) error {
	return fmt.Errorf("protocol error: %s", message)
}

func isDisconnectErr(err error) bool {
	return err != nil && (errorsIsEOF(err) || isConnReset(err))
}

func errorsIsEOF(err error) bool {
	return errors.Is(err, io.EOF)
}

func isConnReset(err error) bool {
	var opErr *net.OpError
	if errors.As(err, &opErr) {
		return opErr != nil && (isSyscall(opErr.Err, syscall.ECONNRESET) || isSyscall(opErr.Err, syscall.EPIPE) || isSyscall(opErr.Err, syscall.ENOTCONN))
	}
	return isSyscall(err, syscall.ECONNRESET) || isSyscall(err, syscall.EPIPE) || isSyscall(err, syscall.ENOTCONN)
}

func isSyscall(err error, target syscall.Errno) bool {
	return err != nil && errors.Is(err, target)
}

func encodeNegHeader(typ uint16) protocol.Frame {
	var frame protocol.Frame
	binary.LittleEndian.PutUint32(frame[negOffMagic:negOffMagic+4], negMagic)
	binary.LittleEndian.PutUint16(frame[negOffVersion:negOffVersion+2], negVersion)
	binary.LittleEndian.PutUint16(frame[negOffType:negOffType+2], typ)
	return frame
}

func encodeHelloNeg(payload protocol.HelloPayload) protocol.Frame {
	frame := encodeNegHeader(negHello)
	hello := protocol.EncodeHelloPayload(payload)
	copy(frame[negPayloadOffset:negPayloadOffset+protocol.ControlHelloPayloadLen], hello[:])
	return frame
}

func decodeAckNeg(frame protocol.Frame) (protocol.HelloAckPayload, uint32, error) {
	if err := decodeNegHeader(frame, negAck); err != nil {
		return protocol.HelloAckPayload{}, 0, err
	}
	ack, err := protocol.DecodeHelloAckPayload(frame[negPayloadOffset : negPayloadOffset+protocol.ControlHelloAckPayloadLen])
	if err != nil {
		return protocol.HelloAckPayload{}, 0, err
	}
	status := binary.LittleEndian.Uint32(frame[negStatusOffset : negStatusOffset+4])
	return ack, status, nil
}

func decodeNegHeader(frame protocol.Frame, expectedTyp uint16) error {
	magic := binary.LittleEndian.Uint32(frame[negOffMagic : negOffMagic+4])
	version := binary.LittleEndian.Uint16(frame[negOffVersion : negOffVersion+2])
	typ := binary.LittleEndian.Uint16(frame[negOffType : negOffType+2])
	if magic != negMagic || version != negVersion || typ != expectedTyp {
		return syscall.EPROTO
	}
	return nil
}

func udsReadFrame(conn *net.UnixConn, timeout time.Duration) (protocol.Frame, error) {
	var frame protocol.Frame
	if timeout > 0 {
		_ = conn.SetReadDeadline(time.Now().Add(timeout))
	} else {
		_ = conn.SetReadDeadline(time.Time{})
	}
	n, err := conn.Read(frame[:])
	if err != nil {
		return frame, err
	}
	if n != protocol.FrameSize {
		return frame, syscall.EPROTO
	}
	return frame, nil
}

func udsWriteFrame(conn *net.UnixConn, frame protocol.Frame, timeout time.Duration) error {
	if timeout > 0 {
		_ = conn.SetWriteDeadline(time.Now().Add(timeout))
	} else {
		_ = conn.SetWriteDeadline(time.Time{})
	}
	n, err := conn.Write(frame[:])
	if err != nil {
		return err
	}
	if n != protocol.FrameSize {
		return syscall.EPROTO
	}
	return nil
}

func udsConfig(runDir, service string) posix.Config {
	config := posix.NewConfig(runDir, service)
	config.SupportedProfiles = parseEnvU32("NETIPC_SUPPORTED_PROFILES", config.SupportedProfiles)
	config.PreferredProfiles = parseEnvU32("NETIPC_PREFERRED_PROFILES", config.PreferredProfiles)
	config.AuthToken = parseEnvU64("NETIPC_AUTH_TOKEN", config.AuthToken)
	return config
}

func udsServerOnce(runDir, service string) error {
	server, err := posix.Listen(udsConfig(runDir, service))
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
		"GO-UDS-SERVER request_id=%d value=%d response=%d profile=%d\n",
		requestID,
		request.Value,
		response.Value,
		server.NegotiatedProfile(),
	)
	return nil
}

func udsClientOnce(runDir, service string, value uint64) error {
	client, err := posix.Dial(udsConfig(runDir, service), 10*time.Second)
	if err != nil {
		return err
	}
	defer client.Close()

	response, err := client.CallIncrement(protocol.IncrementRequest{Value: value}, 10*time.Second)
	if err != nil {
		return err
	}
	if response.Status != protocol.StatusOK || response.Value != value+1 {
		return protocolError("unexpected increment response")
	}

	fmt.Printf(
		"GO-UDS-CLIENT request=%d response=%d profile=%d\n",
		value,
		response.Value,
		client.NegotiatedProfile(),
	)
	return nil
}

func udsServerLoop(runDir, service string, maxRequests uint64) error {
	_, err := udsServerLoopInternal(runDir, service, maxRequests)
	return err
}

func udsServerLoopInternal(runDir, service string, maxRequests uint64) (uint64, error) {
	server, err := posix.Listen(udsConfig(runDir, service))
	if err != nil {
		return 0, err
	}
	defer server.Close()

	if err := server.Accept(10 * time.Second); err != nil {
		return 0, err
	}

	handled := uint64(0)
	for maxRequests == 0 || handled < maxRequests {
		requestID, request, err := server.ReceiveIncrement(0)
		if err != nil {
			if isDisconnectErr(err) {
				return handled, nil
			}
			return handled, err
		}
		response := protocol.IncrementResponse{
			Status: protocol.StatusOK,
			Value:  request.Value + 1,
		}
		if err := server.SendIncrement(requestID, response, 0); err != nil {
			if isDisconnectErr(err) {
				return handled, nil
			}
			return handled, err
		}
		handled++
	}

	return handled, nil
}

func printServerBenchRow(label string, handled uint64, elapsedSec, serverCPUCores float64) {
	fmt.Printf("%s-server,%d,%.6f,%.3f\n", label, handled, elapsedSec, serverCPUCores)
}

func udsServerBench(runDir, service string, maxRequests uint64) error {
	start := time.Now()
	cpuStart := selfCPUSeconds()
	handled, err := udsServerLoopInternal(runDir, service, maxRequests)
	elapsedSec := time.Since(start).Seconds()
	serverCPUCores := 0.0
	if elapsedSec > 0 {
		serverCPUCores = (selfCPUSeconds() - cpuStart) / elapsedSec
	}
	if err == nil {
		printServerBenchRow("go-uds", handled, elapsedSec, serverCPUCores)
	}
	return err
}

func runClientBenchCapture(runDir, service string, durationSec, targetRPS int) (benchResult, error) {
	if durationSec <= 0 {
		return benchResult{}, fmt.Errorf("duration_sec must be > 0")
	}
	if targetRPS < 0 {
		return benchResult{}, fmt.Errorf("target_rps must be >= 0")
	}

	client, err := posix.Dial(udsConfig(runDir, service), 10*time.Second)
	if err != nil {
		return benchResult{}, err
	}
	defer client.Close()

	startNs := time.Now().UnixNano()
	endNs := startNs + int64(durationSec)*int64(time.Second)
	cpuStart := selfCPUSeconds()

	latNs := make([]int64, 0, 1<<20)
	counter := uint64(1)
	requests := 0
	responses := 0
	mismatches := 0

	for {
		if !waitForBenchmarkSlot(startNs, endNs, targetRPS, requests) {
			break
		}
		if time.Now().UnixNano() >= endNs {
			break
		}

		sendStart := time.Now().UnixNano()
		requests++

		response, err := client.CallIncrement(protocol.IncrementRequest{Value: counter}, 0)
		if err != nil {
			return benchResult{}, err
		}
		if response.Status != protocol.StatusOK {
			return benchResult{}, fmt.Errorf("server returned non-OK status during benchmark: %d", response.Status)
		}
		if response.Value != counter+1 {
			return benchResult{}, fmt.Errorf("benchmark counter mismatch: got=%d expected=%d", response.Value, counter+1)
		}

		counter = response.Value
		responses++
		latNs = append(latNs, time.Now().UnixNano()-sendStart)
	}

	elapsedSec := float64(time.Now().UnixNano()-startNs) / 1e9
	if elapsedSec <= 0 {
		elapsedSec = 1e-9
	}
	cpuCores := (selfCPUSeconds() - cpuStart) / elapsedSec
	throughput := float64(responses) / elapsedSec

	if responses != requests {
		return benchResult{}, fmt.Errorf("benchmark request/response mismatch: requests=%d responses=%d", requests, responses)
	}
	if counter != uint64(responses)+1 {
		return benchResult{}, fmt.Errorf("benchmark final counter mismatch: counter=%d expected=%d", counter, uint64(responses)+1)
	}

	sort.Slice(latNs, func(i, j int) bool { return latNs[i] < latNs[j] })
	p50 := percentileMicros(latNs, 50)
	p95 := percentileMicros(latNs, 95)
	p99 := percentileMicros(latNs, 99)

	return benchResult{
		durationSec:    durationSec,
		targetRPS:      targetRPS,
		requests:       requests,
		responses:      responses,
		mismatches:     mismatches,
		elapsedSec:     elapsedSec,
		throughputRPS:  throughput,
		p50US:          p50,
		p95US:          p95,
		p99US:          p99,
		clientCPUCores: cpuCores,
	}, nil
}

func printBenchHeader() {
	fmt.Println("mode,duration_sec,target_rps,requests,responses,mismatches,throughput_rps,p50_us,p95_us,p99_us,client_cpu_cores,server_cpu_cores,total_cpu_cores")
}

func printBenchRow(label string, result benchResult, serverCPUCores float64) {
	totalCPUCores := result.clientCPUCores + serverCPUCores
	fmt.Printf(
		"%s,%d,%d,%d,%d,%d,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f\n",
		label,
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
		serverCPUCores,
		totalCPUCores,
	)
}

func udsClientBench(runDir, service string, durationSec, targetRPS int) error {
	result, err := runClientBenchCapture(runDir, service, durationSec, targetRPS)
	if err != nil {
		return err
	}

	printBenchHeader()
	printBenchRow("go-uds", result, 0)
	return nil
}

func udsClientBadHello(runDir, service string) error {
	addr := &net.UnixAddr{Name: endpointSockPath(runDir, service), Net: "unixpacket"}
	conn, err := net.DialUnix("unixpacket", nil, addr)
	if err != nil {
		return err
	}
	defer conn.Close()

	var bad protocol.Frame
	binary.LittleEndian.PutUint32(bad[0:4], 0xdeadbeef)
	binary.LittleEndian.PutUint16(bad[4:6], 999)
	binary.LittleEndian.PutUint16(bad[6:8], 99)

	if err := udsWriteFrame(conn, bad, 10*time.Second); err != nil {
		return err
	}

	fmt.Println("GO-UDS-BADHELLO sent malformed negotiation frame")
	return nil
}

func udsClientRawHello(runDir, service string, supportedMask, preferredMask uint32, authToken uint64) error {
	addr := &net.UnixAddr{Name: endpointSockPath(runDir, service), Net: "unixpacket"}
	conn, err := net.DialUnix("unixpacket", nil, addr)
	if err != nil {
		return err
	}
	defer conn.Close()

	hello := protocol.HelloPayload{
		LayoutVersion:           protocol.MessageVersion,
		Flags:                   0,
		Supported:               supportedMask,
		Preferred:               preferredMask,
		MaxRequestPayloadBytes:  protocol.MaxPayloadDefault,
		MaxRequestBatchItems:    1,
		MaxResponsePayloadBytes: protocol.MaxPayloadDefault,
		MaxResponseBatchItems:   1,
		AuthToken:               authToken,
	}
	if err := udsWriteFrame(conn, encodeHelloNeg(hello), 10*time.Second); err != nil {
		return err
	}

	ackFrame, err := udsReadFrame(conn, 10*time.Second)
	if err != nil {
		return err
	}
	ack, status, err := decodeAckNeg(ackFrame)
	if err != nil {
		return err
	}

	fmt.Printf("GO-UDS-RAWHELLO status=%d intersection=%d selected=%d\n", status, ack.Intersection, ack.Selected)
	if status != negStatusOK {
		return syscall.Errno(status)
	}
	return nil
}

func endpointSockPath(runDir, service string) string {
	return filepath.Join(runDir, service+".sock")
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

func selfCPUSeconds() float64 {
	var ru syscall.Rusage
	if err := syscall.Getrusage(syscall.RUSAGE_SELF, &ru); err != nil {
		return 0
	}

	return float64(ru.Utime.Sec) + float64(ru.Utime.Usec)/1e6 +
		float64(ru.Stime.Sec) + float64(ru.Stime.Usec)/1e6
}

func adaptiveSleepNs(remainingNs int64) int64 {
	if remainingNs > 5_000_000 {
		return remainingNs - 1_000_000
	}
	if remainingNs > 500_000 {
		return remainingNs / 2
	}
	if remainingNs > 50_000 {
		return remainingNs / 4
	}
	return remainingNs
}

func waitForBenchmarkSlot(startNs, endNs int64, targetRPS, requestsSent int) bool {
	if targetRPS <= 0 {
		return time.Now().UnixNano() < endNs
	}

	rate := int64(targetRPS)
	for {
		nowNs := time.Now().UnixNano()
		if nowNs >= endNs {
			return false
		}

		elapsedNs := nowNs - startNs
		targetCompleted := (elapsedNs * rate) / int64(time.Second)
		if int64(requestsSent) <= targetCompleted {
			return true
		}

		targetElapsedNs := (int64(requestsSent) * int64(time.Second)) / rate
		if targetElapsedNs <= elapsedNs {
			return true
		}

		time.Sleep(time.Duration(adaptiveSleepNs(targetElapsedNs - elapsedNs)))
	}
}

func waitForPath(path string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for {
		if _, err := os.Stat(path); err == nil {
			return nil
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("endpoint %s was not created in time", path)
		}
		time.Sleep(time.Millisecond)
	}
}

func udsBench(runDir, service string, durationSec, targetRPS int) error {
	serverStart := time.Now()
	cmd := exec.Command(os.Args[0], "uds-server-loop", runDir, service, "0")
	cmd.Stdout = io.Discard
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		return err
	}

	if err := waitForPath(endpointSockPath(runDir, service), 5*time.Second); err != nil {
		_ = cmd.Process.Kill()
		_, _ = cmd.Process.Wait()
		return err
	}

	result, err := runClientBenchCapture(runDir, service, durationSec, targetRPS)
	if err != nil {
		_ = cmd.Process.Kill()
		_, _ = cmd.Process.Wait()
		return err
	}

	if err := cmd.Wait(); err != nil {
		return err
	}
	if cmd.ProcessState == nil {
		return fmt.Errorf("server benchmark child produced no process state")
	}

	serverCPU := 0.0
	serverElapsedSec := time.Since(serverStart).Seconds()
	if serverElapsedSec > 0 {
		serverCPU = (cmd.ProcessState.UserTime().Seconds() + cmd.ProcessState.SystemTime().Seconds()) / serverElapsedSec
	}

	printBenchHeader()
	printBenchRow("go-uds", result, serverCPU)
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
	case "uds-server-once":
		if len(args) != 4 {
			usage(args[0])
			os.Exit(2)
		}
		err = udsServerOnce(args[2], args[3])
	case "uds-client-once":
		if len(args) != 5 {
			usage(args[0])
			os.Exit(2)
		}
		err = udsClientOnce(args[2], args[3], parseU64(args[4]))
	case "uds-server-loop":
		if len(args) != 5 {
			usage(args[0])
			os.Exit(2)
		}
		err = udsServerLoop(args[2], args[3], parseU64(args[4]))
	case "uds-server-bench":
		if len(args) != 5 {
			usage(args[0])
			os.Exit(2)
		}
		err = udsServerBench(args[2], args[3], parseU64(args[4]))
	case "uds-client-bench":
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
		err = udsClientBench(args[2], args[3], durationSec, targetRPS)
	case "uds-bench":
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
		err = udsBench(args[2], args[3], durationSec, targetRPS)
	case "uds-client-badhello":
		if len(args) != 4 {
			usage(args[0])
			os.Exit(2)
		}
		err = udsClientBadHello(args[2], args[3])
	case "uds-client-rawhello":
		if len(args) != 7 {
			usage(args[0])
			os.Exit(2)
		}
		err = udsClientRawHello(args[2], args[3], parseU32(args[4]), parseU32(args[5]), parseU64(args[6]))
	default:
		usage(args[0])
		os.Exit(2)
	}

	if err != nil {
		fmt.Fprintf(os.Stderr, "netipc-live-go failed: %v\n", err)
		os.Exit(1)
	}
}
