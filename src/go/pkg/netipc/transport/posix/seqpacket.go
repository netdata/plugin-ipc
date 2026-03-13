//go:build unix

package posix

import (
	"encoding/binary"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"syscall"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

const (
	ProfileUDSSeqpacket uint32 = 1 << 0
	ProfileSHMHybrid    uint32 = 1 << 1
	ProfileSHMFutex     uint32 = 1 << 2

	DefaultSupportedProfiles        = ProfileUDSSeqpacket
	DefaultPreferredProfiles        = ProfileUDSSeqpacket
	negMagic                 uint32 = 0x4e48534b
	negVersion               uint16 = 1
	negHello                 uint16 = 1
	negAck                   uint16 = 2
	negStatusOK              uint32 = 0
	negPayloadOffset                = 8
	negStatusOffset                 = 48
	negDefaultBatchItems     uint32 = 1
)

var implementedProfiles = platformImplementedProfiles

const (
	negOffMagic   = 0
	negOffVersion = 4
	negOffType    = 6
)

type Config struct {
	RunDir            string
	ServiceName       string
	FileMode          os.FileMode
	SupportedProfiles uint32
	PreferredProfiles uint32
	MaxRequestPayloadBytes  uint32
	MaxRequestBatchItems    uint32
	MaxResponsePayloadBytes uint32
	MaxResponseBatchItems   uint32
	AuthToken         uint64
}

func NewConfig(runDir, serviceName string) Config {
	return Config{
		RunDir:            runDir,
		ServiceName:       serviceName,
		FileMode:          0o600,
		SupportedProfiles: DefaultSupportedProfiles,
		PreferredProfiles: DefaultPreferredProfiles,
		MaxRequestPayloadBytes:  protocol.MaxPayloadDefault,
		MaxRequestBatchItems:    negDefaultBatchItems,
		MaxResponsePayloadBytes: protocol.MaxPayloadDefault,
		MaxResponseBatchItems:   negDefaultBatchItems,
	}
}

type Server struct {
	listener              *net.UnixListener
	conn                  *net.UnixConn
	shm                   *shmServer
	path                  string
	supportedProfiles     uint32
	preferredProfiles     uint32
	maxRequestPayloadBytes  uint32
	maxRequestBatchItems    uint32
	maxResponsePayloadBytes uint32
	maxResponseBatchItems   uint32
	authToken             uint64
	negotiatedProfile     uint32
	maxRequestMessageLen  int
	maxResponseMessageLen int
}

type Client struct {
	conn                  *net.UnixConn
	shm                   *shmClient
	supportedProfiles     uint32
	preferredProfiles     uint32
	maxRequestPayloadBytes  uint32
	maxRequestBatchItems    uint32
	maxResponsePayloadBytes uint32
	maxResponseBatchItems   uint32
	authToken             uint64
	negotiatedProfile     uint32
	maxRequestMessageLen  int
	maxResponseMessageLen int
	nextRequestID         uint64
}

type negotiationResult struct {
	profile               uint32
	maxRequestMessageLen  int
	maxResponseMessageLen int
}

func endpointSockPath(runDir, service string) string {
	return filepath.Join(runDir, service+".sock")
}

func effectiveSupportedProfiles(config Config) uint32 {
	supported := config.SupportedProfiles
	if supported == 0 {
		supported = DefaultSupportedProfiles
	}
	supported &= implementedProfiles
	if supported == 0 {
		supported = DefaultSupportedProfiles
	}
	return supported
}

func effectivePreferredProfiles(config Config, supported uint32) uint32 {
	preferred := config.PreferredProfiles
	if preferred == 0 {
		preferred = supported
	}
	preferred &= supported
	if preferred == 0 {
		preferred = supported
	}
	return preferred
}

func selectProfile(mask uint32) uint32 {
	if (mask & ProfileUDSSeqpacket) != 0 {
		return ProfileUDSSeqpacket
	}
	if (mask & ProfileSHMHybrid) != 0 {
		return ProfileSHMHybrid
	}
	if (mask & ProfileSHMFutex) != 0 {
		return ProfileSHMFutex
	}
	return 0
}

func negotiateLimit(offered, local uint32) uint32 {
	if offered == 0 || local == 0 {
		return 0
	}
	if offered < local {
		return offered
	}
	return local
}

func effectivePayloadLimit(value uint32) uint32 {
	if value == 0 {
		return protocol.MaxPayloadDefault
	}
	return value
}

func effectiveBatchLimit(value uint32) uint32 {
	if value == 0 {
		return negDefaultBatchItems
	}
	return value
}

func computeMaxMessageLen(maxPayloadBytes, maxBatchItems uint32) (int, error) {
	total, err := protocol.MaxBatchTotalSize(maxPayloadBytes, maxBatchItems)
	if err != nil {
		return 0, err
	}
	return total, nil
}

func encodeNegHeader(typ uint16) protocol.Frame {
	var frame protocol.Frame
	binary.LittleEndian.PutUint32(frame[negOffMagic:negOffMagic+4], negMagic)
	binary.LittleEndian.PutUint16(frame[negOffVersion:negOffVersion+2], negVersion)
	binary.LittleEndian.PutUint16(frame[negOffType:negOffType+2], typ)
	return frame
}

func decodeNegHeader(frame protocol.Frame, expectedType uint16) error {
	magic := binary.LittleEndian.Uint32(frame[negOffMagic : negOffMagic+4])
	version := binary.LittleEndian.Uint16(frame[negOffVersion : negOffVersion+2])
	typ := binary.LittleEndian.Uint16(frame[negOffType : negOffType+2])
	if magic != negMagic || version != negVersion || typ != expectedType {
		return errors.New("invalid negotiation frame")
	}
	return nil
}

func encodeHelloNeg(payload protocol.HelloPayload) protocol.Frame {
	frame := encodeNegHeader(negHello)
	hello := protocol.EncodeHelloPayload(payload)
	copy(frame[negPayloadOffset:negPayloadOffset+protocol.ControlHelloPayloadLen], hello[:])
	return frame
}

func decodeHelloNeg(frame protocol.Frame) (protocol.HelloPayload, error) {
	if err := decodeNegHeader(frame, negHello); err != nil {
		return protocol.HelloPayload{}, err
	}
	return protocol.DecodeHelloPayload(frame[negPayloadOffset : negPayloadOffset+protocol.ControlHelloPayloadLen])
}

func encodeAckNeg(payload protocol.HelloAckPayload, status uint32) protocol.Frame {
	frame := encodeNegHeader(negAck)
	ack := protocol.EncodeHelloAckPayload(payload)
	copy(frame[negPayloadOffset:negPayloadOffset+protocol.ControlHelloAckPayloadLen], ack[:])
	binary.LittleEndian.PutUint32(frame[negStatusOffset:negStatusOffset+4], status)
	return frame
}

func decodeAckNeg(frame protocol.Frame) (protocol.HelloAckPayload, uint32, error) {
	if err := decodeNegHeader(frame, negAck); err != nil {
		return protocol.HelloAckPayload{}, 0, err
	}
	payload, err := protocol.DecodeHelloAckPayload(frame[negPayloadOffset : negPayloadOffset+protocol.ControlHelloAckPayloadLen])
	if err != nil {
		return protocol.HelloAckPayload{}, 0, err
	}
	status := binary.LittleEndian.Uint32(frame[negStatusOffset : negStatusOffset+4])
	return payload, status, nil
}

func readFrame(conn *net.UnixConn, timeout time.Duration) (protocol.Frame, error) {
	var frame protocol.Frame
	if timeout > 0 {
		_ = conn.SetReadDeadline(time.Now().Add(timeout))
	} else {
		_ = conn.SetReadDeadline(time.Time{})
	}
	n, err := conn.Read(frame[:])
	if err != nil {
		return frame, err
	}
	if n != protocol.FrameSize {
		return frame, fmt.Errorf("short frame read: %d", n)
	}
	return frame, nil
}

func readMessage(conn *net.UnixConn, buf []byte, timeout time.Duration) (int, error) {
	if len(buf) == 0 {
		return 0, fmt.Errorf("buffer must not be empty")
	}
	if timeout > 0 {
		_ = conn.SetReadDeadline(time.Now().Add(timeout))
	} else {
		_ = conn.SetReadDeadline(time.Time{})
	}
	n, _, flags, _, err := conn.ReadMsgUnix(buf, nil)
	if err != nil {
		return 0, err
	}
	if (flags & syscall.MSG_TRUNC) != 0 {
		return 0, fmt.Errorf("message exceeds negotiated size")
	}
	return n, nil
}

func writeFrame(conn *net.UnixConn, frame protocol.Frame, timeout time.Duration) error {
	if timeout > 0 {
		_ = conn.SetWriteDeadline(time.Now().Add(timeout))
	} else {
		_ = conn.SetWriteDeadline(time.Time{})
	}
	n, err := conn.Write(frame[:])
	if err != nil {
		return err
	}
	if n != protocol.FrameSize {
		return fmt.Errorf("short frame write: %d", n)
	}
	return nil
}

func writeMessage(conn *net.UnixConn, message []byte, timeout time.Duration) error {
	if len(message) == 0 {
		return fmt.Errorf("message must not be empty")
	}
	if timeout > 0 {
		_ = conn.SetWriteDeadline(time.Now().Add(timeout))
	} else {
		_ = conn.SetWriteDeadline(time.Time{})
	}
	n, err := conn.Write(message)
	if err != nil {
		return err
	}
	if n != len(message) {
		return fmt.Errorf("short message write: %d", n)
	}
	return nil
}

func validateMessageForSend(message []byte, maxMessageLen int) error {
	if len(message) == 0 {
		return fmt.Errorf("message must not be empty")
	}
	if len(message) > maxMessageLen {
		return fmt.Errorf("message exceeds negotiated size")
	}
	header, err := protocol.DecodeMessageHeader(message)
	if err != nil {
		return err
	}
	total, err := protocol.MessageTotalSize(header)
	if err != nil {
		return err
	}
	if total != len(message) {
		return fmt.Errorf("message size does not match header")
	}
	return nil
}

func validateReceivedMessage(message []byte, messageLen int, maxMessageLen int) error {
	if messageLen == 0 {
		return fmt.Errorf("message must not be empty")
	}
	if messageLen > maxMessageLen {
		return fmt.Errorf("message exceeds negotiated size")
	}
	header, err := protocol.DecodeMessageHeader(message[:messageLen])
	if err != nil {
		return err
	}
	total, err := protocol.MessageTotalSize(header)
	if err != nil {
		return err
	}
	if total != messageLen {
		return fmt.Errorf("message size does not match header")
	}
	return nil
}

func tryTakeoverStaleSocket(path string) (bool, error) {
	addr := &net.UnixAddr{Name: path, Net: "unixpacket"}
	conn, err := net.DialUnix("unixpacket", nil, addr)
	if err == nil {
		_ = conn.Close()
		return false, os.ErrExist
	}

	if errors.Is(err, os.ErrNotExist) {
		return true, nil
	}

	var opErr *net.OpError
	if errors.As(err, &opErr) {
		switch {
		case errors.Is(opErr.Err, syscallECONNREFUSED()), errors.Is(opErr.Err, syscallENOTSOCK()), errors.Is(opErr.Err, syscallECONNRESET()):
			rmErr := os.Remove(path)
			if rmErr != nil && !errors.Is(rmErr, os.ErrNotExist) {
				return false, rmErr
			}
			return true, nil
		}
	}

	return false, err
}

func listenUnixPacket(path string, mode os.FileMode) (*net.UnixListener, error) {
	addr := &net.UnixAddr{Name: path, Net: "unixpacket"}
	for attempt := 0; attempt < 2; attempt++ {
		ln, err := net.ListenUnix("unixpacket", addr)
		if err == nil {
			if mode == 0 {
				mode = 0o600
			}
			_ = os.Chmod(path, mode)
			return ln, nil
		}
		if !errors.Is(err, os.ErrExist) && !isAddrInUse(err) {
			return nil, err
		}
		takeover, takeoverErr := tryTakeoverStaleSocket(path)
		if takeoverErr != nil {
			return nil, takeoverErr
		}
		if !takeover {
			return nil, os.ErrExist
		}
	}
	return nil, os.ErrExist
}

func dialUnixPacket(path string, timeout time.Duration) (*net.UnixConn, error) {
	if timeout > 0 {
		conn, err := net.DialTimeout("unixpacket", path, timeout)
		if err != nil {
			return nil, err
		}
		unixConn, ok := conn.(*net.UnixConn)
		if !ok {
			_ = conn.Close()
			return nil, fmt.Errorf("unexpected unix connection type %T", conn)
		}
		return unixConn, nil
	}

	addr := &net.UnixAddr{Name: path, Net: "unixpacket"}
	return net.DialUnix("unixpacket", nil, addr)
}

func isAddrInUse(err error) bool {
	var opErr *net.OpError
	if errors.As(err, &opErr) {
		if errors.Is(opErr.Err, syscallEADDRINUSE()) {
			return true
		}
	}
	return false
}

func udsServerHandshake(conn *net.UnixConn, server *Server, timeout time.Duration) (negotiationResult, error) {
	helloFrame, err := readFrame(conn, timeout)
	if err != nil {
		return negotiationResult{}, err
	}
	hello, err := decodeHelloNeg(helloFrame)
	if err != nil {
		return negotiationResult{}, err
	}

	ack := protocol.HelloAckPayload{
		LayoutVersion:              hello.LayoutVersion,
		Flags:                      0,
		ServerSupported:            server.supportedProfiles,
		Intersection:               hello.Supported & server.supportedProfiles,
		Selected:                   0,
		AgreedMaxRequestPayload:    negotiateLimit(hello.MaxRequestPayloadBytes, server.maxRequestPayloadBytes),
		AgreedMaxRequestBatchItems: negotiateLimit(hello.MaxRequestBatchItems, server.maxRequestBatchItems),
		AgreedMaxResponsePayload:   negotiateLimit(hello.MaxResponsePayloadBytes, server.maxResponsePayloadBytes),
		AgreedMaxResponseBatchItems: negotiateLimit(
			hello.MaxResponseBatchItems,
			server.maxResponseBatchItems,
		),
	}
	status := negStatusOK
	if server.authToken != 0 && hello.AuthToken != server.authToken {
		status = uint32(syscallEACCES())
	} else {
		candidates := ack.Intersection & server.preferredProfiles
		if candidates == 0 {
			candidates = ack.Intersection
		}
		ack.Selected = selectProfile(candidates)
		if ack.Selected == 0 {
			status = uint32(syscallENOTSUP())
		} else if ack.AgreedMaxRequestPayload == 0 || ack.AgreedMaxRequestBatchItems == 0 ||
			ack.AgreedMaxResponsePayload == 0 || ack.AgreedMaxResponseBatchItems == 0 {
			status = uint32(syscallEPROTO())
		}
	}

	if err := writeFrame(conn, encodeAckNeg(ack, status), timeout); err != nil {
		return negotiationResult{}, err
	}
	if status != negStatusOK {
		return negotiationResult{}, syscallErrno(status)
	}

	maxRequestMessageLen, err := computeMaxMessageLen(ack.AgreedMaxRequestPayload, ack.AgreedMaxRequestBatchItems)
	if err != nil {
		return negotiationResult{}, err
	}
	maxResponseMessageLen, err := computeMaxMessageLen(ack.AgreedMaxResponsePayload, ack.AgreedMaxResponseBatchItems)
	if err != nil {
		return negotiationResult{}, err
	}
	return negotiationResult{
		profile:               ack.Selected,
		maxRequestMessageLen:  maxRequestMessageLen,
		maxResponseMessageLen: maxResponseMessageLen,
	}, nil
}

func udsClientHandshake(conn *net.UnixConn, client *Client, timeout time.Duration) (negotiationResult, error) {
	hello := protocol.HelloPayload{
		LayoutVersion:           protocol.MessageVersion,
		Flags:                   0,
		Supported:               client.supportedProfiles,
		Preferred:               client.preferredProfiles,
		MaxRequestPayloadBytes:  client.maxRequestPayloadBytes,
		MaxRequestBatchItems:    client.maxRequestBatchItems,
		MaxResponsePayloadBytes: client.maxResponsePayloadBytes,
		MaxResponseBatchItems:   client.maxResponseBatchItems,
		AuthToken:               client.authToken,
	}
	if err := writeFrame(conn, encodeHelloNeg(hello), timeout); err != nil {
		return negotiationResult{}, err
	}
	ackFrame, err := readFrame(conn, timeout)
	if err != nil {
		return negotiationResult{}, err
	}
	ack, status, err := decodeAckNeg(ackFrame)
	if err != nil {
		return negotiationResult{}, err
	}
	if status != negStatusOK {
		return negotiationResult{}, syscallErrno(status)
	}
	if ack.Selected == 0 || (ack.Selected&client.supportedProfiles) == 0 || (ack.Intersection&client.supportedProfiles) == 0 ||
		ack.AgreedMaxRequestPayload == 0 || ack.AgreedMaxRequestBatchItems == 0 ||
		ack.AgreedMaxResponsePayload == 0 || ack.AgreedMaxResponseBatchItems == 0 {
		return negotiationResult{}, errors.New("invalid negotiated profile")
	}
	maxRequestMessageLen, err := computeMaxMessageLen(ack.AgreedMaxRequestPayload, ack.AgreedMaxRequestBatchItems)
	if err != nil {
		return negotiationResult{}, err
	}
	maxResponseMessageLen, err := computeMaxMessageLen(ack.AgreedMaxResponsePayload, ack.AgreedMaxResponseBatchItems)
	if err != nil {
		return negotiationResult{}, err
	}
	return negotiationResult{
		profile:               ack.Selected,
		maxRequestMessageLen:  maxRequestMessageLen,
		maxResponseMessageLen: maxResponseMessageLen,
	}, nil
}

func Listen(config Config) (*Server, error) {
	if config.RunDir == "" || config.ServiceName == "" {
		return nil, fmt.Errorf("run_dir and service_name must be set")
	}
	path := endpointSockPath(config.RunDir, config.ServiceName)
	ln, err := listenUnixPacket(path, config.FileMode)
	if err != nil {
		return nil, err
	}
	supportedProfiles := effectiveSupportedProfiles(config)
	preferredProfiles := effectivePreferredProfiles(config, supportedProfiles)
	return &Server{
		listener:          ln,
		path:              path,
		supportedProfiles: supportedProfiles,
		preferredProfiles: preferredProfiles,
		maxRequestPayloadBytes:  effectivePayloadLimit(config.MaxRequestPayloadBytes),
		maxRequestBatchItems:    effectiveBatchLimit(config.MaxRequestBatchItems),
		maxResponsePayloadBytes: effectivePayloadLimit(config.MaxResponsePayloadBytes),
		maxResponseBatchItems:   effectiveBatchLimit(config.MaxResponseBatchItems),
		authToken:         config.AuthToken,
	}, nil
}

func (s *Server) Accept(timeout time.Duration) error {
	if s.conn != nil {
		return errors.New("server is already connected")
	}
	if timeout > 0 {
		_ = s.listener.SetDeadline(time.Now().Add(timeout))
	} else {
		_ = s.listener.SetDeadline(time.Time{})
	}
	conn, err := s.listener.AcceptUnix()
	if err != nil {
		return err
	}
	negotiated, err := udsServerHandshake(conn, s, timeout)
	if err != nil {
		_ = conn.Close()
		return err
	}
	s.conn = conn
	s.negotiatedProfile = negotiated.profile
	s.maxRequestMessageLen = negotiated.maxRequestMessageLen
	s.maxResponseMessageLen = negotiated.maxResponseMessageLen
	if s.negotiatedProfile == ProfileSHMHybrid {
		shm, err := newSHMServer(s.path, s.maxRequestMessageLen, s.maxResponseMessageLen)
		if err != nil {
			_ = conn.Close()
			s.conn = nil
			s.negotiatedProfile = 0
			return err
		}
		s.shm = shm
	} else {
		s.shm = nil
	}
	return nil
}

func (s *Server) ReceiveMessage(message []byte, timeout time.Duration) (int, error) {
	if s.conn == nil {
		return 0, errors.New("server is not connected")
	}
	if s.negotiatedProfile == ProfileSHMHybrid {
		if s.shm == nil {
			return 0, errors.New("SHM server is not available")
		}
		return s.shm.receiveMessage(message, timeout)
	}
	if s.maxRequestMessageLen == 0 || len(message) < s.maxRequestMessageLen {
		return 0, fmt.Errorf("message buffer is smaller than negotiated request size")
	}
	messageLen, err := readMessage(s.conn, message, timeout)
	if err != nil {
		return 0, err
	}
	if err := validateReceivedMessage(message, messageLen, s.maxRequestMessageLen); err != nil {
		return 0, err
	}
	return messageLen, nil
}

func (s *Server) SendMessage(message []byte, timeout time.Duration) error {
	if s.conn == nil {
		return errors.New("server is not connected")
	}
	if s.negotiatedProfile == ProfileSHMHybrid {
		if s.shm == nil {
			return errors.New("SHM server is not available")
		}
		return s.shm.sendMessage(message)
	}
	if s.maxResponseMessageLen == 0 {
		return errors.New("negotiated response size is not available")
	}
	if err := validateMessageForSend(message, s.maxResponseMessageLen); err != nil {
		return err
	}
	return writeMessage(s.conn, message, timeout)
}

func (s *Server) ReceiveFrame(timeout time.Duration) (protocol.Frame, error) {
	if s.conn == nil {
		return protocol.Frame{}, errors.New("server is not connected")
	}
	if s.negotiatedProfile == ProfileSHMHybrid {
		if s.shm == nil {
			return protocol.Frame{}, errors.New("SHM server is not available")
		}
		return s.shm.receiveFrame(timeout)
	}
	buf := make([]byte, protocol.FrameSize)
	messageLen, err := readMessage(s.conn, buf, timeout)
	if err != nil {
		return protocol.Frame{}, err
	}
	if messageLen != protocol.FrameSize {
		return protocol.Frame{}, errors.New("received non-frame message on frame path")
	}
	var out protocol.Frame
	copy(out[:], buf[:messageLen])
	return out, nil
}

func (s *Server) SendFrame(frame protocol.Frame, timeout time.Duration) error {
	if s.conn == nil {
		return errors.New("server is not connected")
	}
	if s.negotiatedProfile == ProfileSHMHybrid {
		if s.shm == nil {
			return errors.New("SHM server is not available")
		}
		return s.shm.sendFrame(frame)
	}
	return writeMessage(s.conn, frame[:], timeout)
}

func (s *Server) ReceiveIncrement(timeout time.Duration) (uint64, protocol.IncrementRequest, error) {
	frame, err := s.ReceiveFrame(timeout)
	if err != nil {
		return 0, protocol.IncrementRequest{}, err
	}
	return protocol.DecodeIncrementRequest(frame)
}

func (s *Server) SendIncrement(requestID uint64, response protocol.IncrementResponse, timeout time.Duration) error {
	return s.SendFrame(protocol.EncodeIncrementResponse(requestID, response), timeout)
}

func (s *Server) NegotiatedProfile() uint32 {
	return s.negotiatedProfile
}

func (s *Server) Close() error {
	var errs []error
	if s.shm != nil {
		if err := s.shm.close(); err != nil {
			errs = append(errs, err)
		}
		s.shm = nil
	}
	if s.conn != nil {
		if err := s.conn.Close(); err != nil {
			errs = append(errs, err)
		}
		s.conn = nil
	}
	if s.listener != nil {
		if err := s.listener.Close(); err != nil {
			errs = append(errs, err)
		}
		s.listener = nil
	}
	if s.path != "" {
		if err := os.Remove(s.path); err != nil && !errors.Is(err, os.ErrNotExist) {
			errs = append(errs, err)
		}
	}
	return errors.Join(errs...)
}

func Dial(config Config, timeout time.Duration) (*Client, error) {
	if config.RunDir == "" || config.ServiceName == "" {
		return nil, fmt.Errorf("run_dir and service_name must be set")
	}
	conn, err := dialUnixPacket(endpointSockPath(config.RunDir, config.ServiceName), timeout)
	if err != nil {
		return nil, err
	}
	supportedProfiles := effectiveSupportedProfiles(config)
	preferredProfiles := effectivePreferredProfiles(config, supportedProfiles)
	negotiated, err := udsClientHandshake(conn, &Client{
		conn:                    conn,
		supportedProfiles:       supportedProfiles,
		preferredProfiles:       preferredProfiles,
		maxRequestPayloadBytes:  effectivePayloadLimit(config.MaxRequestPayloadBytes),
		maxRequestBatchItems:    effectiveBatchLimit(config.MaxRequestBatchItems),
		maxResponsePayloadBytes: effectivePayloadLimit(config.MaxResponsePayloadBytes),
		maxResponseBatchItems:   effectiveBatchLimit(config.MaxResponseBatchItems),
		authToken:               config.AuthToken,
		negotiatedProfile:       0,
		maxRequestMessageLen:    0,
		maxResponseMessageLen:   0,
		nextRequestID:           1,
	}, timeout)
	if err != nil {
		_ = conn.Close()
		return nil, err
	}
	client := &Client{
		conn:                  conn,
		shm:                   nil,
		supportedProfiles:     supportedProfiles,
		preferredProfiles:     preferredProfiles,
		maxRequestPayloadBytes:  effectivePayloadLimit(config.MaxRequestPayloadBytes),
		maxRequestBatchItems:    effectiveBatchLimit(config.MaxRequestBatchItems),
		maxResponsePayloadBytes: effectivePayloadLimit(config.MaxResponsePayloadBytes),
		maxResponseBatchItems:   effectiveBatchLimit(config.MaxResponseBatchItems),
		authToken:             config.AuthToken,
		negotiatedProfile:     negotiated.profile,
		maxRequestMessageLen:  negotiated.maxRequestMessageLen,
		maxResponseMessageLen: negotiated.maxResponseMessageLen,
		nextRequestID:         1,
	}
	if negotiated.profile == ProfileSHMHybrid {
		shm, err := newSHMClient(endpointSockPath(config.RunDir, config.ServiceName), negotiated.maxRequestMessageLen, negotiated.maxResponseMessageLen, timeout)
		if err != nil {
			_ = conn.Close()
			return nil, err
		}
		client.shm = shm
	}
	return client, nil
}

func (c *Client) CallMessage(requestMessage []byte, responseMessage []byte, timeout time.Duration) (int, error) {
	if c.conn == nil {
		return 0, errors.New("client is closed")
	}
	if c.negotiatedProfile == ProfileSHMHybrid {
		if c.shm == nil {
			return 0, errors.New("SHM client is not available")
		}
		return c.shm.callMessage(requestMessage, responseMessage, timeout)
	}
	if c.maxRequestMessageLen == 0 || c.maxResponseMessageLen == 0 {
		return 0, errors.New("negotiated message limits are not available")
	}
	if len(responseMessage) < c.maxResponseMessageLen {
		return 0, fmt.Errorf("response buffer is smaller than negotiated response size")
	}
	if err := validateMessageForSend(requestMessage, c.maxRequestMessageLen); err != nil {
		return 0, err
	}
	if err := writeMessage(c.conn, requestMessage, timeout); err != nil {
		return 0, err
	}
	messageLen, err := readMessage(c.conn, responseMessage, timeout)
	if err != nil {
		return 0, err
	}
	if err := validateReceivedMessage(responseMessage, messageLen, c.maxResponseMessageLen); err != nil {
		return 0, err
	}
	return messageLen, nil
}

func (c *Client) CallFrame(requestFrame protocol.Frame, timeout time.Duration) (protocol.Frame, error) {
	if c.conn == nil {
		return protocol.Frame{}, errors.New("client is closed")
	}
	if c.negotiatedProfile == ProfileSHMHybrid {
		if c.shm == nil {
			return protocol.Frame{}, errors.New("SHM client is not available")
		}
		return c.shm.callFrame(requestFrame, timeout)
	}
	if err := writeMessage(c.conn, requestFrame[:], timeout); err != nil {
		return protocol.Frame{}, err
	}
	buf := make([]byte, protocol.FrameSize)
	messageLen, err := readMessage(c.conn, buf, timeout)
	if err != nil {
		return protocol.Frame{}, err
	}
	if messageLen != protocol.FrameSize {
		return protocol.Frame{}, errors.New("received non-frame message on frame path")
	}
	var out protocol.Frame
	copy(out[:], buf[:messageLen])
	return out, nil
}

func (c *Client) CallIncrement(request protocol.IncrementRequest, timeout time.Duration) (protocol.IncrementResponse, error) {
	requestID := c.nextRequestID
	c.nextRequestID++
	responseFrame, err := c.CallFrame(protocol.EncodeIncrementRequest(requestID, request), timeout)
	if err != nil {
		return protocol.IncrementResponse{}, err
	}
	responseID, response, err := protocol.DecodeIncrementResponse(responseFrame)
	if err != nil {
		return protocol.IncrementResponse{}, err
	}
	if responseID != requestID {
		return protocol.IncrementResponse{}, errors.New("response request_id mismatch")
	}
	return response, nil
}

func (c *Client) NegotiatedProfile() uint32 {
	return c.negotiatedProfile
}

func (c *Client) Close() error {
	var errs []error
	if c.shm != nil {
		if err := c.shm.close(); err != nil {
			errs = append(errs, err)
		}
		c.shm = nil
	}
	if c.conn == nil {
		return errors.Join(errs...)
	}
	err := c.conn.Close()
	c.conn = nil
	if err != nil {
		errs = append(errs, err)
	}
	return errors.Join(errs...)
}
