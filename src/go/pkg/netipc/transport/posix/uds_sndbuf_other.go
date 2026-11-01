//go:build unix && !freebsd && !darwin

package posix

// sndbufOverhead is what the kernel keeps from SO_SNDBUF for a SEQPACKET
// datagram: Linux accepts SO_SNDBUF - 32.
const sndbufOverhead = 32
