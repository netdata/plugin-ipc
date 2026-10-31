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
	doneCh := runServerForTest(s)
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

// servedOnce connects once and completes one increment call. Callers make
// sure a worker is free first, so any failure is a real one.
func servedOnce(service string, cfg posix.ClientConfig) error {
	c := NewIncrementClient(testRunDir, service, cfg)
	defer c.Close()
	c.Refresh()
	if !c.Ready() {
		return fmt.Errorf("state %v", c.Status().State)
	}
	v, err := c.CallIncrementWithTimeout(1, 1000)
	if err != nil || v != 2 {
		return fmt.Errorf("call: v=%d err=%v", v, err)
	}
	return nil
}

func shmFiles(service string) []string {
	matches, _ := filepath.Glob(filepath.Join(testRunDir, service+"-*.ipcshm"))
	return matches
}

// blockingIncrementDispatch parks every request; the helper process holding
// it is killed by the test.
func blockingIncrementDispatch(entered func()) DispatchHandler {
	return IncrementDispatch(func(v uint64) (uint64, bool) {
		entered()
		select {}
	})
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

// helperEntered reports, per helper server process, that its handler holds a
// request.
var helperEntered = map[*exec.Cmd]chan struct{}{}

// spawnHelper re-executes the test binary as a client or server process.
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
	entered := make(chan struct{}, 1)
	go func() {
		sc := bufio.NewScanner(stdout)
		for sc.Scan() {
			if strings.Contains(sc.Text(), "HELPER_READY") {
				ready <- true
			}
			if strings.Contains(sc.Text(), "HELPER_HANDLER_ENTERED") {
				entered <- struct{}{}
			}
		}
		close(ready)
	}()
	helperEntered[cmd] = entered
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
		// The server listens before the helper starts: one connect.
		c := NewIncrementClient(testRunDir, service, testUnixShmClientConfig())
		c.Refresh()
		if v, err := c.CallIncrementWithTimeout(1, 2000); err != nil || v != 2 {
			fmt.Printf("HELPER_FAILED v=%d err=%v\n", v, err)
			os.Exit(1)
		}
		fmt.Println("HELPER_READY")
		select {} // until the test kills this process
	case "shm_server", "uds_server":
		cfg := testServerConfig()
		if mode == "shm_server" {
			cfg = testUnixShmServerConfig()
		}
		startRobustServer(service, cfg, protocol.MethodIncrement,
			blockingIncrementDispatch(func() { fmt.Println("HELPER_HANDLER_ENTERED") }), 2)
		fmt.Println("HELPER_READY")
		select {} // until the test kills this process
	default:
		t.Fatalf("unknown helper mode %q", mode)
	}
}

// K1: SHM server must release a session once its client leaves.

func TestRobustShmSessionsReleasedAfterCleanClose(t *testing.T) {
	svc := robustService("go_rob_shm_clean")
	rs := startRobustServer(svc, testUnixShmServerConfig(), protocol.MethodIncrement, robustIncrementDispatch(0), 2)
	defer cleanupAll(svc)

	// Each worker must exit and remove its region once its client closes;
	// a leaked session fails waitSessionExits loudly.
	var failures []string
	for i := 0; i < 5; i++ {
		if err := servedOnce(svc, testUnixShmClientConfig()); err != nil {
			failures = append(failures, fmt.Sprintf("client %d: %v", i, err))
		}
		waitSessionExits(rs.server, 1)
	}
	leftover := shmFiles(svc)
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
	// Both workers must notice their crashed clients and free the slots.
	waitSessionExits(rs.server, 2)
	served := servedOnce(svc, testUnixShmClientConfig())
	waitSessionExits(rs.server, 1)
	leftover := shmFiles(svc)
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
	waitHandshakesStarted(rs.server, 1)
	var served error = fmt.Errorf("did not finish")
	finished := within(5*time.Second, func() {
		served = servedOnce(svc, testClientConfig())
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
	waitHandshakesStarted(rs.server, 1)
	stopped := rs.stopWithin(3 * time.Second)

	syscall.Close(silent)
	<-rs.doneCh
	if !stopped {
		t.Error("server Run must return within 3s of Stop while a silent client is connected")
	}
}

// A silent client holds a worker only until the server handshake deadline.
func TestRobustSilentClientReleasedAtHandshakeDeadline(t *testing.T) {
	svc := robustService("go_rob_silent_deadline")
	rs := startRobustServer(svc, testServerConfig(), protocol.MethodIncrement, robustIncrementDispatch(0), 1)
	defer cleanupAll(svc)

	silent := silentConnect(t, svc)
	defer syscall.Close(silent)
	start := time.Now()
	waitHandshakesStarted(rs.server, 1)
	waitSessionExits(rs.server, 1)
	elapsed := time.Since(start)
	served := servedOnce(svc, testClientConfig())
	deadline := time.Duration(rs.server.timings.handshakeMs) * time.Millisecond
	if elapsed < deadline {
		t.Errorf("silent client released after %v, before the %v handshake deadline", elapsed, deadline)
	}

	var buf [1]byte
	n, _, rerr := syscall.Recvfrom(silent, buf[:], syscall.MSG_DONTWAIT)
	rs.stopWithin(5 * time.Second)
	if served != nil {
		t.Errorf("the only worker must be freed once the silent client misses the handshake deadline: %v (after %v)", served, elapsed)
	}
	if n != 0 || rerr != nil {
		t.Errorf("server must close the silent connection at the deadline (recv n=%d err=%v)", n, rerr)
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
	// After the handler returns, the worker can only be sending the 4 MiB
	// response, which blocks because the client never reads.
	handled := make(chan struct{}, 1)
	reverse := pingPongStringReverseDispatch()
	rs := startRobustServer(svc, scfg, protocol.MethodStringReverse, func(req, resp []byte) (int, error) {
		n, err := reverse(req, resp)
		handled <- struct{}{}
		return n, err
	}, 2)
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
	waitClosed(handled, "string reverse handler")

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
	c.Refresh()
	if !c.Ready() {
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		t.Fatalf("client not ready against the helper server: %+v", c.Status())
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
	waitClosed(helperEntered[cmd], "helper server handler") // request parked in the stalled handler
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
	holder.Refresh()
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
	waitSessionExits(rs.server, 1)
	refused.Refresh()
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
