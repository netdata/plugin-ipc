//go:build unix

// Package cgroups provides L2 orchestration for the cgroups snapshot service.
//
// Pure composition of L1 (UDS/SHM) + Codec. No direct socket/mmap calls.
// Client manages connection lifecycle with at-least-once retry.
// Server handles accept, read, dispatch, respond.
//
// Pure Go — no cgo. Works with CGO_ENABLED=0.
package cgroups

import (
	"encoding/binary"
	"errors"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix"
)

// ---------------------------------------------------------------------------
//  Client state
// ---------------------------------------------------------------------------

// ClientState represents the connection state machine.
type ClientState int

const (
	StateDisconnected ClientState = iota
	StateConnecting
	StateReady
	StateNotFound
	StateAuthFailed
	StateIncompatible
	StateBroken
)

// ClientStatus is a diagnostic counters snapshot.
type ClientStatus struct {
	State          ClientState
	ConnectCount   uint32
	ReconnectCount uint32
	CallCount      uint32
	ErrorCount     uint32
}

// ---------------------------------------------------------------------------
//  Client context
// ---------------------------------------------------------------------------

// Client is an L2 client context for the cgroups snapshot service.
// Manages connection lifecycle and provides typed blocking calls
// with at-least-once retry semantics.
type Client struct {
	state       ClientState
	runDir      string
	serviceName string
	config      posix.ClientConfig

	// Connection (managed internally)
	session *posix.Session
	shm     *posix.ShmContext

	// Stats
	connectCount   uint32
	reconnectCount uint32
	callCount      uint32
	errorCount     uint32
}

// NewClient creates a new client context. Does NOT connect. Does NOT
// require the server to be running.
func NewClient(runDir, serviceName string, config posix.ClientConfig) *Client {
	return &Client{
		state:       StateDisconnected,
		runDir:      runDir,
		serviceName: serviceName,
		config:      config,
	}
}

// Refresh attempts connect if DISCONNECTED/NOT_FOUND, reconnect if BROKEN.
// Returns true if the state changed.
func (c *Client) Refresh() bool {
	oldState := c.state

	switch c.state {
	case StateDisconnected, StateNotFound:
		c.state = StateConnecting
		c.state = c.tryConnect()
		if c.state == StateReady {
			c.connectCount++
		}

	case StateBroken:
		c.disconnect()
		c.state = StateConnecting
		c.state = c.tryConnect()
		if c.state == StateReady {
			c.reconnectCount++
		}

	case StateReady, StateConnecting, StateAuthFailed, StateIncompatible:
		// No action needed
	}

	return c.state != oldState
}

// Ready returns true only if the client is in the READY state.
// Cheap cached boolean, no I/O.
func (c *Client) Ready() bool {
	return c.state == StateReady
}

// Status returns a diagnostic counters snapshot.
func (c *Client) Status() ClientStatus {
	return ClientStatus{
		State:          c.state,
		ConnectCount:   c.connectCount,
		ReconnectCount: c.reconnectCount,
		CallCount:      c.callCount,
		ErrorCount:     c.errorCount,
	}
}

// CallSnapshot performs a blocking typed cgroups snapshot call.
//
// responseBuf must be large enough for the expected snapshot.
//
// Retry policy (per spec): if the call fails and the context was
// previously READY, disconnect, reconnect (full handshake), retry ONCE.
func (c *Client) CallSnapshot(responseBuf []byte) (*protocol.CgroupsResponseView, error) {
	// Fail fast if not READY
	if c.state != StateReady {
		c.errorCount++
		return nil, protocol.ErrBadLayout
	}

	// First attempt
	payloadLen, firstErr := c.doCgroupsCallRaw(responseBuf)
	if firstErr == nil {
		view, err := protocol.DecodeCgroupsResponse(responseBuf[:payloadLen])
		if err == nil {
			c.callCount++
			return &view, nil
		}
		firstErr = err
	}

	// Call failed. Was previously READY: disconnect, reconnect, retry ONCE.
	c.disconnect()
	c.state = StateBroken

	// Reconnect (full handshake)
	c.state = c.tryConnect()
	if c.state != StateReady {
		c.errorCount++
		return nil, firstErr
	}
	c.reconnectCount++

	// Retry once
	payloadLen, retryErr := c.doCgroupsCallRaw(responseBuf)
	if retryErr == nil {
		view, err := protocol.DecodeCgroupsResponse(responseBuf[:payloadLen])
		if err == nil {
			c.callCount++
			return &view, nil
		}
		retryErr = err
	}

	// Retry also failed
	c.disconnect()
	c.state = StateBroken
	c.errorCount++
	return nil, retryErr
}

// Close tears down the connection and releases resources.
func (c *Client) Close() {
	c.disconnect()
	c.state = StateDisconnected
}

// ------------------------------------------------------------------
//  Internal helpers
// ------------------------------------------------------------------

func (c *Client) disconnect() {
	if c.shm != nil {
		c.shm.ShmClose()
		c.shm = nil
	}
	if c.session != nil {
		c.session.Close()
		c.session = nil
	}
}

func (c *Client) tryConnect() ClientState {
	session, err := posix.Connect(c.runDir, c.serviceName, &c.config)
	if err != nil {
		switch {
		case isConnectError(err):
			return StateNotFound
		case isAuthError(err):
			return StateAuthFailed
		case isProfileError(err):
			return StateIncompatible
		default:
			return StateDisconnected
		}
	}

	// SHM upgrade if negotiated
	if session.SelectedProfile == protocol.ProfileSHMHybrid ||
		session.SelectedProfile == protocol.ProfileSHMFutex {
		// Retry attach: server creates the SHM region after
		// the UDS handshake, so it may not exist yet.
		for i := 0; i < 200; i++ {
			shm, serr := posix.ShmClientAttach(c.runDir, c.serviceName)
			if serr == nil {
				c.shm = shm
				break
			}
			time.Sleep(5 * time.Millisecond)
		}
		// If SHM attach failed, fall back to UDS only.
	}

	c.session = session
	return StateReady
}

// doCgroupsCallRaw performs a single call attempt. On success, the
// response payload is in responseBuf[:payloadLen].
func (c *Client) doCgroupsCallRaw(responseBuf []byte) (int, error) {
	// 1. Encode request using Codec
	req := protocol.CgroupsRequest{LayoutVersion: 1, Flags: 0}
	var reqBuf [4]byte
	if req.Encode(reqBuf[:]) == 0 {
		return 0, protocol.ErrTruncated
	}

	// 2. Build outer header
	hdr := protocol.Header{
		Kind:            protocol.KindRequest,
		Code:            protocol.MethodCgroupsSnapshot,
		Flags:           0,
		ItemCount:       1,
		MessageID:       uint64(c.callCount) + 1,
		TransportStatus: protocol.StatusOK,
	}

	// 3. Send via L1 (SHM or UDS)
	if err := c.transportSend(&hdr, reqBuf[:]); err != nil {
		return 0, err
	}

	// 4. Receive via L1
	respHdr, payloadLen, err := c.transportReceive(responseBuf)
	if err != nil {
		return 0, err
	}

	// 5. Check transport_status BEFORE decode (spec requirement)
	if respHdr.TransportStatus != protocol.StatusOK {
		return 0, protocol.ErrBadLayout
	}

	return payloadLen, nil
}

func (c *Client) transportSend(hdr *protocol.Header, payload []byte) error {
	if c.shm != nil {
		msgLen := protocol.HeaderSize + len(payload)
		msg := make([]byte, msgLen)

		hdr.Magic = protocol.MagicMsg
		hdr.Version = protocol.Version
		hdr.HeaderLen = protocol.HeaderLen
		hdr.PayloadLen = uint32(len(payload))

		hdr.Encode(msg[:protocol.HeaderSize])
		if len(payload) > 0 {
			copy(msg[protocol.HeaderSize:], payload)
		}

		return c.shm.ShmSend(msg)
	}

	// UDS path
	if c.session == nil {
		return protocol.ErrTruncated
	}
	return c.session.Send(hdr, payload)
}

func (c *Client) transportReceive(responseBuf []byte) (protocol.Header, int, error) {
	if c.shm != nil {
		msg, err := c.shm.ShmReceive(30000)
		if err != nil {
			return protocol.Header{}, 0, protocol.ErrTruncated
		}
		if len(msg) < protocol.HeaderSize {
			return protocol.Header{}, 0, protocol.ErrTruncated
		}

		hdr, err := protocol.DecodeHeader(msg)
		if err != nil {
			return protocol.Header{}, 0, err
		}

		payloadLen := len(msg) - protocol.HeaderSize
		if payloadLen > len(responseBuf) {
			return protocol.Header{}, 0, protocol.ErrOverflow
		}
		copy(responseBuf[:payloadLen], msg[protocol.HeaderSize:])
		return hdr, payloadLen, nil
	}

	// UDS path: receive returns (Header, payload, error)
	if c.session == nil {
		return protocol.Header{}, 0, protocol.ErrTruncated
	}

	scratch := make([]byte, len(responseBuf)+protocol.HeaderSize)
	hdr, payload, err := c.session.Receive(scratch)
	if err != nil {
		return protocol.Header{}, 0, protocol.ErrTruncated
	}

	payloadLen := len(payload)
	if payloadLen > len(responseBuf) {
		return protocol.Header{}, 0, protocol.ErrOverflow
	}
	copy(responseBuf[:payloadLen], payload)
	return hdr, payloadLen, nil
}

// Error classification helpers
func isConnectError(err error) bool {
	return errors.Is(err, posix.ErrConnect) || errors.Is(err, posix.ErrSocket)
}

func isAuthError(err error) bool {
	return errors.Is(err, posix.ErrAuthFailed)
}

func isProfileError(err error) bool {
	return errors.Is(err, posix.ErrNoProfile)
}

// ---------------------------------------------------------------------------
//  Managed server
// ---------------------------------------------------------------------------

// HandlerFunc is the server handler callback. Receives (methodCode,
// requestPayload). Returns (responsePayload, ok). If ok is false,
// transport_status = INTERNAL_ERROR with empty payload.
type HandlerFunc func(methodCode uint16, request []byte) ([]byte, bool)

// Server is an L2 managed server for the cgroups snapshot service.
type Server struct {
	runDir        string
	serviceName   string
	config        posix.ServerConfig
	handler       HandlerFunc
	running       atomic.Bool
}

// NewServer creates a new managed server.
func NewServer(runDir, serviceName string, config posix.ServerConfig, handler HandlerFunc) *Server {
	return &Server{
		runDir:      runDir,
		serviceName: serviceName,
		config:      config,
		handler:     handler,
	}
}

// Run starts the acceptor loop. Blocking. Accepts clients, reads requests,
// dispatches to the handler, sends responses.
// Returns when Stop() is called or on fatal error.
func (s *Server) Run() error {
	listener, err := posix.Listen(s.runDir, s.serviceName, s.config)
	if err != nil {
		return err
	}
	defer listener.Close()

	s.running.Store(true)

	for s.running.Load() {
		// Poll the listener fd before blocking on accept
		ready := pollFd(listener.Fd(), 500)
		if ready < 0 {
			break
		}
		if ready == 0 {
			continue
		}

		session, err := listener.Accept()
		if err != nil {
			if !s.running.Load() {
				break
			}
			time.Sleep(10 * time.Millisecond)
			continue
		}

		// SHM upgrade if negotiated
		var shm *posix.ShmContext
		if session.SelectedProfile == protocol.ProfileSHMHybrid ||
			session.SelectedProfile == protocol.ProfileSHMFutex {
			shmCtx, serr := posix.ShmServerCreate(
				s.runDir, s.serviceName,
				session.MaxRequestPayloadBytes+uint32(protocol.HeaderSize),
				session.MaxResponsePayloadBytes+uint32(protocol.HeaderSize),
			)
			if serr == nil {
				shm = shmCtx
			}
		}

		// Handle this session (blocking, single-threaded)
		s.handleSession(session, shm)
	}

	return nil
}

// Stop signals the server to stop.
func (s *Server) Stop() {
	s.running.Store(false)
}

func (s *Server) handleSession(session *posix.Session, shm *posix.ShmContext) {
	recvBuf := make([]byte, 65536)

	defer func() {
		if shm != nil {
			shm.ShmDestroy()
		}
		session.Close()
	}()

	for s.running.Load() {
		var hdr protocol.Header
		var payload []byte

		if shm != nil {
			msg, err := shm.ShmReceive(500)
			if err != nil {
				if err == posix.ErrShmTimeout {
					continue
				}
				return
			}
			if len(msg) < protocol.HeaderSize {
				return
			}
			h, err := protocol.DecodeHeader(msg)
			if err != nil {
				return
			}
			hdr = h
			// Copy payload to avoid holding SHM reference
			payload = make([]byte, len(msg)-protocol.HeaderSize)
			copy(payload, msg[protocol.HeaderSize:])
		} else {
			// Poll the session fd before blocking on receive
			ready := pollFd(session.Fd(), 500)
			if ready < 0 {
				return
			}
			if ready == 0 {
				continue
			}

			h, p, err := session.Receive(recvBuf)
			if err != nil {
				return
			}
			hdr = h
			payload = make([]byte, len(p))
			copy(payload, p)
		}

		// Skip non-request messages
		if hdr.Kind != protocol.KindRequest {
			continue
		}

		// Dispatch to handler
		respPayload, ok := s.handler(hdr.Code, payload)

		// Build response header
		respHdr := protocol.Header{
			Kind:      protocol.KindResponse,
			Code:      hdr.Code,
			MessageID: hdr.MessageID,
			ItemCount: 1,
		}

		if ok {
			respHdr.TransportStatus = protocol.StatusOK
		} else {
			// Handler failure: INTERNAL_ERROR + empty payload
			respHdr.TransportStatus = protocol.StatusInternalError
			respPayload = nil
		}

		// Send response via the active transport
		if shm != nil {
			msgLen := protocol.HeaderSize + len(respPayload)
			msg := make([]byte, msgLen)

			respHdr.Magic = protocol.MagicMsg
			respHdr.Version = protocol.Version
			respHdr.HeaderLen = protocol.HeaderLen
			respHdr.PayloadLen = uint32(len(respPayload))

			respHdr.Encode(msg[:protocol.HeaderSize])
			if len(respPayload) > 0 {
				copy(msg[protocol.HeaderSize:], respPayload)
			}

			if err := shm.ShmSend(msg); err != nil {
				return
			}
		} else {
			if err := session.Send(&respHdr, respPayload); err != nil {
				return
			}
		}
	}
}

// ---------------------------------------------------------------------------
//  Internal: poll helper (raw syscall, pure Go, no cgo)
// ---------------------------------------------------------------------------

// poll constants (not exported by Go's syscall package)
const (
	_POLLIN   = 0x0001
	_POLLERR  = 0x0008
	_POLLHUP  = 0x0010
	_POLLNVAL = 0x0020
)

// pollfd matches struct pollfd from <poll.h>.
type pollfd struct {
	fd      int32
	events  int16
	revents int16
}

// pollFd polls a file descriptor for readability with a timeout in ms.
// Returns: 1 = data ready, 0 = timeout, -1 = error/hangup.
func pollFd(fd int, timeoutMs int) int {
	pfd := pollfd{
		fd:     int32(fd),
		events: _POLLIN,
	}

	r, _, errno := syscall.Syscall(
		syscall.SYS_POLL,
		uintptr(unsafe.Pointer(&pfd)),
		1,
		uintptr(timeoutMs),
	)

	n := int(r)
	if n < 0 {
		if errno == syscall.EINTR {
			return 0
		}
		return -1
	}

	if n == 0 {
		return 0
	}

	if pfd.revents&(_POLLERR|_POLLHUP|_POLLNVAL) != 0 {
		return -1
	}

	if pfd.revents&_POLLIN != 0 {
		return 1
	}

	return 0
}

// Suppress unused import warnings.
var _ = binary.LittleEndian
