//go:build windows

package raw

// Robustness reproduction tests (SOW-0040), Windows.
//
// Each test asserts the correct behavior and runs every blocking step in a
// goroutine under a watchdog, so a regression fails instead of hanging.
// Bounds are deliberately lenient so they do not dictate a fix design.

import (
	"bufio"
	"fmt"
	"os"
	"os/exec"
	"strings"
	"syscall"
	"testing"
	"time"
	"unsafe"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	windows "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"
)

const (
	winRobustHelperEnv           = "NETIPC_ROBUSTNESS_HELPER"
	winFileFlagFirstPipeInstance = 0x00080000
)

func winWithin(limit time.Duration, f func()) bool {
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

func winSlowIncrementDispatch(delay time.Duration) DispatchHandler {
	return IncrementDispatch(func(v uint64) (uint64, bool) {
		time.Sleep(delay)
		return v + 1, true
	})
}

type winRobustServer struct {
	server *Server
	doneCh chan struct{}
}

func startWinRobustServer(service string, cfg windows.ServerConfig, method uint16, handler DispatchHandler, workers int) *winRobustServer {
	s := NewServerWithWorkers(winTestRunDir, service, cfg, method, handler, workers)
	doneCh := make(chan struct{})
	go func() {
		defer close(doneCh)
		_ = s.Run()
	}()
	time.Sleep(200 * time.Millisecond)
	return &winRobustServer{server: s, doneCh: doneCh}
}

// stopWithin stops the server and reports whether Run returned in time.
func (rs *winRobustServer) stopWithin(limit time.Duration) bool {
	stopped := make(chan struct{})
	go func() {
		rs.server.Stop()
		close(stopped)
	}()
	select {
	case <-rs.doneCh:
		return true
	case <-time.After(limit):
		return false
	}
}

func winServedWithin(service string, cfg windows.ClientConfig, limit time.Duration) error {
	deadline := time.Now().Add(limit)
	last := fmt.Errorf("never connected")
	for time.Now().Before(deadline) {
		c := NewIncrementClient(winTestRunDir, service, cfg)
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

func winOpenPipeSilently(t *testing.T, service string) syscall.Handle {
	t.Helper()
	name, err := windows.BuildPipeName(winTestRunDir, service)
	if err != nil {
		t.Fatalf("pipe name: %v", err)
	}
	for i := 0; i < 100; i++ {
		h, err := syscall.CreateFile(&name[0], syscall.GENERIC_READ|syscall.GENERIC_WRITE, 0, nil, syscall.OPEN_EXISTING, 0, 0)
		if err == nil {
			return h
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("could not open pipe for %s", service)
	return syscall.InvalidHandle
}

func winSpawnHelper(t *testing.T, mode, service string) *exec.Cmd {
	t.Helper()
	cmd := exec.Command(os.Args[0], "-test.run=^TestWinRobustnessHelperProcess$", "-test.v")
	cmd.Env = append(os.Environ(), winRobustHelperEnv+"="+mode+":"+service)
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
	case <-time.After(15 * time.Second):
		_ = cmd.Process.Kill()
		t.Fatalf("helper %s did not become ready", mode)
	}
	return cmd
}

// TestWinRobustnessHelperProcess is the child-process entry point; inert
// unless spawned by winSpawnHelper.
func TestWinRobustnessHelperProcess(t *testing.T) {
	spec := os.Getenv(winRobustHelperEnv)
	if spec == "" {
		return
	}
	mode, service, _ := strings.Cut(spec, ":")
	switch mode {
	case "shm_client":
		c := NewIncrementClient(winTestRunDir, service, testWinShmClientConfig())
		for i := 0; i < 200 && !c.Ready(); i++ {
			c.Refresh()
			time.Sleep(10 * time.Millisecond)
		}
		if v, err := c.CallIncrementWithTimeout(1, 2000); err != nil || v != 2 {
			fmt.Printf("HELPER_FAILED v=%d err=%v\n", v, err)
			os.Exit(1)
		}
	case "shm_server":
		startWinRobustServer(service, testWinShmServerConfig(), protocol.MethodIncrement, winSlowIncrementDispatch(60*time.Second), 2)
	default:
		t.Fatalf("unknown helper mode %q", mode)
	}
	fmt.Println("HELPER_READY")
	time.Sleep(600 * time.Second)
}

// K2: SHM server must release a session once its client crashes.
func TestWinRobustShmSessionReleasedAfterClientCrash(t *testing.T) {
	svc := uniqueWinService("go_wrob_shm_crash")
	rs := startWinRobustServer(svc, testWinShmServerConfig(), protocol.MethodIncrement, winSlowIncrementDispatch(0), 2)

	for i := 0; i < 2; i++ {
		cmd := winSpawnHelper(t, "shm_client", svc)
		_ = cmd.Process.Kill() // TerminateProcess
		_ = cmd.Wait()
	}
	served := winServedWithin(svc, testWinShmClientConfig(), 8*time.Second)
	rs.stopWithin(5 * time.Second)
	if served != nil {
		t.Errorf("after 2 crashed SHM clients on 2 workers, a new client must be served: %v", served)
	}
}

// K3: a client that opens the pipe but never sends HELLO.
func TestWinRobustSilentClientDoesNotBlockOtherClients(t *testing.T) {
	svc := uniqueWinService("go_wrob_silent")
	rs := startWinRobustServer(svc, testWinServerConfig(), protocol.MethodIncrement, winSlowIncrementDispatch(0), 4)

	silent := winOpenPipeSilently(t, svc)
	time.Sleep(100 * time.Millisecond)
	var served error = fmt.Errorf("did not finish")
	finished := winWithin(5*time.Second, func() {
		served = winServedWithin(svc, testWinClientConfig(), 5*time.Second)
	})

	syscall.CloseHandle(silent)
	rs.stopWithin(5 * time.Second)
	if !finished || served != nil {
		t.Errorf("a legitimate client must be served within 5s while a silent client is connected (finished=%v err=%v)", finished, served)
	}
}

func TestWinRobustSilentClientDoesNotBlockShutdown(t *testing.T) {
	svc := uniqueWinService("go_wrob_silent_stop")
	rs := startWinRobustServer(svc, testWinServerConfig(), protocol.MethodIncrement, winSlowIncrementDispatch(0), 4)

	silent := winOpenPipeSilently(t, svc)
	time.Sleep(100 * time.Millisecond)
	stopped := rs.stopWithin(3 * time.Second)

	syscall.CloseHandle(silent)
	<-rs.doneCh
	if !stopped {
		t.Error("server Run must return within 3s of Stop while a silent client is connected")
	}
}

// K4: a client call must be bounded even when the server never answers HELLO.
func TestWinRobustClientBoundedWhenServerNeverSendsHelloAck(t *testing.T) {
	svc := uniqueWinService("go_wrob_nohelloack")
	name, err := windows.BuildPipeName(winTestRunDir, svc)
	if err != nil {
		t.Fatalf("pipe name: %v", err)
	}
	h, _, callErr := rawCreateNamedPipeW.Call(
		uintptr(unsafe.Pointer(&name[0])),
		uintptr(rawPipeAccessDuplex|winFileFlagFirstPipeInstance),
		uintptr(rawPipeTypeMessage|rawPipeReadModeMsg|rawPipeWait),
		4, rawPipeBufSize, rawPipeBufSize, 0, 0,
	)
	if syscall.Handle(h) == syscall.InvalidHandle {
		t.Fatalf("CreateNamedPipeW: %v", callErr)
	}

	done := make(chan struct{})
	finished := winWithin(6*time.Second, func() {
		defer close(done)
		c := NewIncrementClient(winTestRunDir, svc, testWinClientConfig())
		c.SetCallTimeout(1000)
		c.Refresh()
		_, _ = c.CallIncrementWithTimeout(1, 1000)
		c.Close()
	})

	syscall.CloseHandle(syscall.Handle(h))
	<-done
	if !finished {
		t.Error("Refresh + call with a 1s timeout must return within 6s when the server never sends HELLO_ACK")
	}
}

// N1 guard: the configured call timeout must be honored on the pipe path.
func TestWinRobustPipeCallHonorsTimeoutLongerThan100ms(t *testing.T) {
	svc := uniqueWinService("go_wrob_slow_handler")
	rs := startWinRobustServer(svc, testWinServerConfig(), protocol.MethodIncrement, winSlowIncrementDispatch(300*time.Millisecond), 2)
	defer rs.stopWithin(5 * time.Second)

	c := NewIncrementClient(winTestRunDir, svc, testWinClientConfig())
	defer c.Close()
	c.Refresh()
	start := time.Now()
	v, err := c.CallIncrementWithTimeout(41, 2000)
	if err != nil || v != 42 {
		t.Errorf("a 300ms handler must succeed with a 2000ms timeout: v=%d err=%v after %v", v, err, time.Since(start))
	}
}

// N2: shutdown must not wait forever on a worker blocked writing a response
// to a client that stopped reading.
func TestWinRobustShutdownNotBlockedByNonReadingClient(t *testing.T) {
	const big = 8 * 1024 * 1024
	svc := uniqueWinService("go_wrob_noread")
	scfg := testWinServerConfig()
	scfg.MaxRequestPayloadBytes = big
	scfg.MaxResponsePayloadBytes = big
	rs := startWinRobustServer(svc, scfg, protocol.MethodStringReverse, winStringReverseDispatchHandler(), 2)

	ccfg := testWinClientConfig()
	ccfg.MaxRequestPayloadBytes = big
	ccfg.MaxResponsePayloadBytes = big
	session, err := windows.Connect(winTestRunDir, svc, &ccfg)
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
	// Never read the response; give the worker time to block in WriteFile.
	time.Sleep(500 * time.Millisecond)

	stopped := rs.stopWithin(3 * time.Second)
	session.Close()
	<-rs.doneCh
	if !stopped {
		t.Error("server Run must return within 3s of Stop while a worker is blocked writing")
	}
}

// N3: Stop issued before Run must not be lost.
func TestWinRobustStopBeforeRunIsHonored(t *testing.T) {
	svc := uniqueWinService("go_wrob_stop_first")
	s := NewServerWithWorkers(winTestRunDir, svc, testWinServerConfig(), protocol.MethodIncrement, winSlowIncrementDispatch(0), 2)
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

// N5: an SHM client must notice a crashed server instead of waiting out its
// whole timeout.
func TestWinRobustShmClientNoticesCrashedServer(t *testing.T) {
	svc := uniqueWinService("go_wrob_dead_server")
	cmd := winSpawnHelper(t, "shm_server", svc)

	c := NewIncrementClient(winTestRunDir, svc, testWinShmClientConfig())
	defer c.Close()
	waitWinClientReady(t, c)

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
}
