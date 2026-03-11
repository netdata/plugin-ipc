//go:build windows

// Package windows provides Named Pipe and SHM HYBRID transports for Windows.
// The negotiation protocol and SHM region layout are wire-compatible
// with the C implementation in netipc_named_pipe.c / netipc_shm_hybrid_win.c.
package windows

import (
	"encoding/binary"
	"errors"
	"fmt"
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

	pageReadWrite  = 0x04
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

	errorAlreadyExists  = 183
	errorFileNotFound   = 2
	errorPipeBusy       = 231
	errorPipeConnected  = 535
	errorPipeListening  = 536
	errorNoData         = 232
	errorBrokenPipe     = 109
	errorAccessDenied   = 5
	errorNotSupported   = 50

	nmpwaitWaitForever = 0xFFFFFFFF

	synchronize      = 0x00100000
	eventModifyState = 0x0002
)

// ---------------------------------------------------------------------------
// Profile constants (wire-compatible with C)
// ---------------------------------------------------------------------------

const (
	ProfileNamedPipe  uint32 = 1 << 0
	ProfileSHMHybrid  uint32 = 1 << 1
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
	negMagic   uint32 = 0x4e48534b
	negVersion uint16 = 1
	negHello   uint16 = 1
	negAck     uint16 = 2
	negStatusOK uint32 = 0
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

// ---------------------------------------------------------------------------
// SHM region layout constants (wire-compatible with C)
// ---------------------------------------------------------------------------

const (
	shmRegionMagic   uint32 = 0x4e535748
	shmRegionVersion uint32 = 2
	cacheline               = 64
	regionSize              = cacheline + (cacheline + protocol.FrameSize) + (cacheline + protocol.FrameSize) // 320 bytes

	offHdrMagic     = 0
	offHdrVersion   = 4
	offHdrProfile   = 8
	offHdrSpinTries = 12

	offReq             = cacheline
	offReqSeq          = offReq
	offReqClientClosed = offReq + 8
	offReqServerWait   = offReq + 12
	offReqFrame        = offReq + cacheline

	offResp             = cacheline + cacheline + protocol.FrameSize
	offRespSeq          = offResp
	offRespServerClosed = offResp + 8
	offRespClientWait   = offResp + 12
	offRespFrame        = offResp + cacheline
)

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

// Config holds the configuration for a Named Pipe transport.
type Config struct {
	RunDir            string
	ServiceName       string
	SupportedProfiles uint32
	PreferredProfiles uint32
	AuthToken         uint64
	SHMSpinTries      uint32
}

// NewConfig creates a new Config with default settings.
func NewConfig(runDir, serviceName string) Config {
	return Config{
		RunDir:            runDir,
		ServiceName:       serviceName,
		SupportedProfiles: DefaultSupportedProfiles,
		PreferredProfiles: DefaultPreferredProfiles,
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

type negMessage struct {
	typ          uint16
	supported    uint32
	preferred    uint32
	intersection uint32
	selected     uint32
	authToken    uint64
	status       uint32
}

func encodeNeg(msg negMessage) protocol.Frame {
	var frame protocol.Frame
	binary.LittleEndian.PutUint32(frame[negOffMagic:], negMagic)
	binary.LittleEndian.PutUint16(frame[negOffVersion:], negVersion)
	binary.LittleEndian.PutUint16(frame[negOffType:], msg.typ)
	binary.LittleEndian.PutUint32(frame[negOffSupported:], msg.supported)
	binary.LittleEndian.PutUint32(frame[negOffPreferred:], msg.preferred)
	binary.LittleEndian.PutUint32(frame[negOffIntersection:], msg.intersection)
	binary.LittleEndian.PutUint32(frame[negOffSelected:], msg.selected)
	binary.LittleEndian.PutUint64(frame[negOffAuthToken:], msg.authToken)
	binary.LittleEndian.PutUint32(frame[negOffStatus:], msg.status)
	return frame
}

func decodeNeg(frame protocol.Frame, expectedType uint16) (negMessage, error) {
	magic := binary.LittleEndian.Uint32(frame[negOffMagic:])
	version := binary.LittleEndian.Uint16(frame[negOffVersion:])
	typ := binary.LittleEndian.Uint16(frame[negOffType:])
	if magic != negMagic || version != negVersion || typ != expectedType {
		return negMessage{}, errors.New("invalid negotiation frame")
	}
	return negMessage{
		typ:          typ,
		supported:    binary.LittleEndian.Uint32(frame[negOffSupported:]),
		preferred:    binary.LittleEndian.Uint32(frame[negOffPreferred:]),
		intersection: binary.LittleEndian.Uint32(frame[negOffIntersection:]),
		selected:     binary.LittleEndian.Uint32(frame[negOffSelected:]),
		authToken:    binary.LittleEndian.Uint64(frame[negOffAuthToken:]),
		status:       binary.LittleEndian.Uint32(frame[negOffStatus:]),
	}, nil
}

// ---------------------------------------------------------------------------
// Pipe I/O helpers
// ---------------------------------------------------------------------------

func pipeReadFrame(pipe syscall.Handle, timeoutMS uint32) (protocol.Frame, error) {
	if timeoutMS != 0 {
		deadline := nowMS() + uint64(timeoutMS)
		for {
			var avail uint32
			r, _, lastErr := procPeekNamedPipe.Call(uintptr(pipe), 0, 0, 0, uintptr(unsafe.Pointer(&avail)), 0)
			if r == 0 {
				return protocol.Frame{}, fmt.Errorf("PeekNamedPipe failed: %w", lastErr)
			}
			if avail != 0 {
				break
			}
			if nowMS() >= deadline {
				return protocol.Frame{}, errors.New("pipe read timeout")
			}
			sleepMS(1)
		}
	}

	var frame protocol.Frame
	var bytesRead uint32
	r, _, lastErr := procReadFile.Call(
		uintptr(pipe),
		uintptr(unsafe.Pointer(&frame[0])),
		uintptr(protocol.FrameSize),
		uintptr(unsafe.Pointer(&bytesRead)),
		0,
	)
	if r == 0 {
		return frame, fmt.Errorf("ReadFile failed: %w", lastErr)
	}
	if bytesRead != protocol.FrameSize {
		return frame, fmt.Errorf("short pipe read: %d", bytesRead)
	}
	return frame, nil
}

func pipeWriteFrame(pipe syscall.Handle, frame protocol.Frame) error {
	var written uint32
	r, _, lastErr := procWriteFile.Call(
		uintptr(pipe),
		uintptr(unsafe.Pointer(&frame[0])),
		uintptr(protocol.FrameSize),
		uintptr(unsafe.Pointer(&written)),
		0,
	)
	if r == 0 {
		return fmt.Errorf("WriteFile failed: %w", lastErr)
	}
	if written != protocol.FrameSize {
		return fmt.Errorf("short pipe write: %d", written)
	}
	return nil
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

func serverHandshake(pipe syscall.Handle, supported, preferred uint32, authToken uint64, timeoutMS uint32) (uint32, error) {
	helloFrame, err := pipeReadFrame(pipe, timeoutMS)
	if err != nil {
		return 0, err
	}
	hello, err := decodeNeg(helloFrame, negHello)
	if err != nil {
		return 0, err
	}

	ack := negMessage{
		typ:          negAck,
		supported:    supported,
		preferred:    preferred,
		intersection: hello.supported & supported,
		status:       negStatusOK,
	}

	if authToken != 0 && hello.authToken != authToken {
		ack.status = errorAccessDenied
	} else {
		candidates := ack.intersection & preferred
		if candidates == 0 {
			candidates = ack.intersection
		}
		ack.selected = selectProfile(candidates)
		if ack.selected == 0 {
			ack.status = errorNotSupported
		}
	}

	if err := pipeWriteFrame(pipe, encodeNeg(ack)); err != nil {
		return 0, err
	}
	if ack.status != negStatusOK {
		return 0, fmt.Errorf("negotiation failed: status %d", ack.status)
	}
	return ack.selected, nil
}

func clientHandshake(pipe syscall.Handle, supported, preferred uint32, authToken uint64, timeoutMS uint32) (uint32, error) {
	hello := negMessage{
		typ:       negHello,
		supported: supported,
		preferred: preferred,
		authToken: authToken,
		status:    negStatusOK,
	}
	if err := pipeWriteFrame(pipe, encodeNeg(hello)); err != nil {
		return 0, err
	}
	ackFrame, err := pipeReadFrame(pipe, timeoutMS)
	if err != nil {
		return 0, err
	}
	ack, err := decodeNeg(ackFrame, negAck)
	if err != nil {
		return 0, err
	}
	if ack.status != negStatusOK {
		return 0, fmt.Errorf("server rejected: status %d", ack.status)
	}
	if ack.selected == 0 || (ack.selected&supported) == 0 || (ack.intersection&supported) == 0 {
		return 0, errors.New("invalid negotiated profile")
	}
	return ack.selected, nil
}

// ---------------------------------------------------------------------------
// SHM region access helpers
// ---------------------------------------------------------------------------

func shmLoadI64(base uintptr, off int) int64 {
	return atomic.LoadInt64((*int64)(unsafe.Pointer(base + uintptr(off))))
}

func shmStoreI64(base uintptr, off int, val int64) {
	atomic.StoreInt64((*int64)(unsafe.Pointer(base + uintptr(off))), val)
}

func shmLoadI32(base uintptr, off int) int32 {
	return atomic.LoadInt32((*int32)(unsafe.Pointer(base + uintptr(off))))
}

func shmStoreI32(base uintptr, off int, val int32) {
	atomic.StoreInt32((*int32)(unsafe.Pointer(base + uintptr(off))), val)
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
	mapping       syscall.Handle
	requestEvent  syscall.Handle
	responseEvent syscall.Handle
	region        uintptr
	lastReqSeq    int64
	activeReqSeq  int64
	spinTries     uint32
}

func newSHMServer(config *Config, profile uint32) (*shmServer, error) {
	mappingName := buildKernelObjectName(config, profile, "shm")
	reqEventName := buildKernelObjectName(config, profile, "req")
	respEventName := buildKernelObjectName(config, profile, "resp")

	mapping, _, lastErr := procCreateFileMappingW.Call(
		uintptr(invalidHandleValue), 0, uintptr(pageReadWrite),
		0, uintptr(regionSize), uintptr(unsafe.Pointer(toUTF16(mappingName))),
	)
	if mapping == 0 {
		return nil, fmt.Errorf("CreateFileMappingW: %w", lastErr)
	}
	if lastErr == syscall.Errno(errorAlreadyExists) {
		closeHandle(syscall.Handle(mapping))
		return nil, errors.New("SHM already exists")
	}

	region, _, lastErr := procMapViewOfFile.Call(mapping, uintptr(fileMapAllAccess), 0, 0, uintptr(regionSize))
	if region == 0 {
		closeHandle(syscall.Handle(mapping))
		return nil, fmt.Errorf("MapViewOfFile: %w", lastErr)
	}

	// Zero region
	for i := 0; i < regionSize; i++ {
		*(*byte)(unsafe.Pointer(region + uintptr(i))) = 0
	}

	// Init header
	spin := effectiveSpinTries(config)
	binary.LittleEndian.PutUint32((*[4]byte)(unsafe.Pointer(region + uintptr(offHdrMagic)))[:], shmRegionMagic)
	binary.LittleEndian.PutUint32((*[4]byte)(unsafe.Pointer(region + uintptr(offHdrVersion)))[:], shmRegionVersion)
	binary.LittleEndian.PutUint32((*[4]byte)(unsafe.Pointer(region + uintptr(offHdrProfile)))[:], profile)
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
		mapping:       syscall.Handle(mapping),
		requestEvent:  syscall.Handle(reqEvent),
		responseEvent: syscall.Handle(respEvent),
		region:        region,
		spinTries:     spin,
	}, nil
}

func (s *shmServer) receiveFrame(timeoutMS uint32) (protocol.Frame, error) {
	var deadline uint64
	if timeoutMS != 0 {
		deadline = nowMS() + uint64(timeoutMS)
	}
	spins := s.spinTries

	for {
		current := shmLoadI64(s.region, offReqSeq)
		if current != s.lastReqSeq {
			frame := shmCopyFrame(s.region, offReqFrame)
			s.activeReqSeq = current
			s.lastReqSeq = current
			return frame, nil
		}

		if shmLoadI32(s.region, offReqClientClosed) != 0 {
			return protocol.Frame{}, errors.New("client closed")
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
			frame := shmCopyFrame(s.region, offReqFrame)
			s.activeReqSeq = current
			s.lastReqSeq = current
			return frame, nil
		}

		waitMS := uint32(infinite)
		if deadline != 0 {
			now := nowMS()
			if now >= deadline {
				shmStoreI32(s.region, offReqServerWait, 0)
				return protocol.Frame{}, errors.New("SHM receive timeout")
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
			return protocol.Frame{}, errors.New("SHM receive timeout")
		}
		return protocol.Frame{}, fmt.Errorf("WaitForSingleObject(req): %w", lastErr)
	}
}

func (s *shmServer) sendFrame(frame protocol.Frame) error {
	if s.activeReqSeq == 0 {
		return errors.New("no active request")
	}
	shmWriteFrame(s.region, offRespFrame, frame)
	shmStoreI64(s.region, offRespSeq, s.activeReqSeq)

	// Conditional SetEvent
	if shmLoadI32(s.region, offRespClientWait) != 0 {
		procSetEvent.Call(uintptr(s.responseEvent))
	}
	s.activeReqSeq = 0
	return nil
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
	mapping       syscall.Handle
	requestEvent  syscall.Handle
	responseEvent syscall.Handle
	region        uintptr
	nextReqSeq    int64
	spinTries     uint32
}

func newSHMClient(config *Config, profile uint32, timeoutMS uint32) (*shmClient, error) {
	mappingName := buildKernelObjectName(config, profile, "shm")
	reqEventName := buildKernelObjectName(config, profile, "req")
	respEventName := buildKernelObjectName(config, profile, "resp")

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

	region, _, lastErr := procMapViewOfFile.Call(mapping, uintptr(fileMapAllAccess), 0, 0, uintptr(regionSize))
	if region == 0 {
		closeHandle(syscall.Handle(respEvent))
		closeHandle(syscall.Handle(reqEvent))
		closeHandle(syscall.Handle(mapping))
		return nil, fmt.Errorf("MapViewOfFile: %w", lastErr)
	}

	// Wait for region ready
	for {
		magic := binary.LittleEndian.Uint32((*[4]byte)(unsafe.Pointer(region + uintptr(offHdrMagic)))[:])
		ver := binary.LittleEndian.Uint32((*[4]byte)(unsafe.Pointer(region + uintptr(offHdrVersion)))[:])
		prof := binary.LittleEndian.Uint32((*[4]byte)(unsafe.Pointer(region + uintptr(offHdrProfile)))[:])
		if magic == shmRegionMagic && ver == shmRegionVersion && prof == profile {
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

	regionSpin := binary.LittleEndian.Uint32((*[4]byte)(unsafe.Pointer(region + uintptr(offHdrSpinTries)))[:])
	spin := regionSpin
	if spin == 0 {
		spin = effectiveSpinTries(config)
	}

	nextReqSeq := shmLoadI64(region, offReqSeq)

	return &shmClient{
		mapping:       syscall.Handle(mapping),
		requestEvent:  syscall.Handle(reqEvent),
		responseEvent: syscall.Handle(respEvent),
		region:        region,
		nextReqSeq:    nextReqSeq,
		spinTries:     spin,
	}, nil
}

func (c *shmClient) callFrame(request protocol.Frame, timeoutMS uint32) (protocol.Frame, error) {
	reqSeq := c.nextReqSeq + 1
	c.nextReqSeq = reqSeq

	shmWriteFrame(c.region, offReqFrame, request)
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
			return shmCopyFrame(c.region, offRespFrame), nil
		}

		if shmLoadI32(c.region, offRespServerClosed) != 0 {
			return protocol.Frame{}, errors.New("server closed")
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
			return shmCopyFrame(c.region, offRespFrame), nil
		}

		waitMS := uint32(infinite)
		if deadline != 0 {
			now := nowMS()
			if now >= deadline {
				shmStoreI32(c.region, offRespClientWait, 0)
				return protocol.Frame{}, errors.New("SHM response timeout")
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
			return protocol.Frame{}, errors.New("SHM response timeout")
		}
		return protocol.Frame{}, fmt.Errorf("WaitForSingleObject(resp): %w", lastErr)
	}
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
	pipe              syscall.Handle
	config            Config
	supportedProfiles uint32
	preferredProfiles uint32
	authToken         uint64
	negotiatedProfile uint32
	shm               *shmServer
	connected         bool
}

// Listen creates a Named Pipe server.
func Listen(config Config) (*Server, error) {
	if config.RunDir == "" || config.ServiceName == "" {
		return nil, errors.New("run_dir and service_name must be set")
	}
	pipeName := buildPipeName(&config)
	supported := effectiveSupported(&config)
	preferred := effectivePreferred(&config, supported)

	pipe, _, lastErr := procCreateNamedPipeW.Call(
		uintptr(unsafe.Pointer(toUTF16(pipeName))),
		uintptr(pipeAccessDuplex|fileFlagFirstPipeInstance),
		uintptr(pipeTypeMessage|pipeReadModeMessage|pipeWait),
		1,
		uintptr(protocol.FrameSize*4),
		uintptr(protocol.FrameSize*4),
		0, 0,
	)
	if syscall.Handle(pipe) == invalidHandleValue {
		return nil, fmt.Errorf("CreateNamedPipeW: %w", lastErr)
	}

	return &Server{
		pipe:              syscall.Handle(pipe),
		config:            config,
		supportedProfiles: supported,
		preferredProfiles: preferred,
		authToken:         config.AuthToken,
	}, nil
}

// Accept waits for a client to connect and performs negotiation.
func (s *Server) Accept(timeout time.Duration) error {
	timeoutMS := uint32(0)
	if timeout > 0 {
		timeoutMS = uint32(timeout.Milliseconds())
	}

	if err := setPipeMode(s.pipe, pipeNoWait); err != nil {
		return err
	}

	var deadline uint64
	if timeoutMS != 0 {
		deadline = nowMS() + uint64(timeoutMS)
	}

	for {
		r, _, lastErr := procConnectNamedPipe.Call(uintptr(s.pipe), 0)
		if r != 0 {
			break
		}
		errCode, _ := lastErr.(syscall.Errno)
		if errCode == syscall.Errno(errorPipeConnected) {
			break
		}
		if errCode != syscall.Errno(errorPipeListening) && errCode != syscall.Errno(errorNoData) {
			_ = setPipeMode(s.pipe, pipeWait)
			return fmt.Errorf("ConnectNamedPipe: %w", lastErr)
		}
		if deadline != 0 && nowMS() >= deadline {
			_ = setPipeMode(s.pipe, pipeWait)
			return errors.New("accept timeout")
		}
		sleepMS(1)
	}

	if err := setPipeMode(s.pipe, pipeWait); err != nil {
		return err
	}

	profile, err := serverHandshake(s.pipe, s.supportedProfiles, s.preferredProfiles, s.authToken, timeoutMS)
	if err != nil {
		procDisconnectNamedPipe.Call(uintptr(s.pipe))
		return err
	}

	if isSHMProfile(profile) {
		shm, err := newSHMServer(&s.config, profile)
		if err != nil {
			procDisconnectNamedPipe.Call(uintptr(s.pipe))
			return err
		}
		s.shm = shm
	}

	s.negotiatedProfile = profile
	s.connected = true
	return nil
}

// ReceiveFrame receives a single frame.
func (s *Server) ReceiveFrame(timeout time.Duration) (protocol.Frame, error) {
	timeoutMS := uint32(0)
	if timeout > 0 {
		timeoutMS = uint32(timeout.Milliseconds())
	}
	if s.shm != nil {
		return s.shm.receiveFrame(timeoutMS)
	}
	return pipeReadFrame(s.pipe, timeoutMS)
}

// SendFrame sends a single frame.
func (s *Server) SendFrame(frame protocol.Frame, timeout time.Duration) error {
	if s.shm != nil {
		return s.shm.sendFrame(frame)
	}
	return pipeWriteFrame(s.pipe, frame)
}

// ReceiveIncrement receives an increment request.
func (s *Server) ReceiveIncrement(timeout time.Duration) (uint64, protocol.IncrementRequest, error) {
	frame, err := s.ReceiveFrame(timeout)
	if err != nil {
		return 0, protocol.IncrementRequest{}, err
	}
	return protocol.DecodeIncrementRequest(frame)
}

// SendIncrement sends an increment response.
func (s *Server) SendIncrement(requestID uint64, response protocol.IncrementResponse, timeout time.Duration) error {
	return s.SendFrame(protocol.EncodeIncrementResponse(requestID, response), timeout)
}

// NegotiatedProfile returns the negotiated profile.
func (s *Server) NegotiatedProfile() uint32 {
	return s.negotiatedProfile
}

// Close releases all resources.
func (s *Server) Close() error {
	if s.shm != nil {
		s.shm.close()
		s.shm = nil
	}
	if s.connected {
		procFlushFileBuffers.Call(uintptr(s.pipe))
		procDisconnectNamedPipe.Call(uintptr(s.pipe))
	}
	closeHandle(s.pipe)
	return nil
}

// ---------------------------------------------------------------------------
// Public API: Client
// ---------------------------------------------------------------------------

// Client is a Named Pipe client that may upgrade to SHM HYBRID.
type Client struct {
	pipe              syscall.Handle
	config            Config
	supportedProfiles uint32
	preferredProfiles uint32
	authToken         uint64
	negotiatedProfile uint32
	shm               *shmClient
	nextRequestID     uint64
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

	profile, err := clientHandshake(pipe, supported, preferred, config.AuthToken, timeoutMS)
	if err != nil {
		closeHandle(pipe)
		return nil, err
	}

	var shm *shmClient
	if isSHMProfile(profile) {
		shm, err = newSHMClient(&config, profile, timeoutMS)
		if err != nil {
			closeHandle(pipe)
			return nil, err
		}
	}

	return &Client{
		pipe:              pipe,
		config:            config,
		supportedProfiles: supported,
		preferredProfiles: preferred,
		authToken:         config.AuthToken,
		negotiatedProfile: profile,
		shm:               shm,
		nextRequestID:     1,
	}, nil
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
