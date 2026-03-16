//go:build unix

package cgroups

import (
	"encoding/binary"
	"errors"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
	"github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix"
)

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

// callWithRetry runs attempt once; on failure disconnects, reconnects,
// retries once. Manages state transitions and counters.
func (c *Client) callWithRetry(attempt func() error) error {
	// Fail fast if not READY
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
// Returns the validated response header and payload length in responseBuf.
func (c *Client) doRawCall(methodCode uint16, reqPayload, responseBuf []byte) (protocol.Header, int, error) {
	hdr := protocol.Header{
		Kind:            protocol.KindRequest,
		Code:            methodCode,
		Flags:           0,
		ItemCount:       1,
		MessageID:       uint64(c.callCount) + 1,
		TransportStatus: protocol.StatusOK,
	}

	if err := c.transportSend(&hdr, reqPayload); err != nil {
		return protocol.Header{}, 0, err
	}

	respHdr, payloadLen, err := c.transportReceive(responseBuf)
	if err != nil {
		return protocol.Header{}, 0, err
	}

	if respHdr.Kind != protocol.KindResponse {
		return protocol.Header{}, 0, protocol.ErrBadKind
	}
	if respHdr.Code != methodCode {
		return protocol.Header{}, 0, protocol.ErrBadLayout
	}
	if respHdr.MessageID != hdr.MessageID {
		return protocol.Header{}, 0, protocol.ErrBadLayout
	}
	if respHdr.TransportStatus != protocol.StatusOK {
		return protocol.Header{}, 0, protocol.ErrBadLayout
	}

	return respHdr, payloadLen, nil
}

// CallSnapshot performs a blocking typed cgroups snapshot call.
//
// responseBuf must be large enough for the expected snapshot.
//
// Retry policy (per spec): if the call fails and the context was
// previously READY, disconnect, reconnect (full handshake), retry ONCE.
func (c *Client) CallSnapshot(responseBuf []byte) (*protocol.CgroupsResponseView, error) {
	var result *protocol.CgroupsResponseView

	err := c.callWithRetry(func() error {
		// Encode request
		req := protocol.CgroupsRequest{LayoutVersion: 1, Flags: 0}
		var reqBuf [4]byte
		if req.Encode(reqBuf[:]) == 0 {
			return protocol.ErrTruncated
		}

		_, payloadLen, rerr := c.doRawCall(protocol.MethodCgroupsSnapshot, reqBuf[:], responseBuf)
		if rerr != nil {
			return rerr
		}

		view, derr := protocol.DecodeCgroupsResponse(responseBuf[:payloadLen])
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
func (c *Client) CallIncrement(requestValue uint64, responseBuf []byte) (uint64, error) {
	var result uint64

	err := c.callWithRetry(func() error {
		var reqBuf [protocol.IncrementPayloadSize]byte
		if protocol.IncrementEncode(requestValue, reqBuf[:]) == 0 {
			return protocol.ErrTruncated
		}

		_, payloadLen, rerr := c.doRawCall(protocol.MethodIncrement, reqBuf[:], responseBuf)
		if rerr != nil {
			return rerr
		}

		val, derr := protocol.IncrementDecode(responseBuf[:payloadLen])
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
func (c *Client) CallStringReverse(requestStr string, responseBuf []byte) (*protocol.StringReverseView, error) {
	var result *protocol.StringReverseView

	err := c.callWithRetry(func() error {
		reqBuf := make([]byte, protocol.StringReverseHdrSize+len(requestStr)+1)
		if protocol.StringReverseEncode(requestStr, reqBuf) == 0 {
			return protocol.ErrTruncated
		}

		_, payloadLen, rerr := c.doRawCall(protocol.MethodStringReverse, reqBuf, responseBuf)
		if rerr != nil {
			return rerr
		}

		view, derr := protocol.StringReverseDecode(responseBuf[:payloadLen])
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
func (c *Client) CallIncrementBatch(values []uint64, responseBuf []byte) ([]uint64, error) {
	if len(values) == 0 {
		return nil, nil
	}

	var results []uint64
	itemCount := uint32(len(values))

	err := c.callWithRetry(func() error {
		// Build batch request payload
		batchBufSize := protocol.Align8(int(itemCount)*8) + int(itemCount)*protocol.IncrementPayloadSize + int(itemCount)*protocol.Alignment
		batchBuf := make([]byte, batchBufSize)
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
		respHdr, payloadLen, err := c.transportReceive(responseBuf)
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
		respPayload := responseBuf[:payloadLen]
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
			shm, serr := posix.ShmClientAttach(c.runDir, c.serviceName, session.SessionID)
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
		shmBuf := make([]byte, len(responseBuf)+protocol.HeaderSize)
		mlen, err := c.shm.ShmReceive(shmBuf, 30000)
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

// Server is an L2 managed server for the cgroups snapshot service.
// Supports multiple concurrent client sessions up to workerCount.
type Server struct {
	runDir      string
	serviceName string
	config      posix.ServerConfig
	handler     HandlerFunc
	running     atomic.Bool
	workerCount int
	wg          sync.WaitGroup
}

// NewServer creates a new managed server. workerCount limits the
// maximum number of concurrent client sessions (default 1 if <= 0).
func NewServer(runDir, serviceName string, config posix.ServerConfig, handler HandlerFunc) *Server {
	return NewServerWithWorkers(runDir, serviceName, config, handler, 8)
}

// NewServerWithWorkers creates a server with an explicit worker count limit.
func NewServerWithWorkers(runDir, serviceName string, config posix.ServerConfig,
	handler HandlerFunc, workerCount int) *Server {
	if workerCount < 1 {
		workerCount = 1
	}
	return &Server{
		runDir:      runDir,
		serviceName: serviceName,
		config:      config,
		handler:     handler,
		workerCount: workerCount,
	}
}

// Run starts the acceptor loop. Blocking. Accepts clients, spawns a
// goroutine per session (up to workerCount concurrently).
// Returns when Stop() is called or on fatal error.
func (s *Server) Run() error {
	posix.ShmCleanupStale(s.runDir, s.serviceName)

	listener, err := posix.Listen(s.runDir, s.serviceName, s.config)
	if err != nil {
		return err
	}
	defer listener.Close()

	s.running.Store(true)

	/* Semaphore channel limits concurrent sessions */
	sem := make(chan struct{}, s.workerCount)

	for s.running.Load() {
		// Poll the listener fd before blocking on accept
		ready := pollFd(listener.Fd(), serverPollTimeoutMs)
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

		// Try to acquire a worker slot (non-blocking check)
		select {
		case sem <- struct{}{}:
			// Got a slot
		default:
			// At capacity: reject client
			session.Close()
			continue
		}

		// SHM upgrade if negotiated
		var shm *posix.ShmContext
		if session.SelectedProfile == protocol.ProfileSHMHybrid ||
			session.SelectedProfile == protocol.ProfileSHMFutex {
			shmCtx, serr := posix.ShmServerCreate(
				s.runDir, s.serviceName, session.SessionID,
				session.MaxRequestPayloadBytes+uint32(protocol.HeaderSize),
				session.MaxResponsePayloadBytes+uint32(protocol.HeaderSize),
			)
			if serr != nil {
				// SHM create failed for negotiated SHM — reject session
				session.Close()
				<-sem // release worker slot
				continue
			}
			shm = shmCtx
		}

		// Handle this session in a goroutine
		s.wg.Add(1)
		go func(sess *posix.Session, shmCtx *posix.ShmContext) {
			defer func() {
				<-sem // release worker slot
				s.wg.Done()
			}()
			s.handleSession(sess, shmCtx)
		}(session, shm)
	}

	// Wait for all active session goroutines to finish
	s.wg.Wait()

	return nil
}

// Stop signals the server to stop.
func (s *Server) Stop() {
	s.running.Store(false)
}

func (s *Server) handleSession(session *posix.Session, shm *posix.ShmContext) {
	recvBuf := make([]byte, protocol.HeaderSize+int(session.MaxRequestPayloadBytes))

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
			mlen, err := shm.ShmReceive(recvBuf, serverPollTimeoutMs)
			if err != nil {
				if err == posix.ErrShmTimeout {
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
			// Copy payload from local buffer
			payload = make([]byte, mlen-protocol.HeaderSize)
			copy(payload, recvBuf[protocol.HeaderSize:mlen])
		} else {
			// Poll the session fd before blocking on receive
			ready := pollFd(session.Fd(), serverPollTimeoutMs)
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

		// Protocol violation: unexpected message kind terminates session
		if hdr.Kind != protocol.KindRequest {
			return
		}

		// Dispatch: single-item or batch
		var respPayload []byte
		ok := true
		isBatch := (hdr.Flags&protocol.FlagBatch != 0) && hdr.ItemCount >= 1

		if !isBatch {
			// Single-item dispatch
			respPayload, ok = s.handler(hdr.Code, payload)
		} else {
			// Batch dispatch: extract each item, call handler per item,
			// reassemble responses using batch builder.
			batchBufSize := int(session.MaxResponsePayloadBytes)
			batchBuf := make([]byte, batchBufSize)
			bb := protocol.NewBatchBuilder(batchBuf, hdr.ItemCount)

			// Per-item response scratch buffer
			itemRespSize := batchBufSize / 2
			itemResp := make([]byte, itemRespSize)

			for i := uint32(0); i < hdr.ItemCount && ok; i++ {
				itemData, gerr := protocol.BatchItemGet(payload, hdr.ItemCount, i)
				if gerr != nil {
					ok = false
					break
				}

				itemResult, handlerOk := s.handler(hdr.Code, itemData)
				if !handlerOk {
					ok = false
					break
				}

				// Copy handler result into scratch (for safety), then add to builder
				if len(itemResult) > itemRespSize {
					ok = false
					break
				}
				copy(itemResp, itemResult)

				if aerr := bb.Add(itemResp[:len(itemResult)]); aerr != nil {
					ok = false
					break
				}
			}

			if ok {
				totalLen, _ := bb.Finish()
				respPayload = batchBuf[:totalLen]
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
var _ = binary.NativeEndian
