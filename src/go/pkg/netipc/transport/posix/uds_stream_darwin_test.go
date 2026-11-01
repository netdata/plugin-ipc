//go:build darwin

package posix

import (
	"errors"
	"os"
	"path/filepath"
	"syscall"
	"testing"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

// A receive that consumed part of a message cannot leave the session
// usable: the rest is still queued and would be read as a header.
func TestStreamPartialReceiveEndsReads(t *testing.T) {
	fds, err := syscall.Socketpair(syscall.AF_UNIX, sockType, 0)
	if err != nil {
		t.Fatalf("socketpair: %v", err)
	}
	defer syscall.Close(fds[1])
	session := &Session{
		fd:                      fds[0],
		role:                    RoleServer,
		MaxRequestPayloadBytes:  4096,
		MaxRequestBatchItems:    16,
		MaxResponsePayloadBytes: 4096,
		MaxResponseBatchItems:   16,
		PacketSize:              1024,
		SelectedProfile:         protocol.ProfileBaseline,
		SessionID:               1,
		inflightIDs:             make(map[uint64]struct{}),
	}
	defer session.Close()
	buf := make([]byte, 2048)

	if _, _, err := session.ReceiveTimeout(buf, 1, nil); !errors.Is(err, ErrTimeout) {
		t.Fatalf("timeout before the first byte: %v, want ErrTimeout", err)
	}

	pkt := streamOuter(5, []byte("hello"))
	if err := sendPacket(fds[1], pkt, nil); err != nil {
		t.Fatalf("send: %v", err)
	}
	if _, payload, err := session.Receive(buf); err != nil || string(payload) != "hello" {
		t.Fatalf("intact after a clean timeout: %q, %v", payload, err)
	}

	if err := sendPacket(fds[1], pkt[:10], nil); err != nil {
		t.Fatalf("send: %v", err)
	}
	if _, _, err := session.ReceiveTimeout(buf, 1, nil); !errors.Is(err, ErrTimeout) {
		t.Fatalf("timeout inside a message: %v, want ErrTimeout", err)
	}
	if err := sendPacket(fds[1], pkt[10:], nil); err != nil {
		t.Fatalf("send: %v", err)
	}
	if _, _, err := session.Receive(buf); !errors.Is(err, ErrRecv) {
		t.Fatalf("later receives: %v, want ErrRecv", err)
	}

	// Sends always write whole messages, so they stay valid.
	resp := protocol.Header{Kind: protocol.KindResponse, Code: 1, ItemCount: 1, MessageID: 9}
	if err := session.Send(&resp, []byte("ok")); err != nil {
		t.Fatalf("sends still work: %v", err)
	}
	if n, err := streamRecvPacket(fds[1], buf, 1024, nil); err != nil || n != protocol.HeaderSize+2 {
		t.Fatalf("peer gets the response: %d, %v", n, err)
	}
}

// macOS binds in {service}.d/ and renames the socket into place.
func TestListenBindDir(t *testing.T) {
	runDir := testRunDir(t)
	service := uniqueService(t)
	dir := filepath.Join(runDir, service+".d")
	path := filepath.Join(runDir, service+".sock")
	t.Cleanup(func() { _ = os.Remove(path) })
	gone := func(p string) bool {
		_, err := os.Lstat(p)
		return errors.Is(err, os.ErrNotExist)
	}

	listener := startListener(t, runDir, service, defaultServerConfig())
	info, err := os.Stat(path)
	if err != nil {
		t.Fatalf("stat socket: %v", err)
	}
	if mode := info.Mode().Perm(); mode != 0o600 {
		t.Fatalf("socket mode %o, want 0600", mode)
	}
	if !gone(dir) {
		t.Fatal("bind directory not removed")
	}
	listener.Close()

	// A crashed server's leftover directory is reclaimed.
	if err := os.Mkdir(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "s"), nil, 0o600); err != nil {
		t.Fatal(err)
	}
	startListener(t, runDir, service, defaultServerConfig()).Close()
	if !gone(dir) {
		t.Fatal("leftover directory not removed")
	}

	// A non-directory at the name is junk, as at the socket name.
	if err := os.WriteFile(dir, nil, 0o600); err != nil {
		t.Fatal(err)
	}
	startListener(t, runDir, service, defaultServerConfig()).Close()
	if !gone(dir) {
		t.Fatal("leftover file not removed")
	}

	// Foreign content is never deleted: listen fails instead.
	if err := os.Mkdir(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	other := filepath.Join(dir, "other")
	if err := os.WriteFile(other, nil, 0o600); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Remove(other); _ = os.Remove(dir) })
	if l, err := Listen(runDir, service, defaultServerConfig()); !errors.Is(err, ErrSocket) {
		if l != nil {
			l.Close()
		}
		t.Fatalf("listen over foreign content: %v, want ErrSocket", err)
	}
	if _, err := os.Stat(other); err != nil {
		t.Fatalf("foreign content removed: %v", err)
	}
	if err := os.Remove(other); err != nil {
		t.Fatal(err)
	}

	// A directory where the socket goes is foreign content too.
	sub := filepath.Join(dir, "s")
	if err := os.Mkdir(sub, 0o700); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Remove(sub) })
	if l, err := Listen(runDir, service, defaultServerConfig()); !errors.Is(err, ErrSocket) {
		if l != nil {
			l.Close()
		}
		t.Fatalf("listen over a directory at the socket name: %v, want ErrSocket", err)
	}
	if info, err := os.Stat(sub); err != nil || !info.IsDir() {
		t.Fatalf("foreign subdirectory removed: %v", err)
	}
}
