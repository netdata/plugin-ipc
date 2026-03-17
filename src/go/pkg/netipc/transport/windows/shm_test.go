//go:build windows

package windows

import (
	"sync/atomic"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

type shmReceiveResult struct {
	payload []byte
	err     error
}

func TestWinShmReceiveIgnoresSpuriousWakeClient(t *testing.T) {
	testWinShmReceiveIgnoresSpuriousWake(t, false)
}

func TestWinShmReceiveIgnoresSpuriousWakeServer(t *testing.T) {
	testWinShmReceiveIgnoresSpuriousWake(t, true)
}

func testWinShmReceiveIgnoresSpuriousWake(t *testing.T, serverReceives bool) {
	t.Helper()

	runDir := t.TempDir()
	service := "spurious-wake"
	const authToken uint64 = 0x1234
	const sessionID uint64 = 7

	server, err := WinShmServerCreate(runDir, service, authToken, sessionID, WinShmProfileHybrid, 4096, 4096)
	if err != nil {
		t.Fatalf("WinShmServerCreate failed: %v", err)
	}
	defer server.WinShmDestroy()

	client, err := WinShmClientAttach(runDir, service, authToken, sessionID, WinShmProfileHybrid)
	if err != nil {
		t.Fatalf("WinShmClientAttach failed: %v", err)
	}
	defer client.WinShmClose()

	first := []byte("first-message")
	second := []byte("second-message")

	var sender *WinShmContext
	var receiver *WinShmContext
	var waitingOff int
	var waitEvent syscall.Handle

	if serverReceives {
		sender = client
		receiver = server
		waitingOff = wshOFFReqServerWaiting
		waitEvent = server.reqEvent
	} else {
		sender = server
		receiver = client
		waitingOff = wshOFFRespClientWaiting
		waitEvent = client.respEvent
	}

	if err := sender.WinShmSend(first); err != nil {
		t.Fatalf("first WinShmSend failed: %v", err)
	}

	firstBuf := make([]byte, 128)
	firstLen, err := receiver.WinShmReceive(firstBuf, 1000)
	if err != nil {
		t.Fatalf("first WinShmReceive failed: %v", err)
	}
	if got := string(firstBuf[:firstLen]); got != string(first) {
		t.Fatalf("first payload = %q, want %q", got, string(first))
	}

	results := make(chan shmReceiveResult, 1)
	go func() {
		buf := make([]byte, 128)
		n, err := receiver.WinShmReceive(buf, 1000)
		if err != nil {
			results <- shmReceiveResult{err: err}
			return
		}
		results <- shmReceiveResult{payload: append([]byte(nil), buf[:n]...)}
	}()

	data := unsafe.Slice((*byte)(unsafe.Pointer(receiver.base)), receiver.size)
	deadline := time.Now().Add(time.Second)
	for atomic.LoadInt32((*int32)(unsafe.Pointer(&data[waitingOff]))) == 0 {
		if time.Now().After(deadline) {
			t.Fatal("receiver never entered the wait state")
		}
		time.Sleep(time.Millisecond)
	}

	if ret, _, _ := procSetEvent.Call(uintptr(waitEvent)); ret == 0 {
		t.Fatal("SetEvent failed for spurious wake probe")
	}

	time.Sleep(10 * time.Millisecond)

	if err := sender.WinShmSend(second); err != nil {
		t.Fatalf("second WinShmSend failed: %v", err)
	}

	select {
	case res := <-results:
		if res.err != nil {
			t.Fatalf("second WinShmReceive failed: %v", res.err)
		}
		if got := string(res.payload); got != string(second) {
			t.Fatalf("second payload = %q, want %q", got, string(second))
		}
	case <-time.After(2 * time.Second):
		t.Fatal("second WinShmReceive timed out")
	}
}
