//go:build darwin

package posix

import "syscall"

// macOS has no AF_UNIX SOCK_SEQPACKET. Its baseline is a stream socket that
// carries the same packets, each delimited by the length its header declares
// (docs/level1-posix-uds.md, "macOS stream baseline").
const (
	sockType        = syscall.SOCK_STREAM
	streamTransport = true
	// staleDialNetwork is the net package's name for sockType.
	staleDialNetwork = "unix"
	// darwin's syscall package has no MSG_NOSIGNAL; setNoSigpipe covers it.
	sendFlags = 0
)

// setNoSigpipe keeps a write to a closed peer from raising SIGPIPE in an
// embedding process (SO_NOSIGPIPE is macOS's documented way).
func setNoSigpipe(fd int) {
	_ = syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_NOSIGPIPE, 1)
}

// kernelPacketLimit: a stream has no per-message size cap; the fixed cap
// keeps the first packet's buffer bounded and the chunking code shared.
func kernelPacketLimit(int) uint32 {
	return packetSizeMax
}
