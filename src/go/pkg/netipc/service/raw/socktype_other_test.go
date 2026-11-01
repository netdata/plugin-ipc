//go:build unix && !darwin

package raw

import "syscall"

const testSockType = syscall.SOCK_SEQPACKET
