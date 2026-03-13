//go:build linux

package posix

import (
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/protocol"
)

const (
	platformImplementedProfiles = ProfileUDSSeqpacket | ProfileSHMHybrid

	shmRegionMagic     uint32 = 0x4e53484d
	shmRegionVersion   uint16 = 3
	shmRegionAlignment        = 64
	shmHeaderLen             = 64
	shmDefaultSpinTries      = uint32(128)

	offHdrMagic         = 0
	offHdrVersion       = 4
	offHdrHeaderLen     = 6
	offHdrOwnerPID      = 8
	offHdrOwnerGen      = 12
	offHdrReqOffset     = 16
	offHdrReqCapacity   = 20
	offHdrRespOffset    = 24
	offHdrRespCapacity  = 28
	offReqSeq           = 32
	offRespSeq          = 40
	offReqLen           = 48
	offRespLen          = 52
	offReqSignal        = 56
	offRespSignal       = 60

	futexWait = 0
	futexWake = 1
)

type shmServer struct {
	file                  *os.File
	mapping               []byte
	base                  uintptr
	path                  string
	lastRequestSeq        uint64
	lastResponseSeq       uint64
	spinTries             uint32
	maxRequestMessageLen  int
	maxResponseMessageLen int
}

type shmClient struct {
	file                  *os.File
	mapping               []byte
	base                  uintptr
	nextRequestSeq        uint64
	spinTries             uint32
	maxRequestMessageLen  int
	maxResponseMessageLen int
}

func shmPathFromSocketPath(sockPath string) string {
	if strings.HasSuffix(sockPath, ".sock") {
		return strings.TrimSuffix(sockPath, ".sock") + ".ipcshm"
	}
	return filepath.Clean(sockPath) + ".ipcshm"
}

func shmAlignUp(value, alignment int) int {
	remainder := value % alignment
	if remainder == 0 {
		return value
	}
	return value + (alignment - remainder)
}

func computeSHMRegionLayout(requestCapacity, responseCapacity int) (int, int, int, error) {
	if requestCapacity <= 0 || responseCapacity <= 0 {
		return 0, 0, 0, fmt.Errorf("request and response capacities must be non-zero")
	}
	requestOffset := shmAlignUp(shmHeaderLen, shmRegionAlignment)
	responseOffset := shmAlignUp(requestOffset+requestCapacity, shmRegionAlignment)
	return requestOffset, responseOffset, responseOffset + responseCapacity, nil
}

func shmLoadU32(base uintptr, off int) uint32 {
	return atomic.LoadUint32((*uint32)(unsafe.Pointer(base + uintptr(off))))
}

func shmStoreU32(base uintptr, off int, val uint32) {
	atomic.StoreUint32((*uint32)(unsafe.Pointer(base+uintptr(off))), val)
}

func shmAddU32(base uintptr, off int, delta uint32) uint32 {
	return atomic.AddUint32((*uint32)(unsafe.Pointer(base+uintptr(off))), delta)
}

func shmLoadU64(base uintptr, off int) uint64 {
	return atomic.LoadUint64((*uint64)(unsafe.Pointer(base + uintptr(off))))
}

func shmStoreU64(base uintptr, off int, val uint64) {
	atomic.StoreUint64((*uint64)(unsafe.Pointer(base+uintptr(off))), val)
}

func shmLoadI32(base uintptr, off int) int32 {
	return atomic.LoadInt32((*int32)(unsafe.Pointer(base + uintptr(off))))
}

func shmStoreI32(base uintptr, off int, val int32) {
	atomic.StoreInt32((*int32)(unsafe.Pointer(base+uintptr(off))), val)
}

func shmRequestOffset(base uintptr) int {
	return int(binary.LittleEndian.Uint32(unsafe.Slice((*byte)(unsafe.Pointer(base+uintptr(offHdrReqOffset))), 4)))
}

func shmRequestCapacity(base uintptr) int {
	return int(binary.LittleEndian.Uint32(unsafe.Slice((*byte)(unsafe.Pointer(base+uintptr(offHdrReqCapacity))), 4)))
}

func shmResponseOffset(base uintptr) int {
	return int(binary.LittleEndian.Uint32(unsafe.Slice((*byte)(unsafe.Pointer(base+uintptr(offHdrRespOffset))), 4)))
}

func shmResponseCapacity(base uintptr) int {
	return int(binary.LittleEndian.Uint32(unsafe.Slice((*byte)(unsafe.Pointer(base+uintptr(offHdrRespCapacity))), 4)))
}

func shmRequestArea(base uintptr) uintptr {
	return base + uintptr(shmRequestOffset(base))
}

func shmResponseArea(base uintptr) uintptr {
	return base + uintptr(shmResponseOffset(base))
}

func shmValidateRegion(base uintptr, mappingLen int) error {
	if shmLoadU32(base, offHdrMagic) != shmRegionMagic ||
		binary.LittleEndian.Uint16(unsafe.Slice((*byte)(unsafe.Pointer(base+uintptr(offHdrVersion))), 2)) != shmRegionVersion ||
		binary.LittleEndian.Uint16(unsafe.Slice((*byte)(unsafe.Pointer(base+uintptr(offHdrHeaderLen))), 2)) != shmHeaderLen {
		return syscall.EPROTO
	}
	reqOffset := shmRequestOffset(base)
	reqCapacity := shmRequestCapacity(base)
	respOffset := shmResponseOffset(base)
	respCapacity := shmResponseCapacity(base)
	if reqCapacity <= 0 || respCapacity <= 0 {
		return syscall.EPROTO
	}
	if reqOffset < shmHeaderLen || respOffset < reqOffset+reqCapacity || respOffset+respCapacity > mappingLen {
		return syscall.EPROTO
	}
	return nil
}

func setEndpointLock(fd int, lockType int16) error {
	lock := syscall.Flock_t{
		Type:   lockType,
		Whence: int16(os.SEEK_SET),
		Start:  0,
		Len:    0,
	}
	return syscall.FcntlFlock(uintptr(fd), syscall.F_SETLK, &lock)
}

func lockEndpointFD(fd int) error {
	return setEndpointLock(fd, syscall.F_WRLCK)
}

func unlockEndpointFD(fd int) {
	_ = setEndpointLock(fd, syscall.F_UNLCK)
}

func endpointOwnedByLiveServer(fd int, ownerPID int32) (bool, error) {
	if ownerPID == int32(os.Getpid()) {
		return true, nil
	}
	if err := lockEndpointFD(fd); err == nil {
		unlockEndpointFD(fd)
		return false, nil
	} else if errors.Is(err, syscall.EACCES) || errors.Is(err, syscall.EAGAIN) {
		return true, nil
	} else {
		return false, err
	}
}

func futexWaitWord(addr *uint32, expected uint32, timeout *syscall.Timespec) error {
	var timeoutPtr uintptr
	if timeout != nil {
		timeoutPtr = uintptr(unsafe.Pointer(timeout))
	}
	_, _, errno := syscall.Syscall6(
		syscall.SYS_FUTEX,
		uintptr(unsafe.Pointer(addr)),
		uintptr(futexWait),
		uintptr(expected),
		timeoutPtr,
		0,
		0,
	)
	if errno != 0 {
		return errno
	}
	return nil
}

func futexWakeWord(addr *uint32) {
	_, _, _ = syscall.Syscall6(
		syscall.SYS_FUTEX,
		uintptr(unsafe.Pointer(addr)),
		uintptr(futexWake),
		1,
		0,
		0,
		0,
	)
}

func durationToTimespec(d time.Duration) syscall.Timespec {
	if d < 0 {
		d = 0
	}
	return syscall.NsecToTimespec(d.Nanoseconds())
}

func shmWaitForSequence(base uintptr, seqOff int, signalOff int, target uint64, spinTries uint32, timeout time.Duration) error {
	var deadline time.Time
	if timeout > 0 {
		deadline = time.Now().Add(timeout)
	}
	seqPtr := (*uint64)(unsafe.Pointer(base + uintptr(seqOff)))
	signalPtr := (*uint32)(unsafe.Pointer(base + uintptr(signalOff)))

	for {
		if atomic.LoadUint64(seqPtr) >= target {
			return nil
		}
		for i := uint32(0); i < spinTries; i++ {
			if atomic.LoadUint64(seqPtr) >= target {
				return nil
			}
			spinPause()
		}

		expected := atomic.LoadUint32(signalPtr)
		if atomic.LoadUint64(seqPtr) >= target {
			return nil
		}

		var waitTimeout *syscall.Timespec
		if !deadline.IsZero() {
			remain := time.Until(deadline)
			if remain <= 0 {
				return syscall.ETIMEDOUT
			}
			ts := durationToTimespec(remain)
			waitTimeout = &ts
		}

		if err := futexWaitWord(signalPtr, expected, waitTimeout); err != nil {
			if errors.Is(err, syscall.EAGAIN) || errors.Is(err, syscall.EINTR) || errors.Is(err, syscall.ETIMEDOUT) {
				if errors.Is(err, syscall.ETIMEDOUT) && !deadline.IsZero() && time.Until(deadline) <= 0 {
					return syscall.ETIMEDOUT
				}
				continue
			}
			return err
		}
	}
}

func shmWake(base uintptr, signalOff int) {
	ptr := (*uint32)(unsafe.Pointer(base + uintptr(signalOff)))
	atomic.AddUint32(ptr, 1)
	futexWakeWord(ptr)
}

func tryTakeoverStaleSHMEndpoint(path string) (bool, error) {
	file, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return false, err
	}
	defer file.Close()

	info, err := file.Stat()
	if err != nil {
		return false, err
	}
	if info.Size() < shmHeaderLen {
		if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
			return false, err
		}
		return true, nil
	}

	mapping, err := syscall.Mmap(int(file.Fd()), 0, int(info.Size()), syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		return false, err
	}
	base := uintptr(unsafe.Pointer(&mapping[0]))
	live := false
	if shmValidateRegion(base, len(mapping)) == nil {
		live, err = endpointOwnedByLiveServer(int(file.Fd()), shmLoadI32(base, offHdrOwnerPID))
	}
	_ = syscall.Munmap(mapping)
	if err != nil {
		return false, err
	}
	if live {
		return false, os.ErrExist
	}
	if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
		return false, err
	}
	return true, nil
}

func newSHMServer(sockPath string, maxRequestMessageLen int, maxResponseMessageLen int) (*shmServer, error) {
	path := shmPathFromSocketPath(sockPath)
	requestOffset, responseOffset, mappingLen, err := computeSHMRegionLayout(maxRequestMessageLen, maxResponseMessageLen)
	if err != nil {
		return nil, err
	}

	var file *os.File
	for attempt := 0; attempt < 2; attempt++ {
		file, err = os.OpenFile(path, os.O_RDWR|os.O_CREATE|os.O_EXCL, 0o600)
		if err == nil {
			if err = lockEndpointFD(int(file.Fd())); err != nil {
				_ = file.Close()
				return nil, err
			}
			break
		}
		if !errors.Is(err, os.ErrExist) {
			return nil, err
		}
		takeover, takeoverErr := tryTakeoverStaleSHMEndpoint(path)
		if takeoverErr != nil {
			return nil, takeoverErr
		}
		if !takeover {
			return nil, os.ErrExist
		}
	}
	if file == nil {
		return nil, os.ErrExist
	}

	if err := file.Truncate(int64(mappingLen)); err != nil {
		_ = file.Close()
		_ = os.Remove(path)
		return nil, err
	}

	mapping, err := syscall.Mmap(int(file.Fd()), 0, mappingLen, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		_ = file.Close()
		_ = os.Remove(path)
		return nil, err
	}
	for i := range mapping {
		mapping[i] = 0
	}
	base := uintptr(unsafe.Pointer(&mapping[0]))
	shmStoreU32(base, offHdrMagic, shmRegionMagic)
	binary.LittleEndian.PutUint16(mapping[offHdrVersion:offHdrVersion+2], shmRegionVersion)
	binary.LittleEndian.PutUint16(mapping[offHdrHeaderLen:offHdrHeaderLen+2], shmHeaderLen)
	shmStoreI32(base, offHdrOwnerPID, int32(os.Getpid()))
	shmStoreU32(base, offHdrOwnerGen, 1)
	shmStoreU32(base, offHdrReqOffset, uint32(requestOffset))
	shmStoreU32(base, offHdrReqCapacity, uint32(maxRequestMessageLen))
	shmStoreU32(base, offHdrRespOffset, uint32(responseOffset))
	shmStoreU32(base, offHdrRespCapacity, uint32(maxResponseMessageLen))

	return &shmServer{
		file:                  file,
		mapping:               mapping,
		base:                  base,
		path:                  path,
		spinTries:             shmDefaultSpinTries,
		maxRequestMessageLen:  maxRequestMessageLen,
		maxResponseMessageLen: maxResponseMessageLen,
	}, nil
}

func openSHMClient(path string, maxRequestMessageLen int, maxResponseMessageLen int) (*shmClient, error) {
	file, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return nil, err
	}
	info, err := file.Stat()
	if err != nil {
		_ = file.Close()
		return nil, err
	}
	if info.Size() < shmHeaderLen {
		_ = file.Close()
		return nil, syscall.EPROTO
	}
	mapping, err := syscall.Mmap(int(file.Fd()), 0, int(info.Size()), syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		_ = file.Close()
		return nil, err
	}
	base := uintptr(unsafe.Pointer(&mapping[0]))
	if err := shmValidateRegion(base, len(mapping)); err != nil {
		_ = syscall.Munmap(mapping)
		_ = file.Close()
		return nil, err
	}
	if shmRequestCapacity(base) < maxRequestMessageLen || shmResponseCapacity(base) < maxResponseMessageLen {
		_ = syscall.Munmap(mapping)
		_ = file.Close()
		return nil, syscall.EMSGSIZE
	}
	live, err := endpointOwnedByLiveServer(int(file.Fd()), shmLoadI32(base, offHdrOwnerPID))
	if err != nil {
		_ = syscall.Munmap(mapping)
		_ = file.Close()
		return nil, err
	}
	if !live {
		_ = syscall.Munmap(mapping)
		_ = file.Close()
		return nil, syscall.ECONNREFUSED
	}
	return &shmClient{
		file:                  file,
		mapping:               mapping,
		base:                  base,
		spinTries:             shmDefaultSpinTries,
		maxRequestMessageLen:  maxRequestMessageLen,
		maxResponseMessageLen: maxResponseMessageLen,
	}, nil
}

func newSHMClient(sockPath string, maxRequestMessageLen int, maxResponseMessageLen int, timeout time.Duration) (*shmClient, error) {
	path := shmPathFromSocketPath(sockPath)
	var deadline time.Time
	if timeout > 0 {
		deadline = time.Now().Add(timeout)
	}
	for {
		client, err := openSHMClient(path, maxRequestMessageLen, maxResponseMessageLen)
		if err == nil {
			return client, nil
		}
		if !errors.Is(err, os.ErrNotExist) && !errors.Is(err, syscall.ECONNREFUSED) && !errors.Is(err, syscall.EPROTO) {
			return nil, err
		}
		if !deadline.IsZero() && time.Until(deadline) <= 0 {
			return nil, syscall.ETIMEDOUT
		}
		time.Sleep(500 * time.Microsecond)
	}
}

func (s *shmServer) receiveBytes(message []byte, messageCapacity int, timeout time.Duration) (int, error) {
	if messageCapacity <= 0 || len(message) < messageCapacity || messageCapacity > s.maxRequestMessageLen {
		return 0, fmt.Errorf("message buffer is smaller than negotiated request size")
	}
	target := s.lastRequestSeq + 1
	if err := shmWaitForSequence(s.base, offReqSeq, offReqSignal, target, s.spinTries, timeout); err != nil {
		return 0, err
	}
	publishedSeq := shmLoadU64(s.base, offReqSeq)
	publishedLen := int(shmLoadU32(s.base, offReqLen))
	if publishedLen < 0 || publishedLen > s.maxRequestMessageLen || publishedLen > shmRequestCapacity(s.base) || publishedLen > messageCapacity {
		return 0, syscall.EMSGSIZE
	}
	copy(message[:publishedLen], unsafe.Slice((*byte)(unsafe.Pointer(shmRequestArea(s.base))), publishedLen))
	s.lastRequestSeq = publishedSeq
	return publishedLen, nil
}

func (s *shmServer) receiveMessage(message []byte, timeout time.Duration) (int, error) {
	messageLen, err := s.receiveBytes(message, s.maxRequestMessageLen, timeout)
	if err != nil {
		return 0, err
	}
	if err := validateReceivedMessage(message, messageLen, s.maxRequestMessageLen); err != nil {
		return 0, err
	}
	return messageLen, nil
}

func (s *shmServer) sendBytes(message []byte, messageCapacity int) error {
	if s.lastRequestSeq == 0 || s.lastRequestSeq == s.lastResponseSeq {
		return syscall.EPROTO
	}
	if len(message) == 0 || len(message) > messageCapacity || messageCapacity > s.maxResponseMessageLen {
		return syscall.EMSGSIZE
	}
	copy(unsafe.Slice((*byte)(unsafe.Pointer(shmResponseArea(s.base))), len(message)), message)
	shmStoreU32(s.base, offRespLen, uint32(len(message)))
	shmStoreU64(s.base, offRespSeq, s.lastRequestSeq)
	shmWake(s.base, offRespSignal)
	s.lastResponseSeq = s.lastRequestSeq
	return nil
}

func (s *shmServer) sendMessage(message []byte) error {
	if err := validateMessageForSend(message, s.maxResponseMessageLen); err != nil {
		return err
	}
	return s.sendBytes(message, s.maxResponseMessageLen)
}

func (s *shmServer) receiveFrame(timeout time.Duration) (protocol.Frame, error) {
	if s.maxRequestMessageLen < protocol.FrameSize {
		return protocol.Frame{}, syscall.EMSGSIZE
	}
	var message protocol.Frame
	messageLen, err := s.receiveBytes(message[:], protocol.FrameSize, timeout)
	if err != nil {
		return protocol.Frame{}, err
	}
	if messageLen != protocol.FrameSize {
		return protocol.Frame{}, syscall.EPROTO
	}
	return message, nil
}

func (s *shmServer) sendFrame(frame protocol.Frame) error {
	if s.maxResponseMessageLen < protocol.FrameSize {
		return syscall.EMSGSIZE
	}
	return s.sendBytes(frame[:], protocol.FrameSize)
}

func (s *shmServer) close() error {
	var errs []error
	if s.base != 0 {
		shmStoreI32(s.base, offHdrOwnerPID, 0)
	}
	if s.mapping != nil {
		if err := syscall.Munmap(s.mapping); err != nil {
			errs = append(errs, err)
		}
		s.mapping = nil
		s.base = 0
	}
	if s.file != nil {
		if err := s.file.Close(); err != nil {
			errs = append(errs, err)
		}
		s.file = nil
	}
	if s.path != "" {
		if err := os.Remove(s.path); err != nil && !errors.Is(err, os.ErrNotExist) {
			errs = append(errs, err)
		}
	}
	return errors.Join(errs...)
}

func (c *shmClient) callBytes(request []byte, response []byte, requestCapacity int, responseCapacity int, timeout time.Duration) (int, error) {
	if len(request) == 0 || len(request) > requestCapacity || requestCapacity > c.maxRequestMessageLen {
		return 0, syscall.EMSGSIZE
	}
	if len(response) < responseCapacity || responseCapacity > c.maxResponseMessageLen {
		return 0, syscall.EMSGSIZE
	}
	seq := c.nextRequestSeq + 1
	c.nextRequestSeq = seq
	copy(unsafe.Slice((*byte)(unsafe.Pointer(shmRequestArea(c.base))), len(request)), request)
	shmStoreU32(c.base, offReqLen, uint32(len(request)))
	shmStoreU64(c.base, offReqSeq, seq)
	shmWake(c.base, offReqSignal)
	if err := shmWaitForSequence(c.base, offRespSeq, offRespSignal, seq, c.spinTries, timeout); err != nil {
		return 0, err
	}
	responseLen := int(shmLoadU32(c.base, offRespLen))
	if responseLen < 0 || responseLen > c.maxResponseMessageLen || responseLen > shmResponseCapacity(c.base) || responseLen > responseCapacity {
		return 0, syscall.EMSGSIZE
	}
	copy(response[:responseLen], unsafe.Slice((*byte)(unsafe.Pointer(shmResponseArea(c.base))), responseLen))
	return responseLen, nil
}

func (c *shmClient) callMessage(request []byte, response []byte, timeout time.Duration) (int, error) {
	if err := validateMessageForSend(request, c.maxRequestMessageLen); err != nil {
		return 0, err
	}
	responseLen, err := c.callBytes(request, response, c.maxRequestMessageLen, c.maxResponseMessageLen, timeout)
	if err != nil {
		return 0, err
	}
	if err := validateReceivedMessage(response, responseLen, c.maxResponseMessageLen); err != nil {
		return 0, err
	}
	return responseLen, nil
}

func (c *shmClient) callFrame(request protocol.Frame, timeout time.Duration) (protocol.Frame, error) {
	if c.maxRequestMessageLen < protocol.FrameSize || c.maxResponseMessageLen < protocol.FrameSize {
		return protocol.Frame{}, syscall.EMSGSIZE
	}
	var response protocol.Frame
	responseLen, err := c.callBytes(request[:], response[:], protocol.FrameSize, protocol.FrameSize, timeout)
	if err != nil {
		return protocol.Frame{}, err
	}
	if responseLen != protocol.FrameSize {
		return protocol.Frame{}, syscall.EPROTO
	}
	return response, nil
}

func (c *shmClient) close() error {
	var errs []error
	if c.mapping != nil {
		if err := syscall.Munmap(c.mapping); err != nil {
			errs = append(errs, err)
		}
		c.mapping = nil
		c.base = 0
	}
	if c.file != nil {
		if err := c.file.Close(); err != nil {
			errs = append(errs, err)
		}
		c.file = nil
	}
	return errors.Join(errs...)
}
