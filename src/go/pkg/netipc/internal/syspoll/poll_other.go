//go:build unix && !linux

package syspoll

import (
	"syscall"
	"unsafe"
)

// Poll waits up to timeoutMs (negative waits indefinitely) for pfd.Events.
// It returns the poll result count and the raw errno.
func Poll(pfd *PollFd, timeoutMs int) (int, syscall.Errno) {
	r, _, errno := syscall.Syscall(
		syscall.SYS_POLL,
		uintptr(unsafe.Pointer(pfd)), // #nosec G103 -- raw poll syscall requires a pollfd pointer.
		1,
		uintptr(timeoutMs),
	)
	return int(r), errno
}
