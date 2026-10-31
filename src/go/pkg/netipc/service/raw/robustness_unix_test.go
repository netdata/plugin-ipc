//go:build linux

package raw

// Robustness reproduction tests (SOW-0040).
//
// Each test asserts the correct behavior and runs every blocking step in a
// goroutine under a watchdog, so a regression fails instead of hanging.
// Bounds are deliberately lenient so they do not dictate a fix design.

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix"
)

const robustHelperEnv = "NETIPC_ROBUSTNESS_HELPER"

var robustCounter int

func robustService(prefix string) string {
	robustCounter++
	return fmt.Sprintf("%s_%d_%d", prefix, os.Getpid(), robustCounter)
}

// within runs f in a goroutine and reports whether it finished in time.
// On timeout the goroutine keeps running; callers release what blocks it.
func within(limit time.Duration, f func()) bool {
	done := make(chan struct{})
	go func() {
		defer close(done)
		f()
	}()
	select {
	case <-done:
		return true
	case <-time.After(limit):
		return false
	}
}

func robustIncrementDispatch(delay time.Duration) DispatchHandler {
	return IncrementDispatch(func(v uint64) (uint64, bool) {
		time.Sleep(delay)
		return v + 1, true
	})
}

type robustServer struct {
	server *Server
	doneCh chan struct{}
}

func startRobustServer(service string, cfg posix.ServerConfig, method uint16, handler DispatchHandler, workers int) *robustServer {
	ensureRunDir()
	cleanupAll(service)
	s := NewServerWithWorkers(testRunDir, service, cfg, method, handler, workers)
	doneCh := make(chan struct{})
	go func() {
		defer close(doneCh)
		_ = s.Run()
	}()
	waitUnixSocketReady(service)
	return &robustServer{server: s, doneCh: doneCh}
}

// stopWithin stops the server and reports whether Run returned in time.
func (rs *robustServer) stopWithin(limit time.Duration) bool {
	rs.server.Stop()
	select {
	case <-rs.doneCh:
		return true
	case <-time.After(limit):
		return false
	}
}

// servedWithin connects and completes one increment call, retrying until limit.
func servedWithin(service string, cfg posix.ClientConfig, limit time.Duration) error {
	deadline := time.Now().Add(limit)
	last := fmt.Errorf("never connected")
	for time.Now().Before(deadline) {
		c := NewIncrementClient(testRunDir, service, cfg)
		c.Refresh()
		if c.Ready() {
			v, err := c.CallIncrementWithTimeout(1, 1000)
			c.Close()
			if err == nil && v == 2 {
				return nil
			}
			last = fmt.Errorf("call: v=%d err=%v", v, err)
		} else {
			last = fmt.Errorf("state %v", c.Status().State)
			c.Close()
		}
		time.Sleep(50 * time.Millisecond)
	}
	return last
}

func shmFiles(service string) []string {
	matches, _ := filepath.Glob(filepath.Join(testRunDir, service+"-*.ipcshm"))
	return matches
}

func waitNoShmFiles(service string, limit time.Duration) []string {
	deadline := time.Now().Add(limit)
	for {
		files := shmFiles(service)
		if len(files) == 0 || time.Now().After(deadline) {
			return files
		}
		time.Sleep(50 * time.Millisecond)
	}
}

// silentConnect opens a SOCK_SEQPACKET connection without sending HELLO.
func silentConnect(t *testing.T, service string) int {
	t.Helper()
	fd, err := syscall.Socket(syscall.AF_UNIX, syscall.SOCK_SEQPACKET, 0)
	if err != nil {
		t.Fatalf("socket: %v", err)
	}
	addr := &syscall.SockaddrUnix{Name: testRunDir + "/" + service + ".sock"}
	if err := syscall.Connect(fd, addr); err != nil {
		syscall.Close(fd)
		t.Fatalf("connect: %v", err)
	}
	return fd
}

// rawListener binds and listens on the service socket but never accepts.
func rawListener(t *testing.T, service string) int {
	t.Helper()
	ensureRunDir()
	cleanupAll(service)
	fd, err := syscall.Socket(syscall.AF_UNIX, syscall.SOCK_SEQPACKET, 0)
	if err != nil {
		t.Fatalf("socket: %v", err)
	}
	if err := syscall.Bind(fd, &syscall.SockaddrUnix{Name: testRunDir + "/" + service + ".sock"}); err != nil {
		t.Fatalf("bind: %v", err)
	}
	if err := syscall.Listen(fd, 4); err != nil {
		t.Fatalf("listen: %v", err)
	}
	return fd
}

// spawnHelper re-executes the test binary as a client process.
func spawnHelper(t *testing.T, mode, service string) *exec.Cmd {
	t.Helper()
	cmd := exec.Command(os.Args[0], "-test.run=^TestRobustnessHelperProcess$", "-test.v")
	cmd.Env = append(os.Environ(), robustHelperEnv+"="+mode+":"+service)
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatalf("stdout pipe: %v", err)
	}
	if err := cmd.Start(); err != nil {
		t.Fatalf("start helper: %v", err)
	}
	ready := make(chan bool, 1)
	go func() {
		sc := bufio.NewScanner(stdout)
		for sc.Scan() {
			if strings.Contains(sc.Text(), "HELPER_READY") {
				ready <- true
				return
			}
		}
		ready <- false
	}()
	select {
	case ok := <-ready:
		if !ok {
			t.Fatalf("helper %s exited before ready", mode)
		}
	case <-time.After(10 * time.Second):
		_ = cmd.Process.Kill()
		t.Fatalf("helper %s did not become ready", mode)
	}
	return cmd
}

// TestRobustnessHelperProcess is the child-process entry point; inert unless
// spawned by spawnHelper.
func TestRobustnessHelperProcess(t *testing.T) {
	spec := os.Getenv(robustHelperEnv)
	if spec == "" {
		return
	}
	mode, service, _ := strings.Cut(spec, ":")
	switch mode {
	case "shm_client":
		c := NewIncrementClient(testRunDir, service, testUnixShmClientConfig())
		for i := 0; i < 200 && !c.Ready(); i++ {
			c.Refresh()
			time.Sleep(10 * time.Millisecond)
		}
		if v, err := c.CallIncrementWithTimeout(1, 2000); err != nil || v != 2 {
			fmt.Printf("HELPER_FAILED v=%d err=%v\n", v, err)
			os.Exit(1)
		}
		fmt.Println("HELPER_READY")
		time.Sleep(600 * time.Second)
	case "shm_server", "uds_server":
		cfg := testServerConfig()
		if mode == "shm_server" {
			cfg = testUnixShmServerConfig()
		}
		startRobustServer(service, cfg, protocol.MethodIncrement, robustIncrementDispatch(60*time.Second), 2)
		fmt.Println("HELPER_READY")
		time.Sleep(600 * time.Second)
	default:
		t.Fatalf("unknown helper mode %q", mode)
	}
}

// K1: SHM server must release a session once its client leaves.

func TestRobustShmSessionsReleasedAfterCleanClose(t *testing.T) {
	svc := robustService("go_rob_shm_clean")
	rs := startRobustServer(svc, testUnixShmServerConfig(), protocol.MethodIncrement, robustIncrementDispatch(0), 2)
	defer cleanupAll(svc)

	var failures []string
	for i := 0; i < 5; i++ {
		if err := servedWithin(svc, testUnixShmClientConfig(), 3*time.Second); err != nil {
			failures = append(failures, fmt.Sprintf("client %d: %v", i, err))
		}
	}
	leftover := waitNoShmFiles(svc, 2*time.Second)
	rs.stopWithin(5 * time.Second)

	if len(failures) > 0 {
		t.Errorf("with 2 workers, sequential SHM clients must all be served: %v", failures)
	}
	if len(leftover) > 0 {
		t.Errorf("SHM regions leaked: %v", leftover)
	}
}

func TestRobustShmSessionsReleasedAfterClientCrash(t *testing.T) {
	svc := robustService("go_rob_shm_crash")
	rs := startRobustServer(svc, testUnixShmServerConfig(), protocol.MethodIncrement, robustIncrementDispatch(0), 2)
	defer cleanupAll(svc)

	for i := 0; i < 2; i++ {
		cmd := spawnHelper(t, "shm_client", svc)
		_ = cmd.Process.Kill() // SIGKILL
		_ = cmd.Wait()
	}
	served := servedWithin(svc, testUnixShmClientConfig(), 3*time.Second)
	leftover := waitNoShmFiles(svc, 2*time.Second)
	rs.stopWithin(5 * time.Second)

	if served != nil {
		t.Errorf("after 2 crashed SHM clients on 2 workers, a new client must be served: %v", served)
	}
	if len(leftover) > 0 {
		t.Errorf("SHM regions leaked: %v", leftover)
	}
}

// K3: a client that connects but never sends HELLO must not block others or
// shutdown.

func TestRobustSilentClientDoesNotBlockOtherClients(t *testing.T) {
	svc := robustService("go_rob_silent")
	rs := startRobustServer(svc, testServerConfig(), protocol.MethodIncrement, robustIncrementDispatch(0), 4)
	defer cleanupAll(svc)

	silent := silentConnect(t, svc)
	time.Sleep(100 * time.Millisecond)
	var served error = fmt.Errorf("did not finish")
	finished := within(5*time.Second, func() {
		served = servedWithin(svc, testClientConfig(), 5*time.Second)
	})

	syscall.Close(silent)
	rs.stopWithin(5 * time.Second)
	if !finished || served != nil {
		t.Errorf("a legitimate client must be served within 5s while a silent client is connected (finished=%v err=%v)", finished, served)
	}
}

func TestRobustSilentClientDoesNotBlockShutdown(t *testing.T) {
	svc := robustService("go_rob_silent_stop")
	rs := startRobustServer(svc, testServerConfig(), protocol.MethodIncrement, robustIncrementDispatch(0), 4)
	defer cleanupAll(svc)

	silent := silentConnect(t, svc)
	time.Sleep(100 * time.Millisecond)
	stopped := rs.stopWithin(3 * time.Second)

	syscall.Close(silent)
	<-rs.doneCh
	if !stopped {
		t.Error("server Run must return within 3s of Stop while a silent client is connected")
	}
}

// K4: a client call must be bounded even when the server never answers HELLO.

func TestRobustClientBoundedWhenServerNeverAccepts(t *testing.T) {
	svc := robustService("go_rob_noaccept")
	listener := rawListener(t, svc)
	defer cleanupAll(svc)

	done := make(chan struct{})
	start := time.Now()
	finished := within(6*time.Second, func() {
		defer close(done)
		c := NewIncrementClient(testRunDir, svc, testClientConfig())
		c.SetCallTimeout(1000)
		c.Refresh()
		_, _ = c.CallIncrementWithTimeout(1, 1000)
		c.Close()
	})
	elapsed := time.Since(start)

	syscall.Close(listener)
	<-done
	if !finished {
		t.Errorf("Refresh + call with a 1s timeout must return within 6s against a server that never accepts (blocked %v)", elapsed)
	}
}

func TestRobustClientBoundedWhenServerNeverSendsHelloAck(t *testing.T) {
	svc := robustService("go_rob_nohelloack")
	listener := rawListener(t, svc)
	defer cleanupAll(svc)

	release := make(chan struct{})
	go func() {
		fd, _, err := syscall.Accept(listener)
		<-release
		if err == nil {
			syscall.Close(fd)
		}
		syscall.Close(listener)
	}()

	done := make(chan struct{})
	finished := within(6*time.Second, func() {
		defer close(done)
		c := NewIncrementClient(testRunDir, svc, testClientConfig())
		c.SetCallTimeout(1000)
		c.Refresh()
		_, _ = c.CallIncrementWithTimeout(1, 1000)
		c.Close()
	})

	close(release)
	<-done
	if !finished {
		t.Error("Refresh + call with a 1s timeout must return within 6s when the server accepts but never sends HELLO_ACK")
	}
}

// N1 guard: the configured call timeout must be honored on the UDS path.

func TestRobustUdsCallHonorsTimeoutLongerThan100ms(t *testing.T) {
	svc := robustService("go_rob_slow_handler")
	rs := startRobustServer(svc, testServerConfig(), protocol.MethodIncrement, robustIncrementDispatch(300*time.Millisecond), 2)
	defer cleanupAll(svc)
	defer rs.stopWithin(5 * time.Second)

	c := NewIncrementClient(testRunDir, svc, testClientConfig())
	defer c.Close()
	c.Refresh()
	start := time.Now()
	v, err := c.CallIncrementWithTimeout(41, 2000)
	if err != nil || v != 42 {
		t.Errorf("a 300ms handler must succeed with a 2000ms timeout: v=%d err=%v after %v", v, err, time.Since(start))
	}
}

// N2: shutdown must not wait forever on a worker blocked sending a response
// to a client that stopped reading.

func TestRobustShutdownNotBlockedByNonReadingClient(t *testing.T) {
	const big = 8 * 1024 * 1024
	svc := robustService("go_rob_noread")
	scfg := testServerConfig()
	scfg.MaxRequestPayloadBytes = big
	scfg.MaxResponsePayloadBytes = big
	scfg.PacketSize = 65536 // isolate from the default packet size issue (N11)
	rs := startRobustServer(svc, scfg, protocol.MethodStringReverse, pingPongStringReverseDispatch(), 2)
	defer cleanupAll(svc)

	ccfg := testClientConfig()
	ccfg.MaxRequestPayloadBytes = big
	ccfg.MaxResponsePayloadBytes = big
	ccfg.PacketSize = 65536
	session, err := posix.Connect(testRunDir, svc, &ccfg)
	if err != nil {
		t.Fatalf("connect: %v", err)
	}
	text := strings.Repeat("x", 4*1024*1024)
	payload := make([]byte, protocol.StringReverseHdrSize+len(text)+1)
	n := protocol.StringReverseEncode(text, payload)
	hdr := protocol.Header{Kind: protocol.KindRequest, Code: protocol.MethodStringReverse, ItemCount: 1, MessageID: 1}
	if err := session.Send(&hdr, payload[:n]); err != nil {
		t.Fatalf("send: %v", err)
	}
	// Never read the response; give the worker time to block in send.
	time.Sleep(500 * time.Millisecond)

	stopped := rs.stopWithin(3 * time.Second)
	session.Close()
	<-rs.doneCh
	if !stopped {
		t.Error("server Run must return within 3s of Stop while a worker is blocked sending")
	}
}

// N3: Stop issued before Run must not be lost.

func TestRobustStopBeforeRunIsHonored(t *testing.T) {
	svc := robustService("go_rob_stop_first")
	ensureRunDir()
	cleanupAll(svc)
	defer cleanupAll(svc)

	s := NewServerWithWorkers(testRunDir, svc, testServerConfig(), protocol.MethodIncrement, robustIncrementDispatch(0), 2)
	s.Stop()
	doneCh := make(chan struct{})
	go func() {
		defer close(doneCh)
		_ = s.Run()
	}()

	select {
	case <-doneCh:
	case <-time.After(2 * time.Second):
		t.Error("Run after Stop must return promptly instead of serving forever")
		waitUnixSocketReady(svc)
		s.Stop()
		<-doneCh
	}
}

// N11: with the auto-detected packet size, a message larger than SO_SNDBUF
// must still be delivered (chunked).

func TestRobustDefaultPacketSizeCarriesLargeMessage(t *testing.T) {
	if err := robustLargeRoundTrip(0); err != nil {
		t.Errorf("a 512 KiB request must round-trip with default packet size: %v", err)
	}
}

// Control: the same message with an explicit 64 KiB packet size passes,
// isolating the failure above to the auto-detected packet size.
func TestRobustExplicitPacketSizeCarriesLargeMessage(t *testing.T) {
	if err := robustLargeRoundTrip(65536); err != nil {
		t.Errorf("a 512 KiB request must round-trip with a 64 KiB packet size: %v", err)
	}
}

func robustLargeRoundTrip(packetSize uint32) error {
	const big = 1024 * 1024
	svc := robustService("go_rob_bigmsg")
	scfg := testServerConfig()
	scfg.MaxRequestPayloadBytes = big
	scfg.MaxResponsePayloadBytes = big
	scfg.PacketSize = packetSize
	rs := startRobustServer(svc, scfg, protocol.MethodStringReverse, pingPongStringReverseDispatch(), 2)
	defer cleanupAll(svc)
	defer rs.stopWithin(5 * time.Second)

	ccfg := testClientConfig()
	ccfg.MaxRequestPayloadBytes = big
	ccfg.MaxResponsePayloadBytes = big
	ccfg.PacketSize = packetSize
	c := NewStringReverseClient(testRunDir, svc, ccfg)
	defer c.Close()
	c.Refresh()
	text := strings.Repeat("y", 512*1024)
	view, err := c.CallStringReverseWithTimeout(text, 5000)
	if err != nil {
		return err
	}
	if view == nil || len(view.Str) != len(text) {
		return fmt.Errorf("unexpected response length")
	}
	return nil
}

// N5: an SHM client must notice a crashed server instead of waiting out its
// whole timeout.

func TestRobustShmClientNoticesCrashedServer(t *testing.T) {
	clientNoticesCrashedServer(t, true)
}

func TestRobustUdsClientReportsCrashedServerAsDisconnected(t *testing.T) {
	clientNoticesCrashedServer(t, false)
}

func clientNoticesCrashedServer(t *testing.T, shm bool) {
	name, mode, ccfg := "go_rob_dead_uds_server", "uds_server", testClientConfig()
	if shm {
		name, mode, ccfg = "go_rob_dead_server", "shm_server", testUnixShmClientConfig()
	}
	svc := robustService(name)
	ensureRunDir()
	cleanupAll(svc)
	defer cleanupAll(svc)
	cmd := spawnHelper(t, mode, svc)

	c := NewIncrementClient(testRunDir, svc, ccfg)
	defer c.Close()
	for i := 0; i < 200 && !c.Ready(); i++ {
		c.Refresh()
		time.Sleep(10 * time.Millisecond)
	}

	type outcome struct {
		err     error
		elapsed time.Duration
	}
	result := make(chan outcome, 1)
	go func() {
		start := time.Now()
		_, err := c.CallIncrementWithTimeout(1, 10000)
		result <- outcome{err, time.Since(start)}
	}()
	time.Sleep(500 * time.Millisecond) // request parked in the stalled handler
	_ = cmd.Process.Kill()
	_ = cmd.Wait()

	r := <-result
	if r.err == nil || r.elapsed > 3500*time.Millisecond {
		t.Errorf("call must fail within 3s of the server crash (timeout is 10s): err=%v after %v", r.err, r.elapsed)
	}
	if !errors.Is(r.err, protocol.ErrDisconnected) {
		t.Errorf("call must report the server as disconnected: err=%v", r.err)
	}
}

// A server at worker capacity completes the handshake, then closes the
// session and removes the prepared SHM region. The client must not spend its
// attach retry budget (5 s) on a region that will never appear, and must keep
// SHM enabled for its next connect.
func TestRobustShmAttachStopsWhenServerClosesSession(t *testing.T) {
	svc := robustService("go_rob_attach_refused")
	rs := startRobustServer(svc, testUnixShmServerConfig(), protocol.MethodIncrement, robustIncrementDispatch(0), 1)
	defer cleanupAll(svc)

	holder := NewIncrementClient(testRunDir, svc, testUnixShmClientConfig())
	for i := 0; i < 200 && !holder.Ready(); i++ {
		holder.Refresh()
		time.Sleep(10 * time.Millisecond)
	}
	if !holder.Ready() {
		holder.Close()
		rs.stopWithin(5 * time.Second)
		t.Fatal("first client must hold the only worker")
	}

	refused := NewIncrementClient(testRunDir, svc, testUnixShmClientConfig())
	defer refused.Close()
	start := time.Now()
	refused.Refresh()
	elapsed := time.Since(start)

	holder.Close()
	for i := 0; i < 300 && !refused.Ready(); i++ {
		refused.Refresh()
		if !refused.Ready() {
			time.Sleep(10 * time.Millisecond)
		}
	}
	ready, usesShm := refused.Ready(), refused.shm != nil
	refused.Close()
	rs.stopWithin(5 * time.Second)

	if elapsed >= time.Second {
		t.Errorf("refused connect must return within 1s (attach retry budget is 5s): %v", elapsed)
	}
	if !ready {
		t.Error("refused client must connect once the worker is free")
	}
	if !usesShm {
		t.Error("refused client must still use SHM")
	}
}
