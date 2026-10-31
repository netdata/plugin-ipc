//go:build freebsd

package posix

// sndbufOverhead is what the kernel keeps from SO_SNDBUF for a SEQPACKET
// datagram: FreeBSD accepts the full SO_SNDBUF.
const sndbufOverhead = 0
