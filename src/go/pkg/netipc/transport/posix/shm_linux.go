//go:build linux

// SHM transport for Linux — shared memory data plane with spin+futex
// synchronization. Wire-compatible with the C and Rust implementations.
//
// Pure Go — no cgo. Works with CGO_ENABLED=0.

package posix

import (
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

const (
	shmRegionMagic     uint32 = 0x4e53484d // "NSHM"
	shmRegionVersion   uint16 = 3
	shmRegionAlignment uint32 = 64
	shmHeaderLen       uint16 = 64
	shmDefaultSpin     uint32 = 128

	// Byte offsets of all fields in the 64-byte region header.
	shmHeaderMagicOff      = 0
	shmHeaderVersionOff    = 4
	shmHeaderHeaderLenOff  = 6
	shmHeaderOwnerPidOff   = 8
	shmHeaderOwnerGenOff   = 12
	shmHeaderReqOffOff     = 16
	shmHeaderReqCapOff     = 20
	shmHeaderRespOffOff    = 24
	shmHeaderRespCapOff    = 28
	shmHeaderReqSeqOff     = 32
	shmHeaderRespSeqOff    = 40
	shmHeaderReqLenOff     = 48
	shmHeaderRespLenOff    = 52
	shmHeaderReqSignalOff  = 56
	shmHeaderRespSignalOff = 60

	// futex operations
	futexWait = 0
	futexWake = 1

	shmMaxPath = 256
)

// ---------------------------------------------------------------------------
//  Errors
// ---------------------------------------------------------------------------

var (
	ErrShmPathTooLong = errors.New("SHM path exceeds limit")
	ErrShmOpen        = errors.New("SHM open failed")
	ErrShmTruncate    = errors.New("SHM ftruncate failed")
	ErrShmMmap        = errors.New("SHM mmap failed")
	ErrShmBadMagic    = errors.New("SHM header magic mismatch")
	ErrShmBadVersion  = errors.New("SHM header version mismatch")
	ErrShmBadHeader   = errors.New("SHM header_len mismatch")
	ErrShmBadSize     = errors.New("SHM file too small for declared areas")
	ErrShmAddrInUse   = errors.New("SHM region owned by live server")
	ErrShmNotReady    = errors.New("SHM server not ready")
	ErrShmMsgTooLarge = errors.New("message exceeds SHM area capacity")
	ErrShmTimeout     = errors.New("SHM futex wait timed out")
	ErrShmBadParam    = errors.New("invalid SHM argument")
	ErrShmPeerDead    = errors.New("SHM owner process has exited")
)

// ---------------------------------------------------------------------------
//  Role
// ---------------------------------------------------------------------------

// ShmRole distinguishes server vs client SHM contexts.
type ShmRole int

const (
	ShmRoleServer ShmRole = 1
	ShmRoleClient ShmRole = 2
)

// ---------------------------------------------------------------------------
//  SHM context
// ---------------------------------------------------------------------------

// ShmContext is a handle to a shared memory region.
type ShmContext struct {
	role ShmRole
	fd   int
	data []byte // mmap'd region (via syscall.Mmap)

	requestOffset    uint32
	requestCapacity  uint32
	responseOffset   uint32
	responseCapacity uint32

	localReqSeq  uint64
	localRespSeq uint64

	SpinTries       uint32
	ownerGeneration uint32 // cached for PID reuse detection
	path            string
}

// Role returns the context role.
func (c *ShmContext) Role() ShmRole { return c.role }

// Fd returns the file descriptor.
func (c *ShmContext) Fd() int { return c.fd }

// OwnerAlive checks if the region's owner process is still alive.
func (c *ShmContext) OwnerAlive() bool {
	if len(c.data) < int(shmHeaderLen) {
		return false
	}
	pid := int32(binary.LittleEndian.Uint32(c.data[shmHeaderOwnerPidOff : shmHeaderOwnerPidOff+4]))
	if !pidAlive(int(pid)) {
		return false
	}
	// Verify generation matches to detect PID reuse.
	// Skip check if cached generation is 0 (legacy region).
	if c.ownerGeneration != 0 {
		curGen := binary.LittleEndian.Uint32(c.data[shmHeaderOwnerGenOff : shmHeaderOwnerGenOff+4])
		if curGen != c.ownerGeneration {
			return false
		}
	}
	return true
}

// ---------------------------------------------------------------------------
//  Server API
// ---------------------------------------------------------------------------

// ShmServerCreate creates a SHM region at {runDir}/{serviceName}.ipcshm.
func ShmServerCreate(runDir, serviceName string, reqCapacity, respCapacity uint32) (*ShmContext, error) {
	path, err := buildShmPath(runDir, serviceName)
	if err != nil {
		return nil, err
	}

	// Stale recovery
	stale := checkShmStale(path)
	if stale == shmStaleLive {
		return nil, ErrShmAddrInUse
	}

	// Round capacities
	reqCap := shmAlign64(reqCapacity)
	respCap := shmAlign64(respCapacity)

	reqOff := shmAlign64(uint32(shmHeaderLen))
	respOff := shmAlign64(reqOff + reqCap)
	regionSize := int(respOff + respCap)

	// Create file
	f, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0600)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrShmOpen, err)
	}
	fd := int(f.Fd())

	if err := syscall.Ftruncate(fd, int64(regionSize)); err != nil {
		f.Close()
		os.Remove(path)
		return nil, fmt.Errorf("%w: %v", ErrShmTruncate, err)
	}

	data, err := syscall.Mmap(fd, 0, regionSize,
		syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		f.Close()
		os.Remove(path)
		return nil, fmt.Errorf("%w: %v", ErrShmMmap, err)
	}

	// Zero region (Mmap may return zero-filled from kernel, but be explicit)
	for i := range data {
		data[i] = 0
	}

	// Use a time-based generation to detect PID reuse across restarts.
	now := time.Now()
	generation := uint32(now.Unix()) ^ uint32(now.Nanosecond()>>10)

	// Write header fields (little-endian)
	binary.LittleEndian.PutUint32(data[shmHeaderMagicOff:shmHeaderMagicOff+4], shmRegionMagic)
	binary.LittleEndian.PutUint16(data[shmHeaderVersionOff:shmHeaderVersionOff+2], shmRegionVersion)
	binary.LittleEndian.PutUint16(data[shmHeaderHeaderLenOff:shmHeaderHeaderLenOff+2], uint16(shmHeaderLen))
	binary.LittleEndian.PutUint32(data[shmHeaderOwnerPidOff:shmHeaderOwnerPidOff+4], uint32(int32(os.Getpid())))
	binary.LittleEndian.PutUint32(data[shmHeaderOwnerGenOff:shmHeaderOwnerGenOff+4], generation)
	binary.LittleEndian.PutUint32(data[shmHeaderReqOffOff:shmHeaderReqOffOff+4], reqOff)
	binary.LittleEndian.PutUint32(data[shmHeaderReqCapOff:shmHeaderReqCapOff+4], reqCap)
	binary.LittleEndian.PutUint32(data[shmHeaderRespOffOff:shmHeaderRespOffOff+4], respOff)
	binary.LittleEndian.PutUint32(data[shmHeaderRespCapOff:shmHeaderRespCapOff+4], respCap)

	// Release fence: ensure header writes are visible before clients
	atomic.StoreUint32((*uint32)(unsafe.Pointer(&data[shmHeaderReqSignalOff])), 0)

	// Close the os.File but keep the fd open (Mmap holds a reference).
	// Actually, we need to keep the fd ourselves for the context.
	// os.File.Close() would close the fd, so we dup first or just
	// not use os.File.Close(). We already have the fd from f.Fd().
	// Trick: prevent Go's finalizer from closing fd.
	// The safe way: dup the fd, then close the file.
	newFd, err := syscall.Dup(fd)
	if err != nil {
		syscall.Munmap(data)
		f.Close()
		os.Remove(path)
		return nil, fmt.Errorf("%w: dup: %v", ErrShmOpen, err)
	}
	f.Close() // closes original fd

	return &ShmContext{
		role:             ShmRoleServer,
		fd:               newFd,
		data:             data,
		requestOffset:    reqOff,
		requestCapacity:  reqCap,
		responseOffset:   respOff,
		responseCapacity: respCap,
		localReqSeq:      0,
		localRespSeq:     0,
		SpinTries:        shmDefaultSpin,
		ownerGeneration:  generation,
		path:             path,
	}, nil
}

// ShmDestroy destroys a server SHM region (munmap, close, unlink).
func (c *ShmContext) ShmDestroy() {
	if c.data != nil {
		syscall.Munmap(c.data)
		c.data = nil
	}
	if c.fd >= 0 {
		syscall.Close(c.fd)
		c.fd = -1
	}
	if c.path != "" {
		os.Remove(c.path)
		c.path = ""
	}
}

// ---------------------------------------------------------------------------
//  Client API
// ---------------------------------------------------------------------------

// ShmClientAttach attaches to an existing SHM region.
func ShmClientAttach(runDir, serviceName string) (*ShmContext, error) {
	path, err := buildShmPath(runDir, serviceName)
	if err != nil {
		return nil, err
	}

	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrShmOpen, err)
	}
	fd := int(f.Fd())

	info, err := f.Stat()
	if err != nil {
		f.Close()
		return nil, fmt.Errorf("%w: stat: %v", ErrShmOpen, err)
	}

	fileSize := int(info.Size())
	if fileSize < int(shmHeaderLen) {
		f.Close()
		return nil, ErrShmNotReady
	}

	data, err := syscall.Mmap(fd, 0, fileSize,
		syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		f.Close()
		return nil, fmt.Errorf("%w: %v", ErrShmMmap, err)
	}

	// Acquire fence
	atomic.LoadUint32((*uint32)(unsafe.Pointer(&data[shmHeaderReqSignalOff])))

	// Validate header
	magic := binary.LittleEndian.Uint32(data[shmHeaderMagicOff : shmHeaderMagicOff+4])
	if magic != shmRegionMagic {
		syscall.Munmap(data)
		f.Close()
		return nil, ErrShmBadMagic
	}

	version := binary.LittleEndian.Uint16(data[shmHeaderVersionOff : shmHeaderVersionOff+2])
	if version != shmRegionVersion {
		syscall.Munmap(data)
		f.Close()
		return nil, ErrShmBadVersion
	}

	hdrLen := binary.LittleEndian.Uint16(data[shmHeaderHeaderLenOff : shmHeaderHeaderLenOff+2])
	if hdrLen != uint16(shmHeaderLen) {
		syscall.Munmap(data)
		f.Close()
		return nil, ErrShmBadHeader
	}

	reqOff := binary.LittleEndian.Uint32(data[shmHeaderReqOffOff : shmHeaderReqOffOff+4])
	reqCap := binary.LittleEndian.Uint32(data[shmHeaderReqCapOff : shmHeaderReqCapOff+4])
	respOff := binary.LittleEndian.Uint32(data[shmHeaderRespOffOff : shmHeaderRespOffOff+4])
	respCap := binary.LittleEndian.Uint32(data[shmHeaderRespCapOff : shmHeaderRespCapOff+4])

	// Validate region size
	reqEnd := int(reqOff) + int(reqCap)
	respEnd := int(respOff) + int(respCap)
	needed := reqEnd
	if respEnd > needed {
		needed = respEnd
	}
	if fileSize < needed {
		syscall.Munmap(data)
		f.Close()
		return nil, ErrShmBadSize
	}

	// Read current sequence numbers
	curReqSeq, err := atomicLoadU64(data, shmHeaderReqSeqOff)
	if err != nil {
		syscall.Munmap(data)
		f.Close()
		return nil, fmt.Errorf("%w: load req_seq: %v", ErrShmBadParam, err)
	}
	curRespSeq, err := atomicLoadU64(data, shmHeaderRespSeqOff)
	if err != nil {
		syscall.Munmap(data)
		f.Close()
		return nil, fmt.Errorf("%w: load resp_seq: %v", ErrShmBadParam, err)
	}
	ownerGen := binary.LittleEndian.Uint32(data[shmHeaderOwnerGenOff : shmHeaderOwnerGenOff+4])

	// Dup fd and close file
	newFd, err := syscall.Dup(fd)
	if err != nil {
		syscall.Munmap(data)
		f.Close()
		return nil, fmt.Errorf("%w: dup: %v", ErrShmOpen, err)
	}
	f.Close()

	return &ShmContext{
		role:             ShmRoleClient,
		fd:               newFd,
		data:             data,
		requestOffset:    reqOff,
		requestCapacity:  reqCap,
		responseOffset:   respOff,
		responseCapacity: respCap,
		localReqSeq:      curReqSeq,
		localRespSeq:     curRespSeq,
		SpinTries:        shmDefaultSpin,
		ownerGeneration:  ownerGen,
		path:             path,
	}, nil
}

// ShmClose closes a client SHM context (no unlink).
func (c *ShmContext) ShmClose() {
	if c.data != nil {
		syscall.Munmap(c.data)
		c.data = nil
	}
	if c.fd >= 0 {
		syscall.Close(c.fd)
		c.fd = -1
	}
}

// ---------------------------------------------------------------------------
//  Data plane
// ---------------------------------------------------------------------------

// ShmSend publishes a message. The message must include the 32-byte
// outer header + payload, exactly as sent over UDS.
func (c *ShmContext) ShmSend(msg []byte) error {
	if c.data == nil || len(msg) == 0 {
		return fmt.Errorf("%w: null context or empty message", ErrShmBadParam)
	}

	var areaOff, areaCap uint32
	var seqOff, lenOff, sigOff int

	if c.role == ShmRoleClient {
		areaOff = c.requestOffset
		areaCap = c.requestCapacity
		seqOff = shmHeaderReqSeqOff
		lenOff = shmHeaderReqLenOff
		sigOff = shmHeaderReqSignalOff
	} else {
		areaOff = c.responseOffset
		areaCap = c.responseCapacity
		seqOff = shmHeaderRespSeqOff
		lenOff = shmHeaderRespLenOff
		sigOff = shmHeaderRespSignalOff
	}

	if uint32(len(msg)) > areaCap {
		return ErrShmMsgTooLarge
	}

	// 1. Write message data
	copy(c.data[areaOff:], msg)

	// 2. Store message length (release)
	if err := atomicStoreU32(c.data, lenOff, uint32(len(msg))); err != nil {
		return fmt.Errorf("%w: store msg_len: %v", ErrShmBadParam, err)
	}

	// 3. Increment sequence number (release)
	if err := atomicAddU64(c.data, seqOff, 1); err != nil {
		return fmt.Errorf("%w: add seq: %v", ErrShmBadParam, err)
	}

	// 4. Wake peer via futex
	if err := atomicAddU32(c.data, sigOff, 1); err != nil {
		return fmt.Errorf("%w: add signal: %v", ErrShmBadParam, err)
	}
	futexWakeCall(c.data, sigOff, 1)

	// Track locally
	if c.role == ShmRoleClient {
		c.localReqSeq++
	} else {
		c.localRespSeq++
	}

	return nil
}

// ShmReceive receives a message. Returns a slice into the SHM region
// (zero-copy). Valid until the next Send/Receive call.
func (c *ShmContext) ShmReceive(timeoutMs uint32) ([]byte, error) {
	if c.data == nil {
		return nil, fmt.Errorf("%w: null context", ErrShmBadParam)
	}

	var areaOff uint32
	var seqOff, lenOff, sigOff int
	var expectedSeq uint64

	if c.role == ShmRoleServer {
		areaOff = c.requestOffset
		seqOff = shmHeaderReqSeqOff
		lenOff = shmHeaderReqLenOff
		sigOff = shmHeaderReqSignalOff
		expectedSeq = c.localReqSeq + 1
	} else {
		areaOff = c.responseOffset
		seqOff = shmHeaderRespSeqOff
		lenOff = shmHeaderRespLenOff
		sigOff = shmHeaderRespSignalOff
		expectedSeq = c.localRespSeq + 1
	}

	// Phase 1: spin
	observed := false
	for i := uint32(0); i < c.SpinTries; i++ {
		cur, err := atomicLoadU64(c.data, seqOff)
		if err != nil {
			return nil, fmt.Errorf("%w: load seq: %v", ErrShmBadParam, err)
		}
		if cur >= expectedSeq {
			observed = true
			break
		}
		spinPause()
	}

	// Phase 2: futex wait
	if !observed {
		sigVal, err := atomicLoadU32(c.data, sigOff)
		if err != nil {
			return nil, fmt.Errorf("%w: load signal: %v", ErrShmBadParam, err)
		}

		// Check once more after reading signal
		cur, err := atomicLoadU64(c.data, seqOff)
		if err != nil {
			return nil, fmt.Errorf("%w: load seq: %v", ErrShmBadParam, err)
		}
		if cur < expectedSeq {
			var ts *syscall.Timespec
			if timeoutMs > 0 {
				ts = &syscall.Timespec{
					Sec:  int64(timeoutMs / 1000),
					Nsec: int64(timeoutMs%1000) * 1_000_000,
				}
			}

			ret := futexWaitCall(c.data, sigOff, sigVal, ts)
			if ret < 0 {
				errno := syscall.Errno(-ret)
				if errno == syscall.ETIMEDOUT {
					return nil, ErrShmTimeout
				}
			}

			// After waking, verify sequence advanced
			cur, err = atomicLoadU64(c.data, seqOff)
			if err != nil {
				return nil, fmt.Errorf("%w: load seq: %v", ErrShmBadParam, err)
			}
			if cur < expectedSeq {
				return nil, ErrShmTimeout
			}
		}
	}

	// Read message length
	mlen, err := atomicLoadU32(c.data, lenOff)
	if err != nil {
		return nil, fmt.Errorf("%w: load msg_len: %v", ErrShmBadParam, err)
	}

	// Advance local tracking
	if c.role == ShmRoleServer {
		c.localReqSeq = expectedSeq
	} else {
		c.localRespSeq = expectedSeq
	}

	// Return zero-copy slice
	return c.data[areaOff : areaOff+mlen], nil
}

// ---------------------------------------------------------------------------
//  Internal helpers
// ---------------------------------------------------------------------------

func shmAlign64(v uint32) uint32 {
	return (v + (shmRegionAlignment - 1)) & ^(shmRegionAlignment - 1)
}

func buildShmPath(runDir, serviceName string) (string, error) {
	path := filepath.Join(runDir, serviceName+".ipcshm")
	if len(path) >= shmMaxPath {
		return "", ErrShmPathTooLong
	}
	return path, nil
}

func pidAlive(pid int) bool {
	if pid <= 0 {
		return false
	}
	err := syscall.Kill(pid, 0)
	return err == nil || err == syscall.EPERM
}

// Atomic operations on the mmap'd region with bounds checking.

var errShmOutOfBounds = errors.New("SHM atomic: offset out of bounds")

func atomicLoadU64(data []byte, off int) (uint64, error) {
	if off < 0 || off+8 > len(data) {
		return 0, errShmOutOfBounds
	}
	ptr := (*uint64)(unsafe.Pointer(&data[off]))
	return atomic.LoadUint64(ptr), nil
}

func atomicLoadU32(data []byte, off int) (uint32, error) {
	if off < 0 || off+4 > len(data) {
		return 0, errShmOutOfBounds
	}
	ptr := (*uint32)(unsafe.Pointer(&data[off]))
	return atomic.LoadUint32(ptr), nil
}

func atomicStoreU32(data []byte, off int, val uint32) error {
	if off < 0 || off+4 > len(data) {
		return errShmOutOfBounds
	}
	ptr := (*uint32)(unsafe.Pointer(&data[off]))
	atomic.StoreUint32(ptr, val)
	return nil
}

func atomicAddU64(data []byte, off int, val uint64) error {
	if off < 0 || off+8 > len(data) {
		return errShmOutOfBounds
	}
	ptr := (*uint64)(unsafe.Pointer(&data[off]))
	atomic.AddUint64(ptr, val)
	return nil
}

func atomicAddU32(data []byte, off int, val uint32) error {
	if off < 0 || off+4 > len(data) {
		return errShmOutOfBounds
	}
	ptr := (*uint32)(unsafe.Pointer(&data[off]))
	atomic.AddUint32(ptr, val)
	return nil
}

func futexWakeCall(data []byte, off int, count int) int {
	if off < 0 || off+4 > len(data) {
		return -1
	}
	addr := unsafe.Pointer(&data[off])
	r1, _, _ := syscall.Syscall6(
		syscall.SYS_FUTEX,
		uintptr(addr),
		uintptr(futexWake),
		uintptr(count),
		0, 0, 0,
	)
	return int(r1)
}

func futexWaitCall(data []byte, off int, expected uint32, ts *syscall.Timespec) int {
	if off < 0 || off+4 > len(data) {
		return -1
	}
	addr := unsafe.Pointer(&data[off])
	var tsPtr uintptr
	if ts != nil {
		tsPtr = uintptr(unsafe.Pointer(ts))
	}
	r1, _, errno := syscall.Syscall6(
		syscall.SYS_FUTEX,
		uintptr(addr),
		uintptr(futexWait),
		uintptr(expected),
		tsPtr,
		0, 0,
	)
	if errno != 0 {
		return -int(errno)
	}
	return int(r1)
}

// ---------------------------------------------------------------------------
//  Stale region recovery
// ---------------------------------------------------------------------------

type shmStaleResult int

const (
	shmStaleNotExist shmStaleResult = iota
	shmStaleRecovered
	shmStaleLive
	shmStaleInvalid
)

func checkShmStale(path string) shmStaleResult {
	info, err := os.Stat(path)
	if err != nil {
		return shmStaleNotExist
	}

	if info.Size() < int64(shmHeaderLen) {
		os.Remove(path)
		return shmStaleInvalid
	}

	f, err := os.Open(path)
	if err != nil {
		os.Remove(path)
		return shmStaleInvalid
	}

	data, err := syscall.Mmap(int(f.Fd()), 0, int(shmHeaderLen),
		syscall.PROT_READ, syscall.MAP_SHARED)
	f.Close()
	if err != nil {
		os.Remove(path)
		return shmStaleInvalid
	}

	magic := binary.LittleEndian.Uint32(data[shmHeaderMagicOff : shmHeaderMagicOff+4])
	if magic != shmRegionMagic {
		syscall.Munmap(data)
		os.Remove(path)
		return shmStaleInvalid
	}

	ownerPid := int(int32(binary.LittleEndian.Uint32(data[shmHeaderOwnerPidOff : shmHeaderOwnerPidOff+4])))
	syscall.Munmap(data)

	if pidAlive(ownerPid) {
		return shmStaleLive
	}

	// Dead owner — stale
	os.Remove(path)
	return shmStaleRecovered
}
