//go:build unix

package posix

import "syscall"

// Role distinguishes client vs server sessions.
type Role int

const (
	RoleClient Role = 1
	RoleServer Role = 2
)

// ClientConfig configures a client connection.
type ClientConfig struct {
	SupportedProfiles       uint32
	PreferredProfiles       uint32
	MaxRequestPayloadBytes  uint32 // 0 = use default (1024)
	MaxRequestBatchItems    uint32 // 0 = use default (1)
	MaxResponsePayloadBytes uint32
	MaxResponseBatchItems   uint32
	AuthToken               uint64
	PacketSize              uint32 // 0 = kernel limit; capped at SO_SNDBUF-32 and 256 KiB
}

// ServerConfig configures a listener and its accepted sessions.
type ServerConfig struct {
	SupportedProfiles       uint32
	PreferredProfiles       uint32
	MaxRequestPayloadBytes  uint32
	MaxRequestBatchItems    uint32
	MaxResponsePayloadBytes uint32
	MaxResponseBatchItems   uint32
	AuthToken               uint64
	PacketSize              uint32 // 0 = kernel limit; capped at SO_SNDBUF-32 and 256 KiB
	Backlog                 int    // 0 = default (16)
}

// Session is a connected UDS SEQPACKET session (client or server side).
type Session struct {
	fd   int
	role Role

	MaxRequestPayloadBytes  uint32
	MaxRequestBatchItems    uint32
	MaxResponsePayloadBytes uint32
	MaxResponseBatchItems   uint32
	PacketSize              uint32
	SelectedProfile         uint32
	SessionID               uint64

	recvBuf []byte
	pktBuf  []byte

	inflightIDs map[uint64]struct{}
}

func (s *Session) failAllInflight() {
	if s.role != RoleClient || len(s.inflightIDs) == 0 {
		return
	}
	clear(s.inflightIDs)
}

// Fd returns the raw file descriptor for poll/epoll integration.
func (s *Session) Fd() int {
	return s.fd
}

// Role returns the session role.
func (s *Session) Role() Role {
	return s.role
}

// Close closes the session and releases resources.
func (s *Session) Close() {
	if s.fd >= 0 {
		_ = syscall.Close(s.fd)
		s.fd = -1
	}
	s.recvBuf = nil
	s.pktBuf = nil
	s.failAllInflight()
}

// Connect establishes a session to a server at {runDir}/{serviceName}.sock.
// Performs the full handshake. Blocks until connected + handshake done.
func Connect(runDir, serviceName string, config *ClientConfig) (*Session, error) {
	return ConnectTimeout(runDir, serviceName, config, 0, nil)
}

// ConnectTimeout is Connect bounded by timeoutMs: connect and the wait for
// HELLO_ACK together end with ErrTimeout after timeoutMs (0 = no deadline),
// and with ErrAborted once abortCh is closed (nil = no abort).
func ConnectTimeout(runDir, serviceName string, config *ClientConfig, timeoutMs uint32, abortCh <-chan struct{}) (*Session, error) {
	wait := newReceiveWait(timeoutMs, abortCh)
	path, err := buildSocketPath(runDir, serviceName)
	if err != nil {
		return nil, err
	}

	fd, err := seqpacketSocket()
	if err != nil {
		return nil, wrapErr(ErrSocket, err.Error())
	}

	session, herr := connectAndHandshake(fd, path, config, wait)
	if herr != nil {
		_ = syscall.Close(fd)
		return nil, herr
	}
	return session, nil
}
