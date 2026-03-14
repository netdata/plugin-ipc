//go:build windows

// Package windows provides Named Pipe and SHM HYBRID transports for Windows.
// The negotiation protocol and SHM region layout are wire-compatible
// with the C implementation in netipc_named_pipe.c / netipc_shm_hybrid_win.c.
package windows

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"runtime"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

// ---------------------------------------------------------------------------
// Win32 API bindings via LazyDLL
// ---------------------------------------------------------------------------

var (
	kernel32 = syscall.NewLazyDLL("kernel32.dll")

	procCreateFileMappingW    = kernel32.NewProc("CreateFileMappingW")
	procOpenFileMappingW      = kernel32.NewProc("OpenFileMappingW")
	procMapViewOfFile         = kernel32.NewProc("MapViewOfFile")
	procUnmapViewOfFile       = kernel32.NewProc("UnmapViewOfFile")
	procCreateEventW          = kernel32.NewProc("CreateEventW")
	procOpenEventW            = kernel32.NewProc("OpenEventW")
	procSetEvent              = kernel32.NewProc("SetEvent")
	procWaitForSingleObject   = kernel32.NewProc("WaitForSingleObject")
	procCreateNamedPipeW      = kernel32.NewProc("CreateNamedPipeW")
	procConnectNamedPipe      = kernel32.NewProc("ConnectNamedPipe")
	procDisconnectNamedPipe   = kernel32.NewProc("DisconnectNamedPipe")
	procSetNamedPipeHandState = kernel32.NewProc("SetNamedPipeHandleState")
	procWaitNamedPipeW        = kernel32.NewProc("WaitNamedPipeW")
	procPeekNamedPipe         = kernel32.NewProc("PeekNamedPipe")
	procFlushFileBuffers      = kernel32.NewProc("FlushFileBuffers")
	procCreateFileW           = kernel32.NewProc("CreateFileW")
	procReadFile              = kernel32.NewProc("ReadFile")
	procWriteFile             = kernel32.NewProc("WriteFile")
	procCloseHandle           = kernel32.NewProc("CloseHandle")
	procGetTickCount64        = kernel32.NewProc("GetTickCount64")
	procSleep                 = kernel32.NewProc("Sleep")
	procGetProcessTimes       = kernel32.NewProc("GetProcessTimes")
	procGetCurrentProcess     = kernel32.NewProc("GetCurrentProcess")
)

// ---------------------------------------------------------------------------
// Win32 constants
// ---------------------------------------------------------------------------

const (
	invalidHandleValue = ^syscall.Handle(0)

	pageReadWrite    = 0x04
	fileMapAllAccess = 0xF001F

	genericRead  = 0x80000000
	genericWrite = 0x40000000
	openExisting = 3

	pipeAccessDuplex          = 0x00000003
	fileFlagFirstPipeInstance = 0x00080000
	pipeTypeMessage           = 0x00000004
	pipeReadModeMessage       = 0x00000002
	pipeWait                  = 0x00000000
	pipeNoWait                = 0x00000001

	waitObject0 = 0
	waitTimeout = 258
	infinite    = 0xFFFFFFFF

	errorAlreadyExists    = 183
	errorFileNotFound     = 2
	errorPipeBusy         = 231
	errorPipeConnected    = 535
	errorPipeListening    = 536
	errorPipeNotConnected = 233
	errorMoreData         = 234
	errorNoData           = 232
	errorBrokenPipe       = 109
	errorAccessDenied     = 5
	errorNotSupported     = 50
	errorInvalidParameter = 87

	nmpwaitWaitForever = 0xFFFFFFFF

	synchronize      = 0x00100000
	eventModifyState = 0x0002
)

// ---------------------------------------------------------------------------
// Profile constants (wire-compatible with C)
// ---------------------------------------------------------------------------

const (
	ProfileNamedPipe   uint32 = 1 << 0
	ProfileSHMHybrid   uint32 = 1 << 1
	ProfileSHMBusyWait uint32 = 1 << 2
	ProfileSHMWaitAddr uint32 = 1 << 3

	implementedProfiles      = ProfileNamedPipe | ProfileSHMHybrid
	DefaultSupportedProfiles = ProfileNamedPipe
	DefaultPreferredProfiles = ProfileNamedPipe
	DefaultSHMSpinTries      = uint32(1024)
)

// ---------------------------------------------------------------------------
// Negotiation constants
// ---------------------------------------------------------------------------

const (
	negMagic             uint32 = 0x4e48534b
	negVersion           uint16 = 1
	negHello             uint16 = 1
	negAck               uint16 = 2
	negStatusOK          uint32 = 0
	negPayloadOffset            = 8
	negStatusOffset             = 48
	negDefaultBatchItems uint32 = 1
)

const (
	negOffMagic   = 0
	negOffVersion = 4
	negOffType    = 6
)

// ---------------------------------------------------------------------------
// SHM region layout constants (wire-compatible with C netipc_win_shm_header v3)
// ---------------------------------------------------------------------------

const (
	shmRegionMagic   uint32 = 0x4e535748
	shmRegionVersion uint32 = 3
	cacheline               = 64
	hdrSize                 = 128

	offHdrMagic         = 0
	offHdrVersion       = 4
	offHdrHeaderLen     = 8
	offHdrProfile       = 12
	offHdrReqOffset     = 16
	offHdrReqCapacity   = 20
	offHdrRespOffset    = 24
	offHdrRespCapacity  = 28
	offHdrSpinTries     = 32
	offReqLen           = 36
	offRespLen          = 40
	offReqClientClosed  = 44
	offReqServerWait    = 48
	offRespServerClosed = 52
	offRespClientWait   = 56
	offReqSeq           = 64
	offRespSeq          = 72
)

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

// Config holds the configuration for a Named Pipe transport.
type Config struct {
	RunDir                  string
	ServiceName             string
	SupportedProfiles       uint32
	PreferredProfiles       uint32
	MaxRequestPayloadBytes  uint32
	MaxRequestBatchItems    uint32
	MaxResponsePayloadBytes uint32
	MaxResponseBatchItems   uint32
	AuthToken               uint64
	SHMSpinTries            uint32
}

// NewConfig creates a new Config with default settings.
func NewConfig(runDir, serviceName string) Config {
	return Config{
		RunDir:                  runDir,
		ServiceName:             serviceName,
		SupportedProfiles:       DefaultSupportedProfiles,
		PreferredProfiles:       DefaultPreferredProfiles,
		MaxRequestPayloadBytes:  protocol.MaxPayloadDefault,
		MaxRequestBatchItems:    negDefaultBatchItems,
		MaxResponsePayloadBytes: protocol.MaxPayloadDefault,
		MaxResponseBatchItems:   negDefaultBatchItems,
	}
}

// ---------------------------------------------------------------------------
// Win32 helpers
// ---------------------------------------------------------------------------

func toUTF16(s string) *uint16 {
	p, _ := syscall.UTF16PtrFromString(s)
	return p
}

func nowMS() uint64 {
	r, _, _ := procGetTickCount64.Call()
	return uint64(r)
}

func sleepMS(ms uint32) {
	procSleep.Call(uintptr(ms))
}

func closeHandle(h syscall.Handle) {
	if h != 0 && h != invalidHandleValue {
		procCloseHandle.Call(uintptr(h))
	}
}

// ---------------------------------------------------------------------------
// FNV-1a hash
// ---------------------------------------------------------------------------

func fnv1a64(data []byte) uint64 {
	h := uint64(14695981039346656037)
	for _, b := range data {
		h ^= uint64(b)
		h *= 1099511628211
	}
	return h
}

func endpointHash(config *Config) uint64 {
	h := uint64(14695981039346656037)
	for _, b := range []byte(config.RunDir) {
		h ^= uint64(b)
		h *= 1099511628211
	}
	h ^= uint64('\n')
	h *= 1099511628211
	for _, b := range []byte(config.ServiceName) {
		h ^= uint64(b)
		h *= 1099511628211
	}
	h ^= uint64('\n')
	h *= 1099511628211
	var tok [8]byte
	binary.LittleEndian.PutUint64(tok[:], config.AuthToken)
	for _, b := range tok {
		h ^= uint64(b)
		h *= 1099511628211
	}
	return h
}

func sanitizeService(name string) string {
	out := make([]byte, 0, len(name))
	for i := 0; i < len(name) && len(out) < 95; i++ {
		ch := name[i]
		if (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
			ch == '-' || ch == '_' || ch == '.' {
			out = append(out, ch)
		} else {
			out = append(out, '_')
		}
	}
	if len(out) == 0 {
		return "service"
	}
	return string(out)
}

func buildPipeName(config *Config) string {
	hash := fnv1a64([]byte(config.RunDir))
	svc := sanitizeService(config.ServiceName)
	return fmt.Sprintf(`\\.\pipe\netipc-%016x-%s`, hash, svc)
}

func buildKernelObjectName(config *Config, profile uint32, suffix string) string {
	hash := endpointHash(config)
	svc := sanitizeService(config.ServiceName)
	return fmt.Sprintf(`Local\netipc-%016x-%s-p%d-%s`, hash, svc, profile, suffix)
}

func effectiveSupported(config *Config) uint32 {
	s := config.SupportedProfiles
	if s == 0 {
		s = DefaultSupportedProfiles
	}
	s &= implementedProfiles
	if s == 0 {
		s = DefaultSupportedProfiles
	}
	return s
}

func effectivePreferred(config *Config, supported uint32) uint32 {
	p := config.PreferredProfiles
	if p == 0 {
		p = supported
	}
	p &= supported
	if p == 0 {
		p = supported
	}
	return p
}

func effectiveSpinTries(config *Config) uint32 {
	if config.SHMSpinTries != 0 {
		return config.SHMSpinTries
	}
	return DefaultSHMSpinTries
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

func isSHMProfile(profile uint32) bool {
	return profile == ProfileSHMHybrid || profile == ProfileSHMBusyWait || profile == ProfileSHMWaitAddr
}

func selectProfile(candidates uint32) uint32 {
	if candidates&ProfileSHMWaitAddr != 0 {
		return ProfileSHMWaitAddr
	}
	if candidates&ProfileSHMBusyWait != 0 {
		return ProfileSHMBusyWait
	}
	if candidates&ProfileSHMHybrid != 0 {
		return ProfileSHMHybrid
	}
	if candidates&ProfileNamedPipe != 0 {
		return ProfileNamedPipe
	}
	return 0
}

// ---------------------------------------------------------------------------
// Negotiation helpers
// ---------------------------------------------------------------------------

func negotiateLimit(offered, local uint32) uint32 {
	if offered == 0 || local == 0 {
		return 0
	}
	if offered < local {
		return offered
	}
	return local
}

type negotiationResult struct {
	profile                       uint32
	agreedMaxRequestPayloadBytes  uint32
	agreedMaxRequestBatchItems    uint32
	agreedMaxResponsePayloadBytes uint32
	agreedMaxResponseBatchItems   uint32
	packetSize                    uint32
	maxRequestMessageLen          int
	maxResponseMessageLen         int
}

func computeMaxMessageLen(maxPayloadBytes, maxBatchItems uint32) (int, error) {
	total, err := protocol.MaxBatchTotalSize(maxPayloadBytes, maxBatchItems)
	if err != nil {
		return 0, err
	}
	return total, nil
}

func computePipePacketSize(maxRequestMessageLen, maxResponseMessageLen int) (uint32, error) {
	packetSize := maxRequestMessageLen
	if maxResponseMessageLen > packetSize {
		packetSize = maxResponseMessageLen
	}
	if packetSize <= protocol.ChunkHeaderLen {
		return 0, fmt.Errorf("invalid named pipe packet size")
	}
	if packetSize > int(^uint32(0)) {
		return 0, fmt.Errorf("named pipe packet size exceeds uint32")
	}
	return uint32(packetSize), nil
}

func computeChunkPayloadBudget(packetSize uint32) (int, error) {
	if packetSize <= protocol.ChunkHeaderLen {
		return 0, fmt.Errorf("packet size is too small for chunking")
	}
	return int(packetSize) - protocol.ChunkHeaderLen, nil
}

func alignUpSize(value, alignment int) int {
	remainder := value % alignment
	if remainder == 0 {
		return value
	}
	return value + (alignment - remainder)
}

func computeRegionLayout(requestCapacity, responseCapacity int) (int, int, int, error) {
	if requestCapacity == 0 || responseCapacity == 0 {
		return 0, 0, 0, fmt.Errorf("invalid SHM capacities")
	}
	requestOffset := alignUpSize(hdrSize, cacheline)
	responseOffset := alignUpSize(requestOffset+requestCapacity, cacheline)
	mappingLen := responseOffset + responseCapacity
	return requestOffset, responseOffset, mappingLen, nil
}

func shmU32(region uintptr, off int) uint32 {
	return binary.LittleEndian.Uint32((*[4]byte)(unsafe.Pointer(region + uintptr(off)))[:])
}

func requestArea(region uintptr) int {
	return int(shmU32(region, offHdrReqOffset))
}

func responseArea(region uintptr) int {
	return int(shmU32(region, offHdrRespOffset))
}

func requestCapacity(region uintptr) int {
	return int(shmU32(region, offHdrReqCapacity))
}

func responseCapacity(region uintptr) int {
	return int(shmU32(region, offHdrRespCapacity))
}

func validateRegionHeader(region uintptr, mappingLen int, profile uint32) (uint32, error) {
	magic := shmU32(region, offHdrMagic)
	version := shmU32(region, offHdrVersion)
	headerLen := int(shmU32(region, offHdrHeaderLen))
	prof := shmU32(region, offHdrProfile)
	reqOff := requestArea(region)
	reqCap := requestCapacity(region)
	respOff := responseArea(region)
	respCap := responseCapacity(region)
	spin := shmU32(region, offHdrSpinTries)

	if magic != shmRegionMagic || version != shmRegionVersion || headerLen != hdrSize || prof != profile || reqCap == 0 || respCap == 0 {
		return 0, errors.New("invalid SHM header")
	}
	if reqOff < headerLen {
		return 0, errors.New("invalid SHM request offset")
	}
	if respOff < reqOff+reqCap {
		return 0, errors.New("invalid SHM response offset")
	}
	if respOff+respCap > mappingLen {
		return 0, errors.New("invalid SHM mapping length")
	}
	return spin, nil
}

func encodeNegHeader(typ uint16) protocol.Frame {
	var frame protocol.Frame
	binary.LittleEndian.PutUint32(frame[negOffMagic:], negMagic)
	binary.LittleEndian.PutUint16(frame[negOffVersion:], negVersion)
	binary.LittleEndian.PutUint16(frame[negOffType:], typ)
	return frame
}

func encodeHelloNeg(payload protocol.HelloPayload) protocol.Frame {
	frame := encodeNegHeader(negHello)
	hello := protocol.EncodeHelloPayload(payload)
	copy(frame[negPayloadOffset:negPayloadOffset+protocol.ControlHelloPayloadLen], hello[:])
	return frame
}

func encodeAckNeg(payload protocol.HelloAckPayload, status uint32) protocol.Frame {
	frame := encodeNegHeader(negAck)
	ack := protocol.EncodeHelloAckPayload(payload)
	copy(frame[negPayloadOffset:negPayloadOffset+protocol.ControlHelloAckPayloadLen], ack[:])
	binary.LittleEndian.PutUint32(frame[negStatusOffset:negStatusOffset+4], status)
	return frame
}

func decodeNegHeader(frame protocol.Frame, expectedType uint16) error {
	magic := binary.LittleEndian.Uint32(frame[negOffMagic:])
	version := binary.LittleEndian.Uint16(frame[negOffVersion:])
	typ := binary.LittleEndian.Uint16(frame[negOffType:])
	if magic != negMagic || version != negVersion || typ != expectedType {
		return errors.New("invalid negotiation frame")
	}
	return nil
}

func decodeHelloNeg(frame protocol.Frame) (protocol.HelloPayload, error) {
	if err := decodeNegHeader(frame, negHello); err != nil {
		return protocol.HelloPayload{}, err
	}
	return protocol.DecodeHelloPayload(frame[negPayloadOffset : negPayloadOffset+protocol.ControlHelloPayloadLen])
}

func decodeAckNeg(frame protocol.Frame) (protocol.HelloAckPayload, uint32, error) {
	if err := decodeNegHeader(frame, negAck); err != nil {
		return protocol.HelloAckPayload{}, 0, err
	}
	ack, err := protocol.DecodeHelloAckPayload(frame[negPayloadOffset : negPayloadOffset+protocol.ControlHelloAckPayloadLen])
	if err != nil {
		return protocol.HelloAckPayload{}, 0, err
	}
	status := binary.LittleEndian.Uint32(frame[negStatusOffset : negStatusOffset+4])
	return ack, status, nil
}

// ---------------------------------------------------------------------------
// Pipe I/O helpers
// ---------------------------------------------------------------------------

func waitPipeMessage(pipe syscall.Handle, timeoutMS uint32) (uint32, error) {
	var deadline uint64
	if timeoutMS != 0 {
		deadline = nowMS() + uint64(timeoutMS)
	}

	for {
		var avail uint32
		var left uint32
		r, _, lastErr := procPeekNamedPipe.Call(
			uintptr(pipe),
			0,
			0,
			0,
			uintptr(unsafe.Pointer(&avail)),
			uintptr(unsafe.Pointer(&left)),
		)
		if r == 0 {
			if errCode, ok := lastErr.(syscall.Errno); ok &&
				(errCode == syscall.Errno(errorBrokenPipe) ||
					errCode == syscall.Errno(errorNoData) ||
					errCode == syscall.Errno(errorPipeNotConnected)) {
				return 0, io.EOF
			}
			return 0, fmt.Errorf("PeekNamedPipe failed: %w", lastErr)
		}
		if left != 0 || avail != 0 {
			if left != 0 {
				return left, nil
			}
			return avail, nil
		}
		if deadline != 0 && nowMS() >= deadline {
			return 0, errors.New("pipe read timeout")
		}
		sleepMS(1)
	}
}

func drainPipeMessage(pipe syscall.Handle) error {
	var scratch [256]byte

	for {
		var bytesRead uint32
		r, _, lastErr := procReadFile.Call(
			uintptr(pipe),
			uintptr(unsafe.Pointer(&scratch[0])),
			uintptr(len(scratch)),
			uintptr(unsafe.Pointer(&bytesRead)),
			0,
		)
		if r != 0 {
			return nil
		}
		errno, _ := lastErr.(syscall.Errno)
		if uint32(errno) == errorMoreData {
			continue
		}
		return fmt.Errorf("ReadFile failed: %w", lastErr)
	}
}

func pipeReadMessage(pipe syscall.Handle, buf []byte, timeoutMS uint32) (int, error) {
	if len(buf) == 0 {
		return 0, errors.New("buffer must not be empty")
	}

	messageLen, err := waitPipeMessage(pipe, timeoutMS)
	if err != nil {
		return 0, err
	}
	if int(messageLen) > len(buf) {
		_ = drainPipeMessage(pipe)
		return 0, errors.New("message exceeds negotiated size")
	}

	var bytesRead uint32
	r, _, lastErr := procReadFile.Call(
		uintptr(pipe),
		uintptr(unsafe.Pointer(&buf[0])),
		uintptr(messageLen),
		uintptr(unsafe.Pointer(&bytesRead)),
		0,
	)
	if r == 0 {
		errno, _ := lastErr.(syscall.Errno)
		if uint32(errno) == errorMoreData {
			_ = drainPipeMessage(pipe)
			return 0, errors.New("message exceeds negotiated size")
		}
		return 0, fmt.Errorf("ReadFile failed: %w", lastErr)
	}
	if bytesRead != messageLen {
		return 0, fmt.Errorf("short pipe read: %d", bytesRead)
	}
	return int(bytesRead), nil
}

func pipeWriteMessage(pipe syscall.Handle, message []byte) error {
	if len(message) == 0 {
		return errors.New("message must not be empty")
	}
	var written uint32
	r, _, lastErr := procWriteFile.Call(
		uintptr(pipe),
		uintptr(unsafe.Pointer(&message[0])),
		uintptr(len(message)),
		uintptr(unsafe.Pointer(&written)),
		0,
	)
	if r == 0 {
		return fmt.Errorf("WriteFile failed: %w", lastErr)
	}
	if int(written) != len(message) {
		return fmt.Errorf("short pipe write: %d", written)
	}
	return nil
}

func sendChunkedMessage(pipe syscall.Handle, message []byte, packetSize uint32) error {
	chunkPayloadBudget, err := computeChunkPayloadBudget(packetSize)
	if err != nil {
		return err
	}
	header, err := protocol.DecodeMessageHeader(message)
	if err != nil {
		return err
	}

	chunkCount := (len(message) + chunkPayloadBudget - 1) / chunkPayloadBudget
	packet := make([]byte, int(packetSize))
	offset := 0
	for chunkIndex := 0; chunkIndex < chunkCount; chunkIndex++ {
		remaining := len(message) - offset
		chunkPayloadLen := remaining
		if chunkPayloadLen > chunkPayloadBudget {
			chunkPayloadLen = chunkPayloadBudget
		}

		chunkHeader, err := protocol.EncodeChunkHeader(protocol.ChunkHeader{
			Magic:           protocol.ChunkMagic,
			Version:         protocol.ChunkVersion,
			Flags:           0,
			MessageID:       header.MessageID,
			TotalMessageLen: uint32(len(message)),
			ChunkIndex:      uint32(chunkIndex),
			ChunkCount:      uint32(chunkCount),
			ChunkPayloadLen: uint32(chunkPayloadLen),
		})
		if err != nil {
			return err
		}

		copy(packet[:protocol.ChunkHeaderLen], chunkHeader[:])
		copy(packet[protocol.ChunkHeaderLen:protocol.ChunkHeaderLen+chunkPayloadLen], message[offset:offset+chunkPayloadLen])
		if err := pipeWriteMessage(pipe, packet[:protocol.ChunkHeaderLen+chunkPayloadLen]); err != nil {
			return err
		}
		offset += chunkPayloadLen
	}

	return nil
}

func validateMessageForSend(message []byte, maxMessageLen int) error {
	if len(message) == 0 {
		return errors.New("message must not be empty")
	}
	if len(message) > maxMessageLen {
		return errors.New("message exceeds negotiated size")
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
		return errors.New("message size does not match header")
	}
	return nil
}

func validateReceivedMessage(message []byte, messageLen int, maxMessageLen int) error {
	if messageLen == 0 {
		return errors.New("message must not be empty")
	}
	if messageLen > maxMessageLen {
		return errors.New("message exceeds negotiated size")
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
		return errors.New("message size does not match header")
	}
	return nil
}

func recvTransportMessage(pipe syscall.Handle, message []byte, maxMessageLen int, packetSize uint32, timeoutMS uint32) (int, error) {
	firstCapacity := len(message)
	if packetSize != 0 && firstCapacity > int(packetSize) {
		firstCapacity = int(packetSize)
	}

	firstPacketLen, err := pipeReadMessage(pipe, message[:firstCapacity], timeoutMS)
	if err != nil {
		return 0, err
	}
	if firstPacketLen >= protocol.MessageHeaderLen {
		header, err := protocol.DecodeMessageHeader(message[:firstPacketLen])
		if err == nil {
			total, err := protocol.MessageTotalSize(header)
			if err == nil && total == firstPacketLen {
				if err := validateReceivedMessage(message, firstPacketLen, maxMessageLen); err != nil {
					return 0, err
				}
				return firstPacketLen, nil
			}
		}
	}

	firstChunk, err := protocol.DecodeChunkHeader(message[:firstPacketLen])
	if err != nil {
		return 0, err
	}
	if firstChunk.ChunkIndex != 0 || firstChunk.ChunkCount < 2 {
		return 0, errors.New("invalid first chunk header")
	}
	if int(firstChunk.TotalMessageLen) > len(message) || int(firstChunk.TotalMessageLen) > maxMessageLen {
		return 0, errors.New("message exceeds negotiated size")
	}

	firstPayloadLen := firstPacketLen - protocol.ChunkHeaderLen
	if firstPayloadLen != int(firstChunk.ChunkPayloadLen) {
		return 0, errors.New("invalid first chunk payload length")
	}

	copy(message[:firstPayloadLen], message[protocol.ChunkHeaderLen:firstPacketLen])
	offset := firstPayloadLen
	packet := make([]byte, int(packetSize))
	for expectedIndex := uint32(1); expectedIndex < firstChunk.ChunkCount; expectedIndex++ {
		packetLen, err := pipeReadMessage(pipe, packet, timeoutMS)
		if err != nil {
			return 0, err
		}
		if packetLen < protocol.ChunkHeaderLen {
			return 0, errors.New("short chunk packet")
		}

		chunk, err := protocol.DecodeChunkHeader(packet[:protocol.ChunkHeaderLen])
		if err != nil {
			return 0, err
		}
		payloadLen := packetLen - protocol.ChunkHeaderLen
		if chunk.MessageID != firstChunk.MessageID ||
			chunk.TotalMessageLen != firstChunk.TotalMessageLen ||
			chunk.ChunkCount != firstChunk.ChunkCount ||
			chunk.ChunkIndex != expectedIndex ||
			payloadLen != int(chunk.ChunkPayloadLen) ||
			offset+payloadLen > int(firstChunk.TotalMessageLen) {
			return 0, errors.New("chunk stream desync")
		}

		copy(message[offset:offset+payloadLen], packet[protocol.ChunkHeaderLen:packetLen])
		offset += payloadLen
	}

	if offset != int(firstChunk.TotalMessageLen) {
		return 0, errors.New("incomplete chunked message")
	}
	if err := validateReceivedMessage(message, offset, maxMessageLen); err != nil {
		return 0, err
	}
	return offset, nil
}

func sendTransportMessage(pipe syscall.Handle, message []byte, maxMessageLen int, packetSize uint32) error {
	if err := validateMessageForSend(message, maxMessageLen); err != nil {
		return err
	}
	if packetSize != 0 && len(message) > int(packetSize) {
		return sendChunkedMessage(pipe, message, packetSize)
	}
	return pipeWriteMessage(pipe, message)
}

func pipeReadFrame(pipe syscall.Handle, timeoutMS uint32) (protocol.Frame, error) {
	buf := make([]byte, protocol.FrameSize)
	n, err := pipeReadMessage(pipe, buf, timeoutMS)
	if err != nil {
		return protocol.Frame{}, err
	}
	if n != protocol.FrameSize {
		return protocol.Frame{}, errors.New("received non-frame message on frame path")
	}
	var frame protocol.Frame
	copy(frame[:], buf[:n])
	return frame, nil
}

func pipeWriteFrame(pipe syscall.Handle, frame protocol.Frame) error {
	return pipeWriteMessage(pipe, frame[:])
}

func setPipeMode(pipe syscall.Handle, waitMode uint32) error {
	mode := pipeReadModeMessage | waitMode
	r, _, lastErr := procSetNamedPipeHandState.Call(uintptr(pipe), uintptr(unsafe.Pointer(&mode)), 0, 0)
	if r == 0 {
		return fmt.Errorf("SetNamedPipeHandleState failed: %w", lastErr)
	}
	return nil
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

func serverHandshake(server *Server, pipe syscall.Handle, timeoutMS uint32) (negotiationResult, error) {
	if server == nil || server.listener == nil {
		return negotiationResult{}, errors.New("listener is not available")
	}
	listener := server.listener
	helloFrame, err := pipeReadFrame(pipe, timeoutMS)
	if err != nil {
		return negotiationResult{}, err
	}
	hello, err := decodeHelloNeg(helloFrame)
	if err != nil {
		return negotiationResult{}, err
	}
	localMaxRequestMessageLen, err := computeMaxMessageLen(
		listener.maxRequestPayloadBytes,
		listener.maxRequestBatchItems,
	)
	if err != nil {
		return negotiationResult{}, err
	}
	localMaxResponseMessageLen, err := computeMaxMessageLen(
		listener.maxResponsePayloadBytes,
		listener.maxResponseBatchItems,
	)
	if err != nil {
		return negotiationResult{}, err
	}
	localPacketSize, err := computePipePacketSize(localMaxRequestMessageLen, localMaxResponseMessageLen)
	if err != nil {
		return negotiationResult{}, err
	}

	ack := protocol.HelloAckPayload{
		LayoutVersion:               hello.LayoutVersion,
		Flags:                       0,
		ServerSupported:             listener.supportedProfiles,
		Intersection:                hello.Supported & listener.supportedProfiles,
		Selected:                    0,
		AgreedMaxRequestPayload:     negotiateLimit(hello.MaxRequestPayloadBytes, listener.maxRequestPayloadBytes),
		AgreedMaxRequestBatchItems:  negotiateLimit(hello.MaxRequestBatchItems, listener.maxRequestBatchItems),
		AgreedMaxResponsePayload:    negotiateLimit(hello.MaxResponsePayloadBytes, listener.maxResponsePayloadBytes),
		AgreedMaxResponseBatchItems: negotiateLimit(hello.MaxResponseBatchItems, listener.maxResponseBatchItems),
		AgreedPacketSize:            negotiateLimit(hello.PacketSize, localPacketSize),
	}
	status := negStatusOK

	if listener.authToken != 0 && hello.AuthToken != listener.authToken {
		status = errorAccessDenied
	} else {
		candidates := ack.Intersection & listener.preferredProfiles
		if candidates == 0 {
			candidates = ack.Intersection
		}
		ack.Selected = selectProfile(candidates)
		if ack.Selected == 0 {
			status = errorNotSupported
		} else if ack.AgreedMaxRequestPayload == 0 || ack.AgreedMaxRequestBatchItems == 0 ||
			ack.AgreedMaxResponsePayload == 0 || ack.AgreedMaxResponseBatchItems == 0 ||
			ack.AgreedPacketSize == 0 {
			status = errorInvalidParameter
		}
	}

	if err := pipeWriteFrame(pipe, encodeAckNeg(ack, status)); err != nil {
		return negotiationResult{}, err
	}
	if status != negStatusOK {
		return negotiationResult{}, fmt.Errorf("negotiation failed: status %d", status)
	}
	maxRequestMessageLen, err := computeMaxMessageLen(ack.AgreedMaxRequestPayload, ack.AgreedMaxRequestBatchItems)
	if err != nil {
		return negotiationResult{}, err
	}
	maxResponseMessageLen, err := computeMaxMessageLen(ack.AgreedMaxResponsePayload, ack.AgreedMaxResponseBatchItems)
	if err != nil {
		return negotiationResult{}, err
	}
	if int(ack.AgreedPacketSize) > max(maxRequestMessageLen, maxResponseMessageLen) {
		return negotiationResult{}, errors.New("invalid negotiated packet size")
	}

	return negotiationResult{
		profile:                       ack.Selected,
		agreedMaxRequestPayloadBytes:  ack.AgreedMaxRequestPayload,
		agreedMaxRequestBatchItems:    ack.AgreedMaxRequestBatchItems,
		agreedMaxResponsePayloadBytes: ack.AgreedMaxResponsePayload,
		agreedMaxResponseBatchItems:   ack.AgreedMaxResponseBatchItems,
		packetSize:                    ack.AgreedPacketSize,
		maxRequestMessageLen:          maxRequestMessageLen,
		maxResponseMessageLen:         maxResponseMessageLen,
	}, nil
}

func clientHandshake(client *Client, pipe syscall.Handle, timeoutMS uint32) (negotiationResult, error) {
	localPacketSize, err := computePipePacketSize(client.maxRequestMessageLen, client.maxResponseMessageLen)
	if err != nil {
		return negotiationResult{}, err
	}
	hello := protocol.HelloPayload{
		LayoutVersion:           protocol.MessageVersion,
		Flags:                   0,
		Supported:               client.supportedProfiles,
		Preferred:               client.preferredProfiles,
		MaxRequestPayloadBytes:  effectivePayloadLimit(client.config.MaxRequestPayloadBytes),
		MaxRequestBatchItems:    effectiveBatchLimit(client.config.MaxRequestBatchItems),
		MaxResponsePayloadBytes: effectivePayloadLimit(client.config.MaxResponsePayloadBytes),
		MaxResponseBatchItems:   effectiveBatchLimit(client.config.MaxResponseBatchItems),
		AuthToken:               client.authToken,
		PacketSize:              localPacketSize,
	}
	if err := pipeWriteFrame(pipe, encodeHelloNeg(hello)); err != nil {
		return negotiationResult{}, err
	}
	ackFrame, err := pipeReadFrame(pipe, timeoutMS)
	if err != nil {
		return negotiationResult{}, err
	}
	ack, status, err := decodeAckNeg(ackFrame)
	if err != nil {
		return negotiationResult{}, err
	}
	if status != negStatusOK {
		return negotiationResult{}, fmt.Errorf("server rejected: status %d", status)
	}
	if ack.Selected == 0 || (ack.Selected&client.supportedProfiles) == 0 || (ack.Intersection&client.supportedProfiles) == 0 ||
		ack.AgreedMaxRequestPayload == 0 || ack.AgreedMaxRequestBatchItems == 0 ||
		ack.AgreedMaxResponsePayload == 0 || ack.AgreedMaxResponseBatchItems == 0 ||
		ack.AgreedPacketSize == 0 || ack.AgreedPacketSize > localPacketSize {
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
	if int(ack.AgreedPacketSize) > max(maxRequestMessageLen, maxResponseMessageLen) {
		return negotiationResult{}, errors.New("invalid negotiated packet size")
	}
	return negotiationResult{
		profile:                       ack.Selected,
		agreedMaxRequestPayloadBytes:  ack.AgreedMaxRequestPayload,
		agreedMaxRequestBatchItems:    ack.AgreedMaxRequestBatchItems,
		agreedMaxResponsePayloadBytes: ack.AgreedMaxResponsePayload,
		agreedMaxResponseBatchItems:   ack.AgreedMaxResponseBatchItems,
		packetSize:                    ack.AgreedPacketSize,
		maxRequestMessageLen:          maxRequestMessageLen,
		maxResponseMessageLen:         maxResponseMessageLen,
	}, nil
}

// ---------------------------------------------------------------------------
// SHM region access helpers
// ---------------------------------------------------------------------------

func shmLoadI64(base uintptr, off int) int64 {
	return atomic.LoadInt64((*int64)(unsafe.Pointer(base + uintptr(off))))
}

func shmStoreI64(base uintptr, off int, val int64) {
	atomic.StoreInt64((*int64)(unsafe.Pointer(base+uintptr(off))), val)
}

func shmLoadI32(base uintptr, off int) int32 {
	return atomic.LoadInt32((*int32)(unsafe.Pointer(base + uintptr(off))))
}

func shmStoreI32(base uintptr, off int, val int32) {
	atomic.StoreInt32((*int32)(unsafe.Pointer(base+uintptr(off))), val)
}

func shmCopyFrame(base uintptr, off int) protocol.Frame {
	var f protocol.Frame
	src := (*[protocol.FrameSize]byte)(unsafe.Pointer(base + uintptr(off)))
	copy(f[:], src[:])
	return f
}

func shmWriteFrame(base uintptr, off int, frame protocol.Frame) {
	dst := (*[protocol.FrameSize]byte)(unsafe.Pointer(base + uintptr(off)))
	copy(dst[:], frame[:])
}

// ---------------------------------------------------------------------------
// SHM Server
// ---------------------------------------------------------------------------

type shmServer struct {
	mapping               syscall.Handle
	requestEvent          syscall.Handle
	responseEvent         syscall.Handle
	region                uintptr
	mappingLen            int
	lastReqSeq            int64
	activeReqSeq          int64
	spinTries             uint32
	maxRequestMessageLen  int
	maxResponseMessageLen int
}

func newSHMServer(config *Config, profile uint32) (*shmServer, error) {
	mappingName := buildKernelObjectName(config, profile, "shm")
	reqEventName := buildKernelObjectName(config, profile, "req")
	respEventName := buildKernelObjectName(config, profile, "resp")
	maxRequestMessageLen, err := computeMaxMessageLen(
		effectivePayloadLimit(config.MaxRequestPayloadBytes),
		effectiveBatchLimit(config.MaxRequestBatchItems),
	)
	if err != nil {
		return nil, err
	}
	maxResponseMessageLen, err := computeMaxMessageLen(
		effectivePayloadLimit(config.MaxResponsePayloadBytes),
		effectiveBatchLimit(config.MaxResponseBatchItems),
	)
	if err != nil {
		return nil, err
	}
	requestOffset, responseOffset, mappingLen, err := computeRegionLayout(maxRequestMessageLen, maxResponseMessageLen)
	if err != nil {
		return nil, err
	}

	mapping, _, lastErr := procCreateFileMappingW.Call(
		uintptr(invalidHandleValue), 0, uintptr(pageReadWrite),
		uintptr(uint64(mappingLen)>>32), uintptr(uint64(mappingLen)&0xffffffff), uintptr(unsafe.Pointer(toUTF16(mappingName))),
	)
	if mapping == 0 {
		return nil, fmt.Errorf("CreateFileMappingW: %w", lastErr)
	}
	if lastErr == syscall.Errno(errorAlreadyExists) {
		closeHandle(syscall.Handle(mapping))
		return nil, errors.New("SHM already exists")
	}

	region, _, lastErr := procMapViewOfFile.Call(mapping, uintptr(fileMapAllAccess), 0, 0, uintptr(mappingLen))
	if region == 0 {
		closeHandle(syscall.Handle(mapping))
		return nil, fmt.Errorf("MapViewOfFile: %w", lastErr)
	}

	// Zero region
	for i := 0; i < mappingLen; i++ {
		*(*byte)(unsafe.Pointer(region + uintptr(i))) = 0
	}

	// Init header
	spin := effectiveSpinTries(config)
	binary.LittleEndian.PutUint32((*[4]byte)(unsafe.Pointer(region + uintptr(offHdrMagic)))[:], shmRegionMagic)
	binary.LittleEndian.PutUint32((*[4]byte)(unsafe.Pointer(region + uintptr(offHdrVersion)))[:], shmRegionVersion)
	binary.LittleEndian.PutUint32((*[4]byte)(unsafe.Pointer(region + uintptr(offHdrHeaderLen)))[:], hdrSize)
	binary.LittleEndian.PutUint32((*[4]byte)(unsafe.Pointer(region + uintptr(offHdrProfile)))[:], profile)
	binary.LittleEndian.PutUint32((*[4]byte)(unsafe.Pointer(region + uintptr(offHdrReqOffset)))[:], uint32(requestOffset))
	binary.LittleEndian.PutUint32((*[4]byte)(unsafe.Pointer(region + uintptr(offHdrReqCapacity)))[:], uint32(maxRequestMessageLen))
	binary.LittleEndian.PutUint32((*[4]byte)(unsafe.Pointer(region + uintptr(offHdrRespOffset)))[:], uint32(responseOffset))
	binary.LittleEndian.PutUint32((*[4]byte)(unsafe.Pointer(region + uintptr(offHdrRespCapacity)))[:], uint32(maxResponseMessageLen))
	binary.LittleEndian.PutUint32((*[4]byte)(unsafe.Pointer(region + uintptr(offHdrSpinTries)))[:], spin)

	reqEvent, _, lastErr := procCreateEventW.Call(0, 0, 0, uintptr(unsafe.Pointer(toUTF16(reqEventName))))
	if reqEvent == 0 {
		procUnmapViewOfFile.Call(region)
		closeHandle(syscall.Handle(mapping))
		return nil, fmt.Errorf("CreateEventW(req): %w", lastErr)
	}
	respEvent, _, lastErr := procCreateEventW.Call(0, 0, 0, uintptr(unsafe.Pointer(toUTF16(respEventName))))
	if respEvent == 0 {
		closeHandle(syscall.Handle(reqEvent))
		procUnmapViewOfFile.Call(region)
		closeHandle(syscall.Handle(mapping))
		return nil, fmt.Errorf("CreateEventW(resp): %w", lastErr)
	}

	return &shmServer{
		mapping:               syscall.Handle(mapping),
		requestEvent:          syscall.Handle(reqEvent),
		responseEvent:         syscall.Handle(respEvent),
		region:                region,
		mappingLen:            mappingLen,
		spinTries:             spin,
		maxRequestMessageLen:  maxRequestMessageLen,
		maxResponseMessageLen: maxResponseMessageLen,
	}, nil
}

func (s *shmServer) receiveBytes(message []byte, messageCapacity int, timeoutMS uint32) (int, error) {
	if messageCapacity > len(message) {
		return 0, errors.New("message buffer is smaller than requested capacity")
	}
	if messageCapacity <= 0 {
		return 0, errors.New("message capacity must not be zero")
	}
	if messageCapacity > s.maxRequestMessageLen {
		return 0, errors.New("message capacity exceeds negotiated request size")
	}
	if len(message) < messageCapacity {
		return 0, errors.New("message buffer is smaller than negotiated request size")
	}

	var deadline uint64
	if timeoutMS != 0 {
		deadline = nowMS() + uint64(timeoutMS)
	}
	spins := s.spinTries

	for {
		current := shmLoadI64(s.region, offReqSeq)
		if current != s.lastReqSeq {
			messageLen := shmLoadI32(s.region, offReqLen)
			if messageLen < 0 || int(messageLen) > messageCapacity || int(messageLen) > requestCapacity(s.region) {
				return 0, errors.New("invalid SHM request length")
			}
			copy(message[:messageLen], unsafe.Slice((*byte)(unsafe.Pointer(s.region+uintptr(requestArea(s.region)))), int(messageLen)))
			s.activeReqSeq = current
			s.lastReqSeq = current
			return int(messageLen), nil
		}

		if shmLoadI32(s.region, offReqClientClosed) != 0 {
			return 0, io.EOF
		}

		runtime.Gosched()

		if spins != 0 {
			spins--
			continue
		}

		// Mark waiting
		shmStoreI32(s.region, offReqServerWait, 1)

		current = shmLoadI64(s.region, offReqSeq)
		if current != s.lastReqSeq {
			shmStoreI32(s.region, offReqServerWait, 0)
			messageLen := shmLoadI32(s.region, offReqLen)
			if messageLen < 0 || int(messageLen) > messageCapacity || int(messageLen) > requestCapacity(s.region) {
				return 0, errors.New("invalid SHM request length")
			}
			copy(message[:messageLen], unsafe.Slice((*byte)(unsafe.Pointer(s.region+uintptr(requestArea(s.region)))), int(messageLen)))
			s.activeReqSeq = current
			s.lastReqSeq = current
			return int(messageLen), nil
		}

		waitMS := uint32(infinite)
		if deadline != 0 {
			now := nowMS()
			if now >= deadline {
				shmStoreI32(s.region, offReqServerWait, 0)
				return 0, errors.New("SHM receive timeout")
			}
			waitMS = uint32(deadline - now)
		}

		rc, _, lastErr := procWaitForSingleObject.Call(uintptr(s.requestEvent), uintptr(waitMS))
		shmStoreI32(s.region, offReqServerWait, 0)

		if uint32(rc) == waitObject0 {
			spins = s.spinTries
			continue
		}
		if uint32(rc) == waitTimeout {
			return 0, errors.New("SHM receive timeout")
		}
		return 0, fmt.Errorf("WaitForSingleObject(req): %w", lastErr)
	}
}

func (s *shmServer) receiveMessage(message []byte, timeoutMS uint32) (int, error) {
	messageLen, err := s.receiveBytes(message, s.maxRequestMessageLen, timeoutMS)
	if err != nil {
		return 0, err
	}
	if err := validateReceivedMessage(message, messageLen, s.maxRequestMessageLen); err != nil {
		return 0, err
	}
	return messageLen, nil
}

func (s *shmServer) sendBytes(message []byte, messageCapacity int) error {
	if s.activeReqSeq == 0 {
		return errors.New("no active request")
	}
	if messageCapacity <= 0 {
		return errors.New("message capacity must not be zero")
	}
	if messageCapacity > s.maxResponseMessageLen {
		return errors.New("message capacity exceeds negotiated response size")
	}
	if len(message) == 0 {
		return errors.New("message must not be empty")
	}
	if len(message) > messageCapacity {
		return errors.New("message exceeds negotiated size")
	}
	copy(unsafe.Slice((*byte)(unsafe.Pointer(s.region+uintptr(responseArea(s.region)))), len(message)), message)
	shmStoreI32(s.region, offRespLen, int32(len(message)))
	shmStoreI64(s.region, offRespSeq, s.activeReqSeq)

	// Conditional SetEvent
	if shmLoadI32(s.region, offRespClientWait) != 0 {
		procSetEvent.Call(uintptr(s.responseEvent))
	}
	s.activeReqSeq = 0
	return nil
}

func (s *shmServer) sendMessage(message []byte) error {
	if err := validateMessageForSend(message, s.maxResponseMessageLen); err != nil {
		return err
	}
	return s.sendBytes(message, s.maxResponseMessageLen)
}

func (s *shmServer) receiveFrame(timeoutMS uint32) (protocol.Frame, error) {
	if s.maxRequestMessageLen < protocol.FrameSize {
		return protocol.Frame{}, errors.New("negotiated request size is smaller than frame size")
	}
	var message protocol.Frame
	messageLen, err := s.receiveBytes(message[:], protocol.FrameSize, timeoutMS)
	if err != nil {
		return protocol.Frame{}, err
	}
	if messageLen != protocol.FrameSize {
		return protocol.Frame{}, errors.New("invalid SHM frame length")
	}
	return message, nil
}

func (s *shmServer) sendFrame(frame protocol.Frame) error {
	if s.maxResponseMessageLen < protocol.FrameSize {
		return errors.New("negotiated response size is smaller than frame size")
	}
	return s.sendBytes(frame[:], protocol.FrameSize)
}

func (s *shmServer) close() {
	if s.region != 0 {
		shmStoreI32(s.region, offRespServerClosed, 1)
		procSetEvent.Call(uintptr(s.requestEvent))
		procSetEvent.Call(uintptr(s.responseEvent))
		procUnmapViewOfFile.Call(s.region)
	}
	closeHandle(s.responseEvent)
	closeHandle(s.requestEvent)
	closeHandle(s.mapping)
}

// ---------------------------------------------------------------------------
// SHM Client
// ---------------------------------------------------------------------------

type shmClient struct {
	mapping               syscall.Handle
	requestEvent          syscall.Handle
	responseEvent         syscall.Handle
	region                uintptr
	mappingLen            int
	nextReqSeq            int64
	spinTries             uint32
	maxRequestMessageLen  int
	maxResponseMessageLen int
}

func newSHMClient(config *Config, profile uint32, timeoutMS uint32) (*shmClient, error) {
	mappingName := buildKernelObjectName(config, profile, "shm")
	reqEventName := buildKernelObjectName(config, profile, "req")
	respEventName := buildKernelObjectName(config, profile, "resp")
	maxRequestMessageLen, err := computeMaxMessageLen(
		effectivePayloadLimit(config.MaxRequestPayloadBytes),
		effectiveBatchLimit(config.MaxRequestBatchItems),
	)
	if err != nil {
		return nil, err
	}
	maxResponseMessageLen, err := computeMaxMessageLen(
		effectivePayloadLimit(config.MaxResponsePayloadBytes),
		effectiveBatchLimit(config.MaxResponseBatchItems),
	)
	if err != nil {
		return nil, err
	}
	requestOffset, responseOffset, mappingLen, err := computeRegionLayout(maxRequestMessageLen, maxResponseMessageLen)
	if err != nil {
		return nil, err
	}

	var deadline uint64
	if timeoutMS != 0 {
		deadline = nowMS() + uint64(timeoutMS)
	}

	var mapping, reqEvent, respEvent uintptr
	for {
		m, _, _ := procOpenFileMappingW.Call(uintptr(fileMapAllAccess), 0, uintptr(unsafe.Pointer(toUTF16(mappingName))))
		if m != 0 {
			re, _, _ := procOpenEventW.Call(uintptr(synchronize|eventModifyState), 0, uintptr(unsafe.Pointer(toUTF16(reqEventName))))
			rsp, _, _ := procOpenEventW.Call(uintptr(synchronize|eventModifyState), 0, uintptr(unsafe.Pointer(toUTF16(respEventName))))
			if re != 0 && rsp != 0 {
				mapping = m
				reqEvent = re
				respEvent = rsp
				break
			}
			closeHandle(syscall.Handle(rsp))
			closeHandle(syscall.Handle(re))
			closeHandle(syscall.Handle(m))
		}
		if deadline != 0 && nowMS() >= deadline {
			return nil, errors.New("SHM connect timeout")
		}
		sleepMS(1)
	}

	region, _, lastErr := procMapViewOfFile.Call(mapping, uintptr(fileMapAllAccess), 0, 0, uintptr(mappingLen))
	if region == 0 {
		closeHandle(syscall.Handle(respEvent))
		closeHandle(syscall.Handle(reqEvent))
		closeHandle(syscall.Handle(mapping))
		return nil, fmt.Errorf("MapViewOfFile: %w", lastErr)
	}

	var regionSpin uint32
	for {
		spin, err := validateRegionHeader(region, mappingLen, profile)
		if err == nil {
			regionSpin = spin
			break
		}
		if deadline != 0 && nowMS() >= deadline {
			procUnmapViewOfFile.Call(region)
			closeHandle(syscall.Handle(respEvent))
			closeHandle(syscall.Handle(reqEvent))
			closeHandle(syscall.Handle(mapping))
			return nil, errors.New("SHM region not ready")
		}
		sleepMS(1)
	}
	if requestArea(region) != requestOffset || responseArea(region) != responseOffset ||
		requestCapacity(region) < maxRequestMessageLen || responseCapacity(region) < maxResponseMessageLen {
		procUnmapViewOfFile.Call(region)
		closeHandle(syscall.Handle(respEvent))
		closeHandle(syscall.Handle(reqEvent))
		closeHandle(syscall.Handle(mapping))
		return nil, errors.New("SHM region capacity mismatch")
	}
	spin := regionSpin
	if spin == 0 {
		spin = effectiveSpinTries(config)
	}

	nextReqSeq := shmLoadI64(region, offReqSeq)

	return &shmClient{
		mapping:               syscall.Handle(mapping),
		requestEvent:          syscall.Handle(reqEvent),
		responseEvent:         syscall.Handle(respEvent),
		region:                region,
		mappingLen:            mappingLen,
		nextReqSeq:            nextReqSeq,
		spinTries:             spin,
		maxRequestMessageLen:  maxRequestMessageLen,
		maxResponseMessageLen: maxResponseMessageLen,
	}, nil
}

func (c *shmClient) callBytes(request []byte, response []byte, requestCapacity int, responseCapacityLimit int, timeoutMS uint32) (int, error) {
	if requestCapacity <= 0 || responseCapacityLimit <= 0 {
		return 0, errors.New("message capacity must not be zero")
	}
	if requestCapacity > c.maxRequestMessageLen || responseCapacityLimit > c.maxResponseMessageLen {
		return 0, errors.New("message capacity exceeds negotiated limits")
	}
	if len(request) == 0 {
		return 0, errors.New("message must not be empty")
	}
	if len(request) > requestCapacity {
		return 0, errors.New("message exceeds negotiated size")
	}
	if len(response) < responseCapacityLimit {
		return 0, errors.New("response buffer is smaller than negotiated response size")
	}

	reqSeq := c.nextReqSeq + 1
	c.nextReqSeq = reqSeq

	copy(unsafe.Slice((*byte)(unsafe.Pointer(c.region+uintptr(requestArea(c.region)))), len(request)), request)
	shmStoreI32(c.region, offReqLen, int32(len(request)))
	shmStoreI64(c.region, offReqSeq, reqSeq)

	// Conditional SetEvent
	if shmLoadI32(c.region, offReqServerWait) != 0 {
		procSetEvent.Call(uintptr(c.requestEvent))
	}

	// Wait for response
	var deadline uint64
	if timeoutMS != 0 {
		deadline = nowMS() + uint64(timeoutMS)
	}
	spins := c.spinTries

	for {
		current := shmLoadI64(c.region, offRespSeq)
		if current >= reqSeq {
			responseLen := shmLoadI32(c.region, offRespLen)
			if responseLen < 0 || int(responseLen) > responseCapacityLimit || int(responseLen) > responseCapacity(c.region) {
				return 0, errors.New("invalid SHM response length")
			}
			copy(response[:responseLen], unsafe.Slice((*byte)(unsafe.Pointer(c.region+uintptr(responseArea(c.region)))), int(responseLen)))
			return int(responseLen), nil
		}

		if shmLoadI32(c.region, offRespServerClosed) != 0 {
			return 0, errors.New("server closed")
		}

		runtime.Gosched()

		if spins != 0 {
			spins--
			continue
		}

		shmStoreI32(c.region, offRespClientWait, 1)

		current = shmLoadI64(c.region, offRespSeq)
		if current >= reqSeq {
			shmStoreI32(c.region, offRespClientWait, 0)
			responseLen := shmLoadI32(c.region, offRespLen)
			if responseLen < 0 || int(responseLen) > responseCapacityLimit || int(responseLen) > responseCapacity(c.region) {
				return 0, errors.New("invalid SHM response length")
			}
			copy(response[:responseLen], unsafe.Slice((*byte)(unsafe.Pointer(c.region+uintptr(responseArea(c.region)))), int(responseLen)))
			return int(responseLen), nil
		}

		waitMS := uint32(infinite)
		if deadline != 0 {
			now := nowMS()
			if now >= deadline {
				shmStoreI32(c.region, offRespClientWait, 0)
				return 0, errors.New("SHM response timeout")
			}
			waitMS = uint32(deadline - now)
		}

		rc, _, lastErr := procWaitForSingleObject.Call(uintptr(c.responseEvent), uintptr(waitMS))
		shmStoreI32(c.region, offRespClientWait, 0)

		if uint32(rc) == waitObject0 {
			spins = c.spinTries
			continue
		}
		if uint32(rc) == waitTimeout {
			return 0, errors.New("SHM response timeout")
		}
		return 0, fmt.Errorf("WaitForSingleObject(resp): %w", lastErr)
	}
}

func (c *shmClient) callMessage(request []byte, response []byte, timeoutMS uint32) (int, error) {
	if err := validateMessageForSend(request, c.maxRequestMessageLen); err != nil {
		return 0, err
	}
	responseLen, err := c.callBytes(request, response, c.maxRequestMessageLen, c.maxResponseMessageLen, timeoutMS)
	if err != nil {
		return 0, err
	}
	if err := validateReceivedMessage(response, responseLen, c.maxResponseMessageLen); err != nil {
		return 0, err
	}
	return responseLen, nil
}

func (c *shmClient) callFrame(request protocol.Frame, timeoutMS uint32) (protocol.Frame, error) {
	if c.maxRequestMessageLen < protocol.FrameSize || c.maxResponseMessageLen < protocol.FrameSize {
		return protocol.Frame{}, errors.New("negotiated frame size exceeds SHM capacities")
	}
	var response protocol.Frame
	responseLen, err := c.callBytes(request[:], response[:], protocol.FrameSize, protocol.FrameSize, timeoutMS)
	if err != nil {
		return protocol.Frame{}, err
	}
	if responseLen != protocol.FrameSize {
		return protocol.Frame{}, errors.New("invalid SHM frame length")
	}
	return response, nil
}

func (c *shmClient) close() {
	if c.region != 0 {
		shmStoreI32(c.region, offReqClientClosed, 1)
		procSetEvent.Call(uintptr(c.requestEvent))
		procSetEvent.Call(uintptr(c.responseEvent))
		procUnmapViewOfFile.Call(c.region)
	}
	closeHandle(c.responseEvent)
	closeHandle(c.requestEvent)
	closeHandle(c.mapping)
}

// ---------------------------------------------------------------------------
// Public API: Server
// ---------------------------------------------------------------------------

// Server is a Named Pipe server that may upgrade to SHM HYBRID.
type Server struct {
	listener *Listener
	session  *Session
}

type Listener struct {
	pipe                    syscall.Handle
	config                  Config
	supportedProfiles       uint32
	preferredProfiles       uint32
	authToken               uint64
	maxRequestPayloadBytes  uint32
	maxRequestBatchItems    uint32
	maxResponsePayloadBytes uint32
	maxResponseBatchItems   uint32
}

type Session struct {
	pipe                  syscall.Handle
	shm                   *shmServer
	negotiatedProfile     uint32
	packetSize            uint32
	maxRequestMessageLen  int
	maxResponseMessageLen int
	connected             bool
}

// Listen creates a Named Pipe server.
func NewListener(config Config) (*Listener, error) {
	if config.RunDir == "" || config.ServiceName == "" {
		return nil, errors.New("run_dir and service_name must be set")
	}
	pipeName := buildPipeName(&config)
	supported := effectiveSupported(&config)
	preferred := effectivePreferred(&config, supported)
	maxRequestMessageLen, err := computeMaxMessageLen(
		effectivePayloadLimit(config.MaxRequestPayloadBytes),
		effectiveBatchLimit(config.MaxRequestBatchItems),
	)
	if err != nil {
		return nil, err
	}
	maxResponseMessageLen, err := computeMaxMessageLen(
		effectivePayloadLimit(config.MaxResponsePayloadBytes),
		effectiveBatchLimit(config.MaxResponseBatchItems),
	)
	if err != nil {
		return nil, err
	}
	maxDefaultMessageLen := maxRequestMessageLen
	if maxResponseMessageLen > maxDefaultMessageLen {
		maxDefaultMessageLen = maxResponseMessageLen
	}

	pipe, _, lastErr := procCreateNamedPipeW.Call(
		uintptr(unsafe.Pointer(toUTF16(pipeName))),
		uintptr(pipeAccessDuplex|fileFlagFirstPipeInstance),
		uintptr(pipeTypeMessage|pipeReadModeMessage|pipeWait),
		1,
		uintptr(maxDefaultMessageLen),
		uintptr(maxDefaultMessageLen),
		0, 0,
	)
	if syscall.Handle(pipe) == invalidHandleValue {
		return nil, fmt.Errorf("CreateNamedPipeW: %w", lastErr)
	}

	return &Listener{
		pipe:                    syscall.Handle(pipe),
		config:                  config,
		supportedProfiles:       supported,
		preferredProfiles:       preferred,
		authToken:               config.AuthToken,
		maxRequestPayloadBytes:  effectivePayloadLimit(config.MaxRequestPayloadBytes),
		maxRequestBatchItems:    effectiveBatchLimit(config.MaxRequestBatchItems),
		maxResponsePayloadBytes: effectivePayloadLimit(config.MaxResponsePayloadBytes),
		maxResponseBatchItems:   effectiveBatchLimit(config.MaxResponseBatchItems),
	}, nil
}

func Listen(config Config) (*Server, error) {
	listener, err := NewListener(config)
	if err != nil {
		return nil, err
	}
	return &Server{listener: listener}, nil
}

// Accept waits for a client to connect and performs negotiation.
func (l *Listener) Accept(timeout time.Duration) (*Session, error) {
	timeoutMS := uint32(0)
	if timeout > 0 {
		timeoutMS = uint32(timeout.Milliseconds())
	}

	if err := setPipeMode(l.pipe, pipeNoWait); err != nil {
		return nil, err
	}

	var deadline uint64
	if timeoutMS != 0 {
		deadline = nowMS() + uint64(timeoutMS)
	}

	for {
		r, _, lastErr := procConnectNamedPipe.Call(uintptr(l.pipe), 0)
		if r != 0 {
			break
		}
		errCode, _ := lastErr.(syscall.Errno)
		if errCode == syscall.Errno(errorPipeConnected) {
			break
		}
		if errCode != syscall.Errno(errorPipeListening) && errCode != syscall.Errno(errorNoData) {
			_ = setPipeMode(l.pipe, pipeWait)
			return nil, fmt.Errorf("ConnectNamedPipe: %w", lastErr)
		}
		if deadline != 0 && nowMS() >= deadline {
			_ = setPipeMode(l.pipe, pipeWait)
			return nil, errors.New("accept timeout")
		}
		sleepMS(1)
	}

	if err := setPipeMode(l.pipe, pipeWait); err != nil {
		return nil, err
	}

	serverForHandshake := &Server{
		listener: l,
		session:  nil,
	}
	negotiated, err := serverHandshake(serverForHandshake, l.pipe, timeoutMS)
	if err != nil {
		procDisconnectNamedPipe.Call(uintptr(l.pipe))
		return nil, err
	}

	session := &Session{
		pipe:                  l.pipe,
		shm:                   nil,
		negotiatedProfile:     negotiated.profile,
		packetSize:            negotiated.packetSize,
		maxRequestMessageLen:  negotiated.maxRequestMessageLen,
		maxResponseMessageLen: negotiated.maxResponseMessageLen,
		connected:             true,
	}

	if isSHMProfile(negotiated.profile) {
		shmConfig := l.config
		shmConfig.MaxRequestPayloadBytes = negotiated.agreedMaxRequestPayloadBytes
		shmConfig.MaxRequestBatchItems = negotiated.agreedMaxRequestBatchItems
		shmConfig.MaxResponsePayloadBytes = negotiated.agreedMaxResponsePayloadBytes
		shmConfig.MaxResponseBatchItems = negotiated.agreedMaxResponseBatchItems
		shm, err := newSHMServer(&shmConfig, negotiated.profile)
		if err != nil {
			procDisconnectNamedPipe.Call(uintptr(l.pipe))
			return nil, err
		}
		session.shm = shm
	}

	return session, nil
}

func (sess *Session) ReceiveMessage(message []byte, timeout time.Duration) (int, error) {
	timeoutMS := uint32(0)
	if timeout > 0 {
		timeoutMS = uint32(timeout.Milliseconds())
	}
	if sess.shm != nil {
		return sess.shm.receiveMessage(message, timeoutMS)
	}
	if sess.maxRequestMessageLen == 0 || len(message) < sess.maxRequestMessageLen {
		return 0, errors.New("message buffer is smaller than negotiated request size")
	}
	return recvTransportMessage(sess.pipe, message, sess.maxRequestMessageLen, sess.packetSize, timeoutMS)
}

// ReceiveFrame receives a single frame.
func (sess *Session) ReceiveFrame(timeout time.Duration) (protocol.Frame, error) {
	timeoutMS := uint32(0)
	if timeout > 0 {
		timeoutMS = uint32(timeout.Milliseconds())
	}
	if sess.shm != nil {
		return sess.shm.receiveFrame(timeoutMS)
	}
	return pipeReadFrame(sess.pipe, timeoutMS)
}

func (sess *Session) SendMessage(message []byte, timeout time.Duration) error {
	_ = timeout
	if sess.shm != nil {
		return sess.shm.sendMessage(message)
	}
	if sess.maxResponseMessageLen == 0 {
		return errors.New("negotiated response size is not available")
	}
	return sendTransportMessage(sess.pipe, message, sess.maxResponseMessageLen, sess.packetSize)
}

// SendFrame sends a single frame.
func (sess *Session) SendFrame(frame protocol.Frame, timeout time.Duration) error {
	if sess.shm != nil {
		return sess.shm.sendFrame(frame)
	}
	return pipeWriteFrame(sess.pipe, frame)
}

// ReceiveIncrement receives an increment request.
func (sess *Session) ReceiveIncrement(timeout time.Duration) (uint64, protocol.IncrementRequest, error) {
	frame, err := sess.ReceiveFrame(timeout)
	if err != nil {
		return 0, protocol.IncrementRequest{}, err
	}
	return protocol.DecodeIncrementRequest(frame)
}

// SendIncrement sends an increment response.
func (sess *Session) SendIncrement(requestID uint64, response protocol.IncrementResponse, timeout time.Duration) error {
	return sess.SendFrame(protocol.EncodeIncrementResponse(requestID, response), timeout)
}

// NegotiatedProfile returns the negotiated profile.
func (sess *Session) NegotiatedProfile() uint32 {
	return sess.negotiatedProfile
}

// Close releases all resources.
func (sess *Session) Close() error {
	if sess.shm != nil {
		sess.shm.close()
		sess.shm = nil
	}
	if sess.connected {
		procFlushFileBuffers.Call(uintptr(sess.pipe))
		procDisconnectNamedPipe.Call(uintptr(sess.pipe))
		sess.connected = false
	}
	return nil
}

func (l *Listener) Close() error {
	if l.pipe != invalidHandleValue {
		closeHandle(l.pipe)
		l.pipe = invalidHandleValue
	}
	return nil
}

func (s *Server) Accept(timeout time.Duration) error {
	if s.listener == nil {
		return errors.New("server is closed")
	}
	if s.session != nil {
		return errors.New("server is already connected")
	}
	session, err := s.listener.Accept(timeout)
	if err != nil {
		return err
	}
	s.session = session
	return nil
}

func (s *Server) ReceiveMessage(message []byte, timeout time.Duration) (int, error) {
	if s.session == nil {
		return 0, errors.New("server is not connected")
	}
	return s.session.ReceiveMessage(message, timeout)
}

func (s *Server) ReceiveFrame(timeout time.Duration) (protocol.Frame, error) {
	if s.session == nil {
		return protocol.Frame{}, errors.New("server is not connected")
	}
	return s.session.ReceiveFrame(timeout)
}

func (s *Server) SendMessage(message []byte, timeout time.Duration) error {
	if s.session == nil {
		return errors.New("server is not connected")
	}
	return s.session.SendMessage(message, timeout)
}

func (s *Server) SendFrame(frame protocol.Frame, timeout time.Duration) error {
	if s.session == nil {
		return errors.New("server is not connected")
	}
	return s.session.SendFrame(frame, timeout)
}

func (s *Server) ReceiveIncrement(timeout time.Duration) (uint64, protocol.IncrementRequest, error) {
	if s.session == nil {
		return 0, protocol.IncrementRequest{}, errors.New("server is not connected")
	}
	return s.session.ReceiveIncrement(timeout)
}

func (s *Server) SendIncrement(requestID uint64, response protocol.IncrementResponse, timeout time.Duration) error {
	if s.session == nil {
		return errors.New("server is not connected")
	}
	return s.session.SendIncrement(requestID, response, timeout)
}

func (s *Server) NegotiatedProfile() uint32 {
	if s.session == nil {
		return 0
	}
	return s.session.NegotiatedProfile()
}

func (s *Server) Close() error {
	if s.session != nil {
		_ = s.session.Close()
		s.session = nil
	}
	if s.listener != nil {
		_ = s.listener.Close()
		s.listener = nil
	}
	return nil
}

// ---------------------------------------------------------------------------
// Public API: Client
// ---------------------------------------------------------------------------

// Client is a Named Pipe client that may upgrade to SHM HYBRID.
type Client struct {
	pipe                  syscall.Handle
	config                Config
	supportedProfiles     uint32
	preferredProfiles     uint32
	authToken             uint64
	negotiatedProfile     uint32
	packetSize            uint32
	maxRequestMessageLen  int
	maxResponseMessageLen int
	shm                   *shmClient
	nextRequestID         uint64
}

// Dial connects to a Named Pipe server.
func Dial(config Config, timeout time.Duration) (*Client, error) {
	if config.RunDir == "" || config.ServiceName == "" {
		return nil, errors.New("run_dir and service_name must be set")
	}
	timeoutMS := uint32(0)
	if timeout > 0 {
		timeoutMS = uint32(timeout.Milliseconds())
	}
	pipeName := buildPipeName(&config)
	supported := effectiveSupported(&config)
	preferred := effectivePreferred(&config, supported)
	maxRequestPayloadBytes := effectivePayloadLimit(config.MaxRequestPayloadBytes)
	maxRequestBatchItems := effectiveBatchLimit(config.MaxRequestBatchItems)
	maxResponsePayloadBytes := effectivePayloadLimit(config.MaxResponsePayloadBytes)
	maxResponseBatchItems := effectiveBatchLimit(config.MaxResponseBatchItems)
	maxRequestMessageLen, err := computeMaxMessageLen(maxRequestPayloadBytes, maxRequestBatchItems)
	if err != nil {
		return nil, err
	}
	maxResponseMessageLen, err := computeMaxMessageLen(maxResponsePayloadBytes, maxResponseBatchItems)
	if err != nil {
		return nil, err
	}

	var deadline uint64
	if timeoutMS != 0 {
		deadline = nowMS() + uint64(timeoutMS)
	}

	var pipe syscall.Handle
	for {
		h, _, lastErr := procCreateFileW.Call(
			uintptr(unsafe.Pointer(toUTF16(pipeName))),
			uintptr(genericRead|genericWrite),
			0, 0, uintptr(openExisting), 0, 0,
		)
		if syscall.Handle(h) != invalidHandleValue {
			pipe = syscall.Handle(h)
			break
		}
		errCode, _ := lastErr.(syscall.Errno)
		if errCode != syscall.Errno(errorFileNotFound) && errCode != syscall.Errno(errorPipeBusy) {
			return nil, fmt.Errorf("CreateFileW: %w", lastErr)
		}
		if deadline != 0 && nowMS() >= deadline {
			return nil, errors.New("pipe connect timeout")
		}
		if errCode == syscall.Errno(errorPipeBusy) {
			wait := uint32(nmpwaitWaitForever)
			if timeoutMS != 0 {
				wait = 50
			}
			procWaitNamedPipeW.Call(uintptr(unsafe.Pointer(toUTF16(pipeName))), uintptr(wait))
		} else {
			sleepMS(1)
		}
	}

	if err := setPipeMode(pipe, pipeWait); err != nil {
		closeHandle(pipe)
		return nil, err
	}

	negotiated, err := clientHandshake(&Client{
		pipe:                  pipe,
		config:                config,
		supportedProfiles:     supported,
		preferredProfiles:     preferred,
		authToken:             config.AuthToken,
		negotiatedProfile:     0,
		packetSize:            0,
		maxRequestMessageLen:  maxRequestMessageLen,
		maxResponseMessageLen: maxResponseMessageLen,
		shm:                   nil,
		nextRequestID:         1,
	}, pipe, timeoutMS)
	if err != nil {
		closeHandle(pipe)
		return nil, err
	}

	var shm *shmClient
	if isSHMProfile(negotiated.profile) {
		shmConfig := config
		shmConfig.MaxRequestPayloadBytes = negotiated.agreedMaxRequestPayloadBytes
		shmConfig.MaxRequestBatchItems = negotiated.agreedMaxRequestBatchItems
		shmConfig.MaxResponsePayloadBytes = negotiated.agreedMaxResponsePayloadBytes
		shmConfig.MaxResponseBatchItems = negotiated.agreedMaxResponseBatchItems
		shm, err = newSHMClient(&shmConfig, negotiated.profile, timeoutMS)
		if err != nil {
			closeHandle(pipe)
			return nil, err
		}
	}

	return &Client{
		pipe:                  pipe,
		config:                config,
		supportedProfiles:     supported,
		preferredProfiles:     preferred,
		authToken:             config.AuthToken,
		negotiatedProfile:     negotiated.profile,
		packetSize:            negotiated.packetSize,
		maxRequestMessageLen:  negotiated.maxRequestMessageLen,
		maxResponseMessageLen: negotiated.maxResponseMessageLen,
		shm:                   shm,
		nextRequestID:         1,
	}, nil
}

func (c *Client) CallMessage(request []byte, response []byte, timeout time.Duration) (int, error) {
	timeoutMS := uint32(0)
	if timeout > 0 {
		timeoutMS = uint32(timeout.Milliseconds())
	}
	if c.shm != nil {
		return c.shm.callMessage(request, response, timeoutMS)
	}
	if c.maxRequestMessageLen == 0 || c.maxResponseMessageLen == 0 {
		return 0, errors.New("negotiated message limits are not available")
	}
	if len(response) < c.maxResponseMessageLen {
		return 0, errors.New("response buffer is smaller than negotiated response size")
	}
	if err := sendTransportMessage(c.pipe, request, c.maxRequestMessageLen, c.packetSize); err != nil {
		return 0, err
	}
	return recvTransportMessage(c.pipe, response, c.maxResponseMessageLen, c.packetSize, timeoutMS)
}

// CallFrame sends a request and waits for the response.
func (c *Client) CallFrame(request protocol.Frame, timeout time.Duration) (protocol.Frame, error) {
	timeoutMS := uint32(0)
	if timeout > 0 {
		timeoutMS = uint32(timeout.Milliseconds())
	}
	if c.shm != nil {
		return c.shm.callFrame(request, timeoutMS)
	}
	if err := pipeWriteFrame(c.pipe, request); err != nil {
		return protocol.Frame{}, err
	}
	return pipeReadFrame(c.pipe, timeoutMS)
}

// CallIncrement sends an increment request and returns the response.
func (c *Client) CallIncrement(request protocol.IncrementRequest, timeout time.Duration) (protocol.IncrementResponse, error) {
	reqID := c.nextRequestID
	c.nextRequestID++
	respFrame, err := c.CallFrame(protocol.EncodeIncrementRequest(reqID, request), timeout)
	if err != nil {
		return protocol.IncrementResponse{}, err
	}
	respID, resp, err := protocol.DecodeIncrementResponse(respFrame)
	if err != nil {
		return protocol.IncrementResponse{}, err
	}
	if respID != reqID {
		return protocol.IncrementResponse{}, errors.New("response request_id mismatch")
	}
	return resp, nil
}

// NegotiatedProfile returns the negotiated profile.
func (c *Client) NegotiatedProfile() uint32 {
	return c.negotiatedProfile
}

// Close releases all resources.
func (c *Client) Close() error {
	if c.shm != nil {
		c.shm.close()
		c.shm = nil
	}
	closeHandle(c.pipe)
	return nil
}

// ---------------------------------------------------------------------------
// CPU measurement helper
// ---------------------------------------------------------------------------

// SelfCPUSeconds returns the total CPU seconds (user + kernel) used by this process.
func SelfCPUSeconds() float64 {
	h, _, _ := procGetCurrentProcess.Call()
	var creation, exit, kernel, user [8]byte
	r, _, _ := procGetProcessTimes.Call(
		h,
		uintptr(unsafe.Pointer(&creation[0])),
		uintptr(unsafe.Pointer(&exit[0])),
		uintptr(unsafe.Pointer(&kernel[0])),
		uintptr(unsafe.Pointer(&user[0])),
	)
	if r == 0 {
		return 0
	}
	k := binary.LittleEndian.Uint64(kernel[:])
	u := binary.LittleEndian.Uint64(user[:])
	return float64(k+u) / 1e7
}
