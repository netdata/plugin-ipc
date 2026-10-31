//go:build unix

package posix

import (
	"os"
	"sync"
	"sync/atomic"
	"syscall"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

// Listener is a listening UDS SEQPACKET endpoint.
type Listener struct {
	mu            sync.Mutex
	fd            int
	config        ServerConfig
	path          string
	nextSessionID atomic.Uint64
}

// Listen creates a listener on {runDir}/{serviceName}.sock.
// Performs stale endpoint recovery.
func Listen(runDir, serviceName string, config ServerConfig) (*Listener, error) {
	path, err := buildSocketPath(runDir, serviceName)
	if err != nil {
		return nil, err
	}

	stale := checkAndRecoverStale(path)
	if stale == staleLiveServer {
		return nil, ErrAddrInUse
	}

	fd, err := seqpacketSocket()
	if err != nil {
		return nil, wrapErr(ErrSocket, err.Error())
	}

	sa := &syscall.SockaddrUnix{Name: path}
	if err := syscall.Bind(fd, sa); err != nil {
		_ = syscall.Close(fd)
		return nil, wrapErr(ErrSocket, "bind: "+err.Error())
	}

	backlog := config.Backlog
	if backlog <= 0 {
		backlog = defaultBacklog
	}

	if err := syscall.Listen(fd, backlog); err != nil {
		_ = syscall.Close(fd)
		_ = os.Remove(path)
		return nil, wrapErr(ErrSocket, "listen: "+err.Error())
	}

	return &Listener{
		fd:     fd,
		config: config,
		path:   path,
	}, nil
}

// Fd returns the raw file descriptor for poll/epoll integration.
func (l *Listener) Fd() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.fd
}

// SetPayloadLimits updates the payload limits used for future handshakes.
func (l *Listener) SetPayloadLimits(maxRequestPayloadBytes, maxResponsePayloadBytes uint32) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.config.MaxRequestPayloadBytes = maxRequestPayloadBytes
	l.config.MaxResponsePayloadBytes = maxResponsePayloadBytes
}

// Accept accepts one client connection. Performs the full handshake.
// Blocks until a client connects and the handshake completes.
func (l *Listener) Accept() (*Session, error) {
	sessionID := l.nextSessionID.Add(1)
	l.mu.Lock()
	config := l.config
	l.mu.Unlock()
	return l.AcceptWithConfig(sessionID, config)
}

// AcceptWithConfig accepts one client connection using a caller-provided
// per-session server config and session ID. The handshake waits at most
// protocol.ServerHandshakeTimeoutMs for HELLO; servers that must not block
// their accept loop use AcceptPending and PendingConn.Handshake instead.
func (l *Listener) AcceptWithConfig(sessionID uint64, config ServerConfig) (*Session, error) {
	pending, err := l.AcceptPending()
	if err != nil {
		return nil, err
	}
	return pending.Handshake(sessionID, config, protocol.ServerHandshakeTimeoutMs, nil)
}

// PendingConn is an accepted connection whose handshake has not run yet.
// Splitting accept from handshake lets a server bound the handshake and run
// it outside its accept loop.
type PendingConn struct {
	fd int
}

// AcceptPending accepts one connection without reading HELLO.
func (l *Listener) AcceptPending() (*PendingConn, error) {
	l.mu.Lock()
	fd := l.fd
	l.mu.Unlock()
	if fd < 0 {
		return nil, wrapErr(ErrAccept, "listener closed")
	}

	nfd, err := acceptCloexec(fd)
	if err != nil {
		return nil, wrapErr(ErrAccept, err.Error())
	}
	return &PendingConn{fd: nfd}, nil
}

// Fd returns the connection's file descriptor, or -1 once consumed.
func (p *PendingConn) Fd() int {
	return p.fd
}

// Handshake reads HELLO and answers it. timeoutMs bounds the wait for HELLO
// (0 = no deadline) and abortCh, when non-nil, cancels it. The connection is
// consumed: on success the Session owns the fd, on failure it is closed.
func (p *PendingConn) Handshake(
	sessionID uint64,
	config ServerConfig,
	timeoutMs uint32,
	abortCh <-chan struct{},
) (*Session, error) {
	fd := p.fd
	if fd < 0 {
		return nil, wrapErr(ErrAccept, "connection already consumed")
	}
	p.fd = -1

	session, err := serverHandshakeWait(fd, &config, sessionID, newReceiveWait(timeoutMs, abortCh))
	if err != nil {
		_ = syscall.Close(fd)
		return nil, err
	}
	return session, nil
}

// Close drops a connection that will not be handshaken.
func (p *PendingConn) Close() {
	if p.fd >= 0 {
		_ = syscall.Close(p.fd)
		p.fd = -1
	}
}

// Close closes the listener, stops accepting, and unlinks the socket file.
func (l *Listener) Close() {
	l.mu.Lock()
	fd := l.fd
	path := l.path
	l.fd = -1
	l.path = ""
	l.mu.Unlock()

	if fd >= 0 {
		_ = syscall.Close(fd)
	}
	if path != "" {
		_ = os.Remove(path)
	}
}
