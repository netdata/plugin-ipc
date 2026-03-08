package main

import (
	"encoding/binary"
	"fmt"
	"net"
	"os"
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

	negStatusOK uint32 = 0
)

const (
	negOffMagic        = 0
	negOffVersion      = 4
	negOffType         = 6
	negOffSupported    = 8
	negOffPreferred    = 12
	negOffIntersection = 16
	negOffSelected     = 20
	negOffAuthToken    = 24
	negOffStatus       = 32
)

type negMessage struct {
	typ          uint16
	supported    uint32
	preferred    uint32
	intersection uint32
	selected     uint32
	authToken    uint64
	status       uint32
}

func usage(argv0 string) {
	fmt.Fprintf(os.Stderr, "usage:\n")
	fmt.Fprintf(os.Stderr, "  %s uds-server-once <run_dir> <service>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s uds-client-once <run_dir> <service> <value>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s uds-server-loop <run_dir> <service> <max_requests|0>\n", argv0)
	fmt.Fprintf(os.Stderr, "  %s uds-client-bench <run_dir> <service> <duration_sec> <target_rps>\n", argv0)
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

func protocolError(message string) error {
	return fmt.Errorf("protocol error: %s", message)
}

func encodeNeg(msg negMessage) protocol.Frame {
	var frame protocol.Frame
	binary.LittleEndian.PutUint32(frame[negOffMagic:negOffMagic+4], negMagic)
	binary.LittleEndian.PutUint16(frame[negOffVersion:negOffVersion+2], negVersion)
	binary.LittleEndian.PutUint16(frame[negOffType:negOffType+2], msg.typ)
	binary.LittleEndian.PutUint32(frame[negOffSupported:negOffSupported+4], msg.supported)
	binary.LittleEndian.PutUint32(frame[negOffPreferred:negOffPreferred+4], msg.preferred)
	binary.LittleEndian.PutUint32(frame[negOffIntersection:negOffIntersection+4], msg.intersection)
	binary.LittleEndian.PutUint32(frame[negOffSelected:negOffSelected+4], msg.selected)
	binary.LittleEndian.PutUint64(frame[negOffAuthToken:negOffAuthToken+8], msg.authToken)
	binary.LittleEndian.PutUint32(frame[negOffStatus:negOffStatus+4], msg.status)
	return frame
}

func decodeNeg(frame protocol.Frame, expectedTyp uint16) (negMessage, error) {
	magic := binary.LittleEndian.Uint32(frame[negOffMagic : negOffMagic+4])
	version := binary.LittleEndian.Uint16(frame[negOffVersion : negOffVersion+2])
	typ := binary.LittleEndian.Uint16(frame[negOffType : negOffType+2])
	if magic != negMagic || version != negVersion || typ != expectedTyp {
		return negMessage{}, syscall.EPROTO
	}

	return negMessage{
		typ:          typ,
		supported:    binary.LittleEndian.Uint32(frame[negOffSupported : negOffSupported+4]),
		preferred:    binary.LittleEndian.Uint32(frame[negOffPreferred : negOffPreferred+4]),
		intersection: binary.LittleEndian.Uint32(frame[negOffIntersection : negOffIntersection+4]),
		selected:     binary.LittleEndian.Uint32(frame[negOffSelected : negOffSelected+4]),
		authToken:    binary.LittleEndian.Uint64(frame[negOffAuthToken : negOffAuthToken+8]),
		status:       binary.LittleEndian.Uint32(frame[negOffStatus : negOffStatus+4]),
	}, nil
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
	return posix.NewConfig(runDir, service)
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
	server, err := posix.Listen(udsConfig(runDir, service))
	if err != nil {
		return err
	}
	defer server.Close()

	if err := server.Accept(10 * time.Second); err != nil {
		return err
	}

	handled := uint64(0)
	for maxRequests == 0 || handled < maxRequests {
		requestID, request, err := server.ReceiveIncrement(0)
		if err != nil {
			return err
		}
		response := protocol.IncrementResponse{
			Status: protocol.StatusOK,
			Value:  request.Value + 1,
		}
		if err := server.SendIncrement(requestID, response, 0); err != nil {
			return err
		}
		handled++
	}

	return nil
}

func udsClientBench(runDir, service string, durationSec, targetRPS int) error {
	if durationSec <= 0 {
		return fmt.Errorf("duration_sec must be > 0")
	}
	if targetRPS < 0 {
		return fmt.Errorf("target_rps must be >= 0")
	}

	client, err := posix.Dial(udsConfig(runDir, service), 10*time.Second)
	if err != nil {
		return err
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
	cpuCores := (selfCPUSeconds() - cpuStart) / elapsedSec
	throughput := float64(responses) / elapsedSec

	sort.Slice(latNs, func(i, j int) bool { return latNs[i] < latNs[j] })
	p50 := percentileMicros(latNs, 50)
	p95 := percentileMicros(latNs, 95)
	p99 := percentileMicros(latNs, 99)

	fmt.Println("mode,duration_sec,target_rps,requests,responses,mismatches,throughput_rps,p50_us,p95_us,p99_us,client_cpu_cores")
	fmt.Printf(
		"go-uds,%d,%d,%d,%d,%d,%.2f,%.2f,%.2f,%.2f,%.3f\n",
		durationSec,
		targetRPS,
		requests,
		responses,
		mismatches,
		throughput,
		p50,
		p95,
		p99,
		cpuCores,
	)

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

	hello := negMessage{
		typ:          negHello,
		supported:    supportedMask,
		preferred:    preferredMask,
		intersection: 0,
		selected:     0,
		authToken:    authToken,
		status:       negStatusOK,
	}
	if err := udsWriteFrame(conn, encodeNeg(hello), 10*time.Second); err != nil {
		return err
	}

	ackFrame, err := udsReadFrame(conn, 10*time.Second)
	if err != nil {
		return err
	}
	ack, err := decodeNeg(ackFrame, negAck)
	if err != nil {
		return err
	}

	fmt.Printf("GO-UDS-RAWHELLO status=%d intersection=%d selected=%d\n", ack.status, ack.intersection, ack.selected)
	if ack.status != negStatusOK {
		return syscall.Errno(ack.status)
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
