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

	requestBuf   []byte
	sendBuf      []byte
	transportBuf []byte

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

// callWithRetry runs attempt once; on failure disconnects, reconnects,
// retries once. Manages state transitions and counters.
func (c *Client) callWithRetry(attempt func() error) error {
	if c.state != StateReady {
		c.errorCount++
		return protocol.ErrBadLayout
	}

	// First attempt
	firstErr := attempt()
	if firstErr == nil {
		c.callCount++
		return nil
	}

	// Call failed. Disconnect, reconnect, retry ONCE.
	c.disconnect()
	c.state = StateBroken

	c.state = c.tryConnect()
	if c.state != StateReady {
		c.errorCount++
		return firstErr
	}
	c.reconnectCount++

	// Retry once
	retryErr := attempt()
	if retryErr == nil {
		c.callCount++
		return nil
	}

	// Retry also failed
	c.disconnect()
	c.state = StateBroken
	c.errorCount++
	return retryErr
}

// doRawCall sends a request and receives/validates the response envelope.
// Returns the validated response header and a borrowed payload view.
func (c *Client) doRawCall(methodCode uint16, reqPayload []byte) (protocol.Header, []byte, error) {
	hdr := protocol.Header{
		Kind:            protocol.KindRequest,
		Code:            methodCode,
		Flags:           0,
		ItemCount:       1,
		MessageID:       uint64(c.callCount) + 1,
		TransportStatus: protocol.StatusOK,
	}

	if err := c.transportSend(&hdr, reqPayload); err != nil {
		return protocol.Header{}, nil, err
	}

	respHdr, payload, err := c.transportReceive()
	if err != nil {
		return protocol.Header{}, nil, err
	}

	if respHdr.Kind != protocol.KindResponse {
		return protocol.Header{}, nil, protocol.ErrBadKind
	}
	if respHdr.Code != methodCode {
		return protocol.Header{}, nil, protocol.ErrBadLayout
	}
	if respHdr.MessageID != hdr.MessageID {
		return protocol.Header{}, nil, protocol.ErrBadLayout
	}
	if respHdr.TransportStatus != protocol.StatusOK {
		return protocol.Header{}, nil, protocol.ErrBadLayout
	}

	return respHdr, payload, nil
}

// CallSnapshot performs a blocking typed cgroups snapshot call.
func (c *Client) CallSnapshot() (*protocol.CgroupsResponseView, error) {
	var result *protocol.CgroupsResponseView

	err := c.callWithRetry(func() error {
		req := protocol.CgroupsRequest{LayoutVersion: 1, Flags: 0}
		var reqBuf [4]byte
		if req.Encode(reqBuf[:]) == 0 {
			return protocol.ErrTruncated
		}

		_, payload, rerr := c.doRawCall(protocol.MethodCgroupsSnapshot, reqBuf[:])
		if rerr != nil {
			return rerr
		}

		view, derr := protocol.DecodeCgroupsResponse(payload)
		if derr != nil {
			return derr
		}
		result = &view
		return nil
	})
	if err != nil {
		return nil, err
	}
	return result, nil
}

// CallIncrement performs a blocking INCREMENT call.
// Sends requestValue, returns the server's response value.
func (c *Client) CallIncrement(requestValue uint64) (uint64, error) {
	var result uint64

	err := c.callWithRetry(func() error {
		var reqBuf [protocol.IncrementPayloadSize]byte
		if protocol.IncrementEncode(requestValue, reqBuf[:]) == 0 {
			return protocol.ErrTruncated
		}

		_, payload, rerr := c.doRawCall(protocol.MethodIncrement, reqBuf[:])
		if rerr != nil {
			return rerr
		}

		val, derr := protocol.IncrementDecode(payload)
		if derr != nil {
			return derr
		}
		result = val
		return nil
	})
	return result, err
}

// CallStringReverse performs a blocking STRING_REVERSE call.
// Sends requestStr, returns the server's reversed string view.
func (c *Client) CallStringReverse(requestStr string) (*protocol.StringReverseView, error) {
	var result *protocol.StringReverseView

	err := c.callWithRetry(func() error {
		reqBuf := ensureClientScratch(&c.requestBuf, protocol.StringReverseHdrSize+len(requestStr)+1)
		if protocol.StringReverseEncode(requestStr, reqBuf) == 0 {
			return protocol.ErrTruncated
		}

		_, payload, rerr := c.doRawCall(protocol.MethodStringReverse, reqBuf)
		if rerr != nil {
			return rerr
		}

		view, derr := protocol.StringReverseDecode(payload)
		if derr != nil {
			return derr
		}
		result = &view
		return nil
	})
	if err != nil {
		return nil, err
	}
	return result, nil
}

// CallIncrementBatch performs a blocking batch INCREMENT call.
// Sends multiple values, returns the server's response values.
func (c *Client) CallIncrementBatch(values []uint64) ([]uint64, error) {
	if len(values) == 0 {
		return nil, nil
	}

	var results []uint64
	itemCount := uint32(len(values))

	err := c.callWithRetry(func() error {
		// Build batch request payload
		batchBufSize := protocol.Align8(int(itemCount)*8) + int(itemCount)*protocol.IncrementPayloadSize + int(itemCount)*protocol.Alignment
		batchBuf := ensureClientScratch(&c.requestBuf, batchBufSize)
		bb := protocol.NewBatchBuilder(batchBuf, itemCount)

		for _, v := range values {
			var item [protocol.IncrementPayloadSize]byte
			if protocol.IncrementEncode(v, item[:]) == 0 {
				return protocol.ErrTruncated
			}
			if err := bb.Add(item[:]); err != nil {
				return err
			}
		}

		totalPayloadLen, _ := bb.Finish()
		reqPayload := batchBuf[:totalPayloadLen]

		// Build and send header with batch flags
		hdr := protocol.Header{
			Kind:            protocol.KindRequest,
			Code:            protocol.MethodIncrement,
			Flags:           protocol.FlagBatch,
			ItemCount:       itemCount,
			MessageID:       uint64(c.callCount) + 1,
			TransportStatus: protocol.StatusOK,
		}

		if err := c.transportSend(&hdr, reqPayload); err != nil {
			return err
		}

		// Receive response
		respHdr, respPayload, err := c.transportReceive()
		if err != nil {
			return err
		}

		if respHdr.Kind != protocol.KindResponse {
			return protocol.ErrBadKind
		}
		if respHdr.Code != protocol.MethodIncrement {
			return protocol.ErrBadLayout
		}
		if respHdr.MessageID != hdr.MessageID {
			return protocol.ErrBadLayout
		}
		if respHdr.TransportStatus != protocol.StatusOK {
			return protocol.ErrBadLayout
		}
		if respHdr.Flags&protocol.FlagBatch == 0 || respHdr.ItemCount != itemCount {
			return protocol.ErrBadItemCount
		}

		// Extract each response item
		out := make([]uint64, itemCount)
		for i := uint32(0); i < itemCount; i++ {
			itemData, gerr := protocol.BatchItemGet(respPayload, itemCount, i)
			if gerr != nil {
				return gerr
			}
			val, derr := protocol.IncrementDecode(itemData)
			if derr != nil {
				return derr
			}
			out[i] = val
		}
		results = out
		return nil
	})
	return results, err
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
				session.SessionID,
				session.SelectedProfile,
			)
			if serr == nil {
				c.shm = shm
				break
			}
			time.Sleep(5 * time.Millisecond)
		}
		if c.shm == nil {
			// SHM attach failed. Fail the session to avoid transport desync.
			session.Close()
			return StateDisconnected
		}
	}

	c.session = session
	return StateReady
}

func (c *Client) transportSend(hdr *protocol.Header, payload []byte) error {
	if c.shm != nil {
		msgLen := protocol.HeaderSize + len(payload)
		msg := ensureClientScratch(&c.sendBuf, msgLen)

		hdr.Magic = protocol.MagicMsg
		hdr.Version = protocol.Version
		hdr.HeaderLen = protocol.HeaderLen
		hdr.PayloadLen = uint32(len(payload))

		hdr.Encode(msg[:protocol.HeaderSize])
		if len(payload) > 0 {
			copy(msg[protocol.HeaderSize:], payload)
		}

		return c.shm.WinShmSend(msg[:msgLen])
	}

	if c.session == nil {
		return protocol.ErrTruncated
	}
	return c.session.Send(hdr, payload)
}

func (c *Client) transportReceive() (protocol.Header, []byte, error) {
	scratch := ensureClientScratch(&c.transportBuf, c.maxReceiveMessageBytes())

	if c.shm != nil {
		mlen, err := c.shm.WinShmReceive(scratch, 30000)
		if err != nil {
			return protocol.Header{}, nil, protocol.ErrTruncated
		}
		if mlen < protocol.HeaderSize {
			return protocol.Header{}, nil, protocol.ErrTruncated
		}

		hdr, err := protocol.DecodeHeader(scratch[:mlen])
		if err != nil {
			return protocol.Header{}, nil, err
		}
		return hdr, scratch[protocol.HeaderSize:mlen], nil
	}

	if c.session == nil {
		return protocol.Header{}, nil, protocol.ErrTruncated
	}

	hdr, payload, err := c.session.Receive(scratch)
	if err != nil {
		return protocol.Header{}, nil, protocol.ErrTruncated
	}
	return hdr, payload, nil
}

func (c *Client) maxReceiveMessageBytes() int {
	maxPayload := c.config.MaxResponsePayloadBytes
	if c.session != nil && c.session.MaxResponsePayloadBytes > 0 {
		maxPayload = c.session.MaxResponsePayloadBytes
	}
	if maxPayload == 0 {
		maxPayload = cacheResponseBufSize
	}
	return protocol.HeaderSize + int(maxPayload)
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
	handlers    Handlers
	running     atomic.Bool
	listener    *windows.Listener // stored so Stop() can close it
}

// NewServer creates a new managed server.
func NewServer(runDir, serviceName string, config windows.ServerConfig, handlers Handlers) *Server {
	return &Server{
		runDir:      runDir,
		serviceName: serviceName,
		config:      config,
		handlers:    handlers,
	}
}

func (s *Server) dispatchSingle(methodCode uint16, request []byte, responseBuf []byte) (int, bool) {
	switch methodCode {
	case protocol.MethodIncrement:
		if s.handlers.OnIncrement == nil {
			return 0, false
		}
		return protocol.DispatchIncrement(request, responseBuf, s.handlers.OnIncrement)

	case protocol.MethodStringReverse:
		if s.handlers.OnStringReverse == nil {
			return 0, false
		}
		return protocol.DispatchStringReverse(request, responseBuf, s.handlers.OnStringReverse)

	case protocol.MethodCgroupsSnapshot:
		if s.handlers.OnSnapshot == nil {
			return 0, false
		}
		maxItems := s.handlers.snapshotMaxItems(len(responseBuf))
		if maxItems == 0 {
			return 0, false
		}
		return protocol.DispatchCgroupsSnapshot(request, responseBuf, maxItems, s.handlers.OnSnapshot)

	default:
		return 0, false
	}
}

// Run starts the acceptor loop. Blocking.
func (s *Server) Run() error {
	listener, err := windows.Listen(s.runDir, s.serviceName, s.config)
	if err != nil {
		return err
	}
	s.listener = listener
	defer func() {
		listener.Close()
		s.listener = nil
	}()

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
				session.SessionID,
				session.SelectedProfile,
				session.MaxRequestPayloadBytes+uint32(protocol.HeaderSize),
				session.MaxResponsePayloadBytes+uint32(protocol.HeaderSize),
			)
			if serr != nil {
				// SHM create failed for negotiated SHM — reject session
				session.Close()
				continue
			}
			shm = shmCtx
		}

		s.handleSession(session, shm)
	}

	return nil
}

// Stop signals the server to stop and unblocks Accept by closing the listener.
func (s *Server) Stop() {
	s.running.Store(false)
	if s.listener != nil {
		s.listener.Close()
	}
}

func (s *Server) handleSession(session *windows.Session, shm *windows.WinShmContext) {
	recvBuf := make([]byte, protocol.HeaderSize+int(session.MaxRequestPayloadBytes))
	respBuf := make([]byte, int(session.MaxResponsePayloadBytes))
	itemRespBuf := make([]byte, int(session.MaxResponsePayloadBytes))
	msgBuf := make([]byte, int(session.MaxResponsePayloadBytes)+protocol.HeaderSize)

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
			mlen, err := shm.WinShmReceive(recvBuf, serverPollTimeoutMs)
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
			payload = recvBuf[protocol.HeaderSize:mlen]
		} else {
			// Named Pipe path
			h, p, err := session.Receive(recvBuf)
			if err != nil {
				return
			}
			hdr = h
			payload = p
		}

		// Protocol violation: unexpected message kind terminates session
		if hdr.Kind != protocol.KindRequest {
			return
		}

		// Dispatch: single-item or batch
		responseLen := 0
		ok := true
		isBatch := (hdr.Flags&protocol.FlagBatch != 0) && hdr.ItemCount >= 1

		if !isBatch {
			// Single-item dispatch
			responseLen, ok = s.dispatchSingle(hdr.Code, payload, respBuf)
			if responseLen < 0 || responseLen > len(respBuf) {
				ok = false
				responseLen = 0
			}
		} else {
			// Batch dispatch: extract each item, call handler per item,
			// reassemble responses using batch builder.
			bb := protocol.NewBatchBuilder(respBuf, hdr.ItemCount)

			for i := uint32(0); i < hdr.ItemCount && ok; i++ {
				itemData, gerr := protocol.BatchItemGet(payload, hdr.ItemCount, i)
				if gerr != nil {
					ok = false
					break
				}

				itemResultLen, handlerOk := s.dispatchSingle(hdr.Code, itemData, itemRespBuf)
				if !handlerOk || itemResultLen < 0 || itemResultLen > len(itemRespBuf) {
					ok = false
					break
				}

				if aerr := bb.Add(itemRespBuf[:itemResultLen]); aerr != nil {
					ok = false
					break
				}
			}

			if ok {
				responseLen, _ = bb.Finish()
			}
		}

		// Build response header
		respHdr := protocol.Header{
			Kind:      protocol.KindResponse,
			Code:      hdr.Code,
			MessageID: hdr.MessageID,
		}

		if ok {
			respHdr.TransportStatus = protocol.StatusOK
			if isBatch {
				respHdr.Flags = protocol.FlagBatch
				respHdr.ItemCount = hdr.ItemCount
			} else {
				respHdr.ItemCount = 1
			}
		} else {
			// Handler/batch failure: INTERNAL_ERROR + empty payload
			respHdr.TransportStatus = protocol.StatusInternalError
			respHdr.ItemCount = 1
			responseLen = 0
		}

		if shm != nil {
			msgLen := protocol.HeaderSize + responseLen
			if len(msgBuf) < msgLen {
				msgBuf = make([]byte, msgLen)
			}
			msg := msgBuf[:msgLen]

			respHdr.Magic = protocol.MagicMsg
			respHdr.Version = protocol.Version
			respHdr.HeaderLen = protocol.HeaderLen
			respHdr.PayloadLen = uint32(responseLen)

			respHdr.Encode(msg[:protocol.HeaderSize])
			if responseLen > 0 {
				copy(msg[protocol.HeaderSize:], respBuf[:responseLen])
			}

			if err := shm.WinShmSend(msg); err != nil {
				return
			}
		} else {
			if err := session.Send(&respHdr, respBuf[:responseLen]); err != nil {
				return
			}
		}
	}
}

// Suppress unused import warnings.
var _ = binary.NativeEndian
