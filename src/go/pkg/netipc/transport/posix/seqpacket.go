//go:build unix

package posix

import (
	"encoding/binary"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"time"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

const (
	ProfileUDSSeqpacket uint32 = 1 << 0
	ProfileSHMHybrid    uint32 = 1 << 1
	ProfileSHMFutex     uint32 = 1 << 2

	implementedProfiles             = ProfileUDSSeqpacket
	DefaultSupportedProfiles        = ProfileUDSSeqpacket
	DefaultPreferredProfiles        = ProfileUDSSeqpacket
	negMagic                 uint32 = 0x4e48534b
	negVersion               uint16 = 1
	negHello                 uint16 = 1
	negAck                   uint16 = 2
	negStatusOK              uint32 = 0
)

const (
	negOffMagic        = 0
	negOffVersion      = 4
	negOffType         = 6
	negOffSupported    = 8
	negOffPreferred    = 12
	negOffIntersection = 16
	negOffSelected     = 20
	negOffAuthToken    = 24
	negOffStatus       = 32
)

type Config struct {
	RunDir            string
	ServiceName       string
	FileMode          os.FileMode
	SupportedProfiles uint32
	PreferredProfiles uint32
	AuthToken         uint64
}

func NewConfig(runDir, serviceName string) Config {
	return Config{
		RunDir:            runDir,
		ServiceName:       serviceName,
		FileMode:          0o600,
		SupportedProfiles: DefaultSupportedProfiles,
		PreferredProfiles: DefaultPreferredProfiles,
	}
}

type Server struct {
	listener          *net.UnixListener
	conn              *net.UnixConn
	path              string
	supportedProfiles uint32
	preferredProfiles uint32
	authToken         uint64
	negotiatedProfile uint32
}

type Client struct {
	conn              *net.UnixConn
	supportedProfiles uint32
	preferredProfiles uint32
	authToken         uint64
	negotiatedProfile uint32
	nextRequestID     uint64
}

type negMessage struct {
	typ          uint16
	supported    uint32
	preferred    uint32
	intersection uint32
	selected     uint32
	authToken    uint64
	status       uint32
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

func encodeNeg(msg negMessage) protocol.Frame {
	var frame protocol.Frame
	binary.LittleEndian.PutUint32(frame[negOffMagic:negOffMagic+4], negMagic)
	binary.LittleEndian.PutUint16(frame[negOffVersion:negOffVersion+2], negVersion)
	binary.LittleEndian.PutUint16(frame[negOffType:negOffType+2], msg.typ)
	binary.LittleEndian.PutUint32(frame[negOffSupported:negOffSupported+4], msg.supported)
	binary.LittleEndian.PutUint32(frame[negOffPreferred:negOffPreferred+4], msg.preferred)
	binary.LittleEndian.PutUint32(frame[negOffIntersection:negOffIntersection+4], msg.intersection)
	binary.LittleEndian.PutUint32(frame[negOffSelected:negOffSelected+4], msg.selected)
	binary.LittleEndian.PutUint64(frame[negOffAuthToken:negOffAuthToken+8], msg.authToken)
	binary.LittleEndian.PutUint32(frame[negOffStatus:negOffStatus+4], msg.status)
	return frame
}

func decodeNeg(frame protocol.Frame, expectedType uint16) (negMessage, error) {
	magic := binary.LittleEndian.Uint32(frame[negOffMagic : negOffMagic+4])
	version := binary.LittleEndian.Uint16(frame[negOffVersion : negOffVersion+2])
	typ := binary.LittleEndian.Uint16(frame[negOffType : negOffType+2])
	if magic != negMagic || version != negVersion || typ != expectedType {
		return negMessage{}, errors.New("invalid negotiation frame")
	}

	return negMessage{
		typ:          typ,
		supported:    binary.LittleEndian.Uint32(frame[negOffSupported : negOffSupported+4]),
		preferred:    binary.LittleEndian.Uint32(frame[negOffPreferred : negOffPreferred+4]),
		intersection: binary.LittleEndian.Uint32(frame[negOffIntersection : negOffIntersection+4]),
		selected:     binary.LittleEndian.Uint32(frame[negOffSelected : negOffSelected+4]),
		authToken:    binary.LittleEndian.Uint64(frame[negOffAuthToken : negOffAuthToken+8]),
		status:       binary.LittleEndian.Uint32(frame[negOffStatus : negOffStatus+4]),
	}, nil
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

func udsServerHandshake(conn *net.UnixConn, supportedProfiles, preferredProfiles uint32, authToken uint64, timeout time.Duration) (uint32, error) {
	helloFrame, err := readFrame(conn, timeout)
	if err != nil {
		return 0, err
	}
	hello, err := decodeNeg(helloFrame, negHello)
	if err != nil {
		return 0, err
	}

	ack := negMessage{
		typ:          negAck,
		supported:    supportedProfiles,
		preferred:    preferredProfiles,
		intersection: hello.supported & supportedProfiles,
		status:       negStatusOK,
	}
	if authToken != 0 && hello.authToken != authToken {
		ack.status = uint32(syscallEACCES())
	} else {
		candidates := ack.intersection & preferredProfiles
		if candidates == 0 {
			candidates = ack.intersection
		}
		ack.selected = selectProfile(candidates)
		if ack.selected == 0 {
			ack.status = uint32(syscallENOTSUP())
		}
	}

	if err := writeFrame(conn, encodeNeg(ack), timeout); err != nil {
		return 0, err
	}
	if ack.status != negStatusOK {
		return 0, syscallErrno(ack.status)
	}
	return ack.selected, nil
}

func udsClientHandshake(conn *net.UnixConn, supportedProfiles, preferredProfiles uint32, authToken uint64, timeout time.Duration) (uint32, error) {
	hello := negMessage{
		typ:       negHello,
		supported: supportedProfiles,
		preferred: preferredProfiles,
		authToken: authToken,
		status:    negStatusOK,
	}
	if err := writeFrame(conn, encodeNeg(hello), timeout); err != nil {
		return 0, err
	}
	ackFrame, err := readFrame(conn, timeout)
	if err != nil {
		return 0, err
	}
	ack, err := decodeNeg(ackFrame, negAck)
	if err != nil {
		return 0, err
	}
	if ack.status != negStatusOK {
		return 0, syscallErrno(ack.status)
	}
	if ack.selected == 0 || (ack.selected&supportedProfiles) == 0 || (ack.intersection&supportedProfiles) == 0 {
		return 0, errors.New("invalid negotiated profile")
	}
	return ack.selected, nil
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
	profile, err := udsServerHandshake(conn, s.supportedProfiles, s.preferredProfiles, s.authToken, timeout)
	if err != nil {
		_ = conn.Close()
		return err
	}
	s.conn = conn
	s.negotiatedProfile = profile
	return nil
}

func (s *Server) ReceiveFrame(timeout time.Duration) (protocol.Frame, error) {
	if s.conn == nil {
		return protocol.Frame{}, errors.New("server is not connected")
	}
	return readFrame(s.conn, timeout)
}

func (s *Server) SendFrame(frame protocol.Frame, timeout time.Duration) error {
	if s.conn == nil {
		return errors.New("server is not connected")
	}
	return writeFrame(s.conn, frame, timeout)
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
	profile, err := udsClientHandshake(conn, supportedProfiles, preferredProfiles, config.AuthToken, timeout)
	if err != nil {
		_ = conn.Close()
		return nil, err
	}
	return &Client{
		conn:              conn,
		supportedProfiles: supportedProfiles,
		preferredProfiles: preferredProfiles,
		authToken:         config.AuthToken,
		negotiatedProfile: profile,
		nextRequestID:     1,
	}, nil
}

func (c *Client) CallFrame(requestFrame protocol.Frame, timeout time.Duration) (protocol.Frame, error) {
	if c.conn == nil {
		return protocol.Frame{}, errors.New("client is closed")
	}
	if err := writeFrame(c.conn, requestFrame, timeout); err != nil {
		return protocol.Frame{}, err
	}
	return readFrame(c.conn, timeout)
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
	if c.conn == nil {
		return nil
	}
	err := c.conn.Close()
	c.conn = nil
	return err
}
