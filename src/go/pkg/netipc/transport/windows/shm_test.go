//go:build windows

package windows

import (
	"encoding/binary"
	"errors"
	"strings"
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

func TestWinShmHelpers(t *testing.T) {
	if got := winShmAlignCacheline(1); got != 64 {
		t.Fatalf("winShmAlignCacheline(1) = %d, want 64", got)
	}
	if got := winShmAlignCacheline(64); got != 64 {
		t.Fatalf("winShmAlignCacheline(64) = %d, want 64", got)
	}
	if got := winShmAlignCacheline(65); got != 128 {
		t.Fatalf("winShmAlignCacheline(65) = %d, want 128", got)
	}

	if err := validateWinShmProfile(0); !errors.Is(err, ErrWinShmBadParam) {
		t.Fatalf("validateWinShmProfile(0) = %v, want ErrWinShmBadParam", err)
	}
	if err := validateWinShmProfile(WinShmProfileHybrid); err != nil {
		t.Fatalf("validateWinShmProfile(hybrid) failed: %v", err)
	}
	if err := validateWinShmProfile(WinShmProfileBusywait); err != nil {
		t.Fatalf("validateWinShmProfile(busywait) failed: %v", err)
	}

	h1 := computeShmHash("run", "svc", 123)
	h2 := computeShmHash("run", "svc", 123)
	h3 := computeShmHash("run", "svc", 124)
	if h1 != h2 {
		t.Fatalf("computeShmHash not deterministic: %d != %d", h1, h2)
	}
	if h1 == h3 {
		t.Fatal("computeShmHash should change when auth token changes")
	}

	name, err := buildWinShmObjectName(h1, "svc", WinShmProfileHybrid, 7, "mapping")
	if err != nil {
		t.Fatalf("buildWinShmObjectName failed: %v", err)
	}
	if got := syscall.UTF16ToString(name); !strings.Contains(got, "svc") {
		t.Fatalf("object name %q does not contain service name", got)
	}

	tooLong := strings.Repeat("a", 260)
	if _, err := buildWinShmObjectName(h1, tooLong, WinShmProfileHybrid, 7, "mapping"); !errors.Is(err, ErrWinShmBadParam) {
		t.Fatalf("buildWinShmObjectName long name = %v, want ErrWinShmBadParam", err)
	}
}

func TestWinShmCreateAttachAndCloseValidation(t *testing.T) {
	runDir := t.TempDir()
	service := "validation"
	const authToken uint64 = 0x5678
	const sessionID uint64 = 9

	if _, err := WinShmServerCreate(runDir, "bad/name", authToken, sessionID, WinShmProfileHybrid, 4096, 4096); err == nil {
		t.Fatal("WinShmServerCreate with invalid service name should fail")
	}
	if _, err := WinShmServerCreate(runDir, service, authToken, sessionID, 0, 4096, 4096); !errors.Is(err, ErrWinShmBadParam) {
		t.Fatalf("WinShmServerCreate invalid profile = %v, want ErrWinShmBadParam", err)
	}
	if _, err := WinShmClientAttach(runDir, service, authToken, sessionID, 0); !errors.Is(err, ErrWinShmBadParam) {
		t.Fatalf("WinShmClientAttach invalid profile = %v, want ErrWinShmBadParam", err)
	}
	if _, err := WinShmClientAttach(runDir, service, authToken, sessionID, WinShmProfileHybrid); err == nil {
		t.Fatal("WinShmClientAttach without mapping should fail")
	}

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

	if server.GetRole() != WinShmRoleServer {
		t.Fatalf("server role = %d, want %d", server.GetRole(), WinShmRoleServer)
	}
	if client.GetRole() != WinShmRoleClient {
		t.Fatalf("client role = %d, want %d", client.GetRole(), WinShmRoleClient)
	}
}

func TestWinShmClientAttachRejectsCorruptHeader(t *testing.T) {
	cases := []struct {
		name   string
		want   error
		mutate func([]byte)
	}{
		{
			name: "bad magic",
			want: ErrWinShmBadMagic,
			mutate: func(data []byte) {
				binary.NativeEndian.PutUint32(data[wshOFFMagic:], 0)
			},
		},
		{
			name: "bad version",
			want: ErrWinShmBadVersion,
			mutate: func(data []byte) {
				binary.NativeEndian.PutUint32(data[wshOFFVersion:], winShmVersion+1)
			},
		},
		{
			name: "bad header len",
			want: ErrWinShmBadHeader,
			mutate: func(data []byte) {
				binary.NativeEndian.PutUint32(data[wshOFFHeaderLen:], winShmHeaderLen+64)
			},
		},
		{
			name: "bad profile",
			want: ErrWinShmBadProfile,
			mutate: func(data []byte) {
				binary.NativeEndian.PutUint32(data[wshOFFProfile:], WinShmProfileBusywait)
			},
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			runDir := t.TempDir()
			service := "corrupt-header"
			const authToken uint64 = 0x123456
			const sessionID uint64 = 15

			server, err := WinShmServerCreate(runDir, service, authToken, sessionID, WinShmProfileHybrid, 4096, 4096)
			if err != nil {
				t.Fatalf("WinShmServerCreate failed: %v", err)
			}
			defer server.WinShmDestroy()

			data := unsafe.Slice((*byte)(unsafe.Pointer(server.base)), server.size)
			tc.mutate(data)

			_, err = WinShmClientAttach(runDir, service, authToken, sessionID, WinShmProfileHybrid)
			if !errors.Is(err, tc.want) {
				t.Fatalf("WinShmClientAttach error = %v, want %v", err, tc.want)
			}
		})
	}
}

func TestWinShmSendReceiveValidation(t *testing.T) {
	runDir := t.TempDir()
	service := "validation-io"
	const authToken uint64 = 0x6789
	const sessionID uint64 = 11

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

	if err := server.WinShmSend(nil); !errors.Is(err, ErrWinShmBadParam) {
		t.Fatalf("WinShmSend(nil) = %v, want ErrWinShmBadParam", err)
	}
	if _, err := client.WinShmReceive(nil, 10); !errors.Is(err, ErrWinShmBadParam) {
		t.Fatalf("WinShmReceive(nil) = %v, want ErrWinShmBadParam", err)
	}

	tooLarge := make([]byte, server.responseCapacity+1)
	if err := server.WinShmSend(tooLarge); !errors.Is(err, ErrWinShmMsgTooLarge) {
		t.Fatalf("WinShmSend(tooLarge) = %v, want ErrWinShmMsgTooLarge", err)
	}

	msg := []byte("0123456789")
	if err := server.WinShmSend(msg); err != nil {
		t.Fatalf("WinShmSend failed: %v", err)
	}

	smallBuf := make([]byte, 4)
	n, err := client.WinShmReceive(smallBuf, 1000)
	if !errors.Is(err, ErrWinShmMsgTooLarge) {
		t.Fatalf("WinShmReceive(smallBuf) = %v, want ErrWinShmMsgTooLarge", err)
	}
	if n != len(msg) {
		t.Fatalf("WinShmReceive reported len %d, want %d", n, len(msg))
	}
}

func TestWinShmReceiveDetectsPeerClosed(t *testing.T) {
	runDir := t.TempDir()
	service := "peer-closed"
	const authToken uint64 = 0x789a
	const sessionID uint64 = 13

	server, err := WinShmServerCreate(runDir, service, authToken, sessionID, WinShmProfileHybrid, 4096, 4096)
	if err != nil {
		t.Fatalf("WinShmServerCreate failed: %v", err)
	}
	defer server.WinShmDestroy()

	client, err := WinShmClientAttach(runDir, service, authToken, sessionID, WinShmProfileHybrid)
	if err != nil {
		t.Fatalf("WinShmClientAttach failed: %v", err)
	}

	results := make(chan error, 1)
	go func() {
		buf := make([]byte, 128)
		_, err := server.WinShmReceive(buf, 1000)
		results <- err
	}()

	time.Sleep(20 * time.Millisecond)
	client.WinShmClose()

	select {
	case err := <-results:
		if !errors.Is(err, ErrWinShmDisconnected) {
			t.Fatalf("WinShmReceive after peer close = %v, want ErrWinShmDisconnected", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("WinShmReceive did not observe peer close")
	}
}

func TestWinShmReceiveTimeoutHybrid(t *testing.T) {
	runDir := t.TempDir()
	service := "timeout-hybrid"
	const authToken uint64 = 0x8912
	const sessionID uint64 = 17

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

	buf := make([]byte, 128)
	if _, err := server.WinShmReceive(buf, 10); !errors.Is(err, ErrWinShmTimeout) {
		t.Fatalf("WinShmReceive hybrid timeout = %v, want %v", err, ErrWinShmTimeout)
	}
}

func TestWinShmReceiveTimeoutBusywait(t *testing.T) {
	runDir := t.TempDir()
	service := "timeout-busywait"
	const authToken uint64 = 0x8913
	const sessionID uint64 = 19

	server, err := WinShmServerCreate(runDir, service, authToken, sessionID, WinShmProfileBusywait, 4096, 4096)
	if err != nil {
		t.Fatalf("WinShmServerCreate failed: %v", err)
	}
	defer server.WinShmDestroy()

	client, err := WinShmClientAttach(runDir, service, authToken, sessionID, WinShmProfileBusywait)
	if err != nil {
		t.Fatalf("WinShmClientAttach failed: %v", err)
	}
	defer client.WinShmClose()

	buf := make([]byte, 128)
	if _, err := server.WinShmReceive(buf, 10); !errors.Is(err, ErrWinShmTimeout) {
		t.Fatalf("WinShmReceive busywait timeout = %v, want %v", err, ErrWinShmTimeout)
	}
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
