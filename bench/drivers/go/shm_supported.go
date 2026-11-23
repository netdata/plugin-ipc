//go:build linux || (freebsd && (amd64 || 386))

package main

// goShmSupported reports whether the Go POSIX transport implements SHM on
// this platform (the build tags match transport/posix/shm_posix.go).
const goShmSupported = true
