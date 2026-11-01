//go:build darwin

package raw

import "syscall"

// macOS has no AF_UNIX SOCK_SEQPACKET; its UDS baseline is a stream.
const testSockType = syscall.SOCK_STREAM
