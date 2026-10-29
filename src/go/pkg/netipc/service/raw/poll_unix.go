//go:build unix

package raw

import (
	"syscall"

	"github.com/netdata/plugin-ipc/go/pkg/netipc/internal/syspoll"
)

// pollFd polls a file descriptor for readability with a timeout in ms.
// Returns: 1 = data ready, 0 = timeout, -1 = error/hangup.
func pollFd(fd int, timeoutMs int) int {
	const maxInt32 = 1<<31 - 1
	if fd < 0 || fd > maxInt32 {
		return -1
	}
	pfd := syspoll.PollFd{
		Fd:     int32(fd), // #nosec G115 -- fd is checked against int32 range above.
		Events: syspoll.In,
	}

	n, errno := syspoll.Poll(&pfd, timeoutMs)
	if n < 0 {
		if errno == syscall.EINTR {
			return 0
		}
		return -1
	}

	if n == 0 {
		return 0
	}

	if pfd.Revents&(syspoll.Err|syspoll.Hup|syspoll.Nval) != 0 {
		return -1
	}

	if pfd.Revents&syspoll.In != 0 {
		return 1
	}

	return 0
}
