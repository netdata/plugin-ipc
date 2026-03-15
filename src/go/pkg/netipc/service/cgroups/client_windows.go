//go:build windows

// L2 cgroups snapshot client for Windows.
//
// Identical state machine and retry logic as the POSIX client.
// Uses Named Pipe + Win SHM transports instead of UDS + POSIX SHM.
//
// Pure Go — no cgo. Works with CGO_ENABLED=0.

package cgroups

import (
	"encoding/binary"
	"errors"
	"sync/atomic"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	windows "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/windows"
)

// ---------------------------------------------------------------------------
//  Client context
// ---------------------------------------------------------------------------

// Client is an L2 client context for the cgroups snapshot service.
type Client struct {
	state       ClientState
	runDir      string
	serviceName string
	config      windows.ClientConfig

	session *windows.Session
	shm     *windows.WinShmContext

	connectCount   uint32
	reconnectCount uint32
	callCount      uint32
	errorCount     uint32
}

// NewClient creates a new client context. Does NOT connect.
func NewClient(runDir, serviceName string, config windows.ClientConfig) *Client {
	return &Client{
		state:       StateDisconnected,
		runDir:      runDir,
		serviceName: serviceName,
		config:      config,
	}
}

// Refresh attempts connect if DISCONNECTED/NOT_FOUND, reconnect if BROKEN.
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
func (c *Client) CallSnapshot(responseBuf []byte) (*protocol.CgroupsResponseView, error) {
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
		c.shm.WinShmClose()
		c.shm = nil
	}
	if c.session != nil {
		c.session.Close()
		c.session = nil
	}
}

func (c *Client) tryConnect() ClientState {
	session, err := windows.Connect(c.runDir, c.serviceName, &c.config)
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

	// Win SHM upgrade if negotiated
	if session.SelectedProfile == windows.WinShmProfileHybrid ||
		session.SelectedProfile == windows.WinShmProfileBusywait {
		for i := 0; i < 200; i++ {
			shm, serr := windows.WinShmClientAttach(
				c.runDir, c.serviceName,
				c.config.AuthToken,
				session.SelectedProfile,
			)
			if serr == nil {
				c.shm = shm
				break
			}
			time.Sleep(5 * time.Millisecond)
		}
	}

	c.session = session
	return StateReady
}

func (c *Client) doCgroupsCallRaw(responseBuf []byte) (int, error) {
	req := protocol.CgroupsRequest{LayoutVersion: 1, Flags: 0}
	var reqBuf [4]byte
	if req.Encode(reqBuf[:]) == 0 {
		return 0, protocol.ErrTruncated
	}

	hdr := protocol.Header{
		Kind:            protocol.KindRequest,
		Code:            protocol.MethodCgroupsSnapshot,
		Flags:           0,
		ItemCount:       1,
		MessageID:       uint64(c.callCount) + 1,
		TransportStatus: protocol.StatusOK,
	}

	if err := c.transportSend(&hdr, reqBuf[:]); err != nil {
		return 0, err
	}

	respHdr, payloadLen, err := c.transportReceive(responseBuf)
	if err != nil {
		return 0, err
	}

	// Verify response envelope fields before decode
	if respHdr.Kind != protocol.KindResponse {
		return 0, protocol.ErrBadKind
	}
	if respHdr.Code != protocol.MethodCgroupsSnapshot {
		return 0, protocol.ErrBadLayout
	}
	if respHdr.MessageID != hdr.MessageID {
		return 0, protocol.ErrBadLayout
	}

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

		return c.shm.WinShmSend(msg)
	}

	if c.session == nil {
		return protocol.ErrTruncated
	}
	return c.session.Send(hdr, payload)
}

func (c *Client) transportReceive(responseBuf []byte) (protocol.Header, int, error) {
	if c.shm != nil {
		shmBuf := make([]byte, len(responseBuf)+protocol.HeaderSize)
		mlen, err := c.shm.WinShmReceive(shmBuf, 30000)
		if err != nil {
			return protocol.Header{}, 0, protocol.ErrTruncated
		}
		if mlen < protocol.HeaderSize {
			return protocol.Header{}, 0, protocol.ErrTruncated
		}

		hdr, err := protocol.DecodeHeader(shmBuf[:mlen])
		if err != nil {
			return protocol.Header{}, 0, err
		}

		payloadLen := mlen - protocol.HeaderSize
		if payloadLen > len(responseBuf) {
			return protocol.Header{}, 0, protocol.ErrOverflow
		}
		copy(responseBuf[:payloadLen], shmBuf[protocol.HeaderSize:mlen])
		return hdr, payloadLen, nil
	}

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
	return errors.Is(err, windows.ErrConnect) || errors.Is(err, windows.ErrCreatePipe)
}

func isAuthError(err error) bool {
	return errors.Is(err, windows.ErrAuthFailed)
}

func isProfileError(err error) bool {
	return errors.Is(err, windows.ErrNoProfile)
}

// ---------------------------------------------------------------------------
//  Managed server
// ---------------------------------------------------------------------------

// Server is an L2 managed server for the cgroups snapshot service.
type Server struct {
	runDir      string
	serviceName string
	config      windows.ServerConfig
	handler     HandlerFunc
	running     atomic.Bool
}

// NewServer creates a new managed server.
func NewServer(runDir, serviceName string, config windows.ServerConfig, handler HandlerFunc) *Server {
	return &Server{
		runDir:      runDir,
		serviceName: serviceName,
		config:      config,
		handler:     handler,
	}
}

// Run starts the acceptor loop. Blocking.
func (s *Server) Run() error {
	listener, err := windows.Listen(s.runDir, s.serviceName, s.config)
	if err != nil {
		return err
	}
	defer listener.Close()

	s.running.Store(true)

	for s.running.Load() {
		session, err := listener.Accept()
		if err != nil {
			if !s.running.Load() {
				break
			}
			time.Sleep(10 * time.Millisecond)
			continue
		}

		// Win SHM upgrade if negotiated
		var shm *windows.WinShmContext
		if session.SelectedProfile == windows.WinShmProfileHybrid ||
			session.SelectedProfile == windows.WinShmProfileBusywait {
			shmCtx, serr := windows.WinShmServerCreate(
				s.runDir, s.serviceName,
				s.config.AuthToken,
				session.SelectedProfile,
				session.MaxRequestPayloadBytes+uint32(protocol.HeaderSize),
				session.MaxResponsePayloadBytes+uint32(protocol.HeaderSize),
			)
			if serr == nil {
				shm = shmCtx
			}
		}

		s.handleSession(session, shm)
	}

	return nil
}

// Stop signals the server to stop.
func (s *Server) Stop() {
	s.running.Store(false)
}

func (s *Server) handleSession(session *windows.Session, shm *windows.WinShmContext) {
	recvBuf := make([]byte, 65536)

	defer func() {
		if shm != nil {
			shm.WinShmDestroy()
		}
		session.Close()
	}()

	for s.running.Load() {
		var hdr protocol.Header
		var payload []byte

		if shm != nil {
			mlen, err := shm.WinShmReceive(recvBuf, 500)
			if err != nil {
				if err == windows.ErrWinShmTimeout {
					continue
				}
				return
			}
			if mlen < protocol.HeaderSize {
				return
			}
			h, err := protocol.DecodeHeader(recvBuf[:mlen])
			if err != nil {
				return
			}
			hdr = h
			payload = make([]byte, mlen-protocol.HeaderSize)
			copy(payload, recvBuf[protocol.HeaderSize:mlen])
		} else {
			// Named Pipe path
			h, p, err := session.Receive(recvBuf)
			if err != nil {
				return
			}
			hdr = h
			payload = make([]byte, len(p))
			copy(payload, p)
		}

		// Protocol violation: unexpected message kind terminates session
		if hdr.Kind != protocol.KindRequest {
			return
		}

		respPayload, ok := s.handler(hdr.Code, payload)

		respHdr := protocol.Header{
			Kind:      protocol.KindResponse,
			Code:      hdr.Code,
			MessageID: hdr.MessageID,
			ItemCount: 1,
		}

		if ok {
			respHdr.TransportStatus = protocol.StatusOK
		} else {
			respHdr.TransportStatus = protocol.StatusInternalError
			respPayload = nil
		}

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

			if err := shm.WinShmSend(msg); err != nil {
				return
			}
		} else {
			if err := session.Send(&respHdr, respPayload); err != nil {
				return
			}
		}
	}
}

// Suppress unused import warnings.
var _ = binary.LittleEndian
