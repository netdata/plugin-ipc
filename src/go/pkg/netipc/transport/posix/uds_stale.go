//go:build unix

package posix

import (
	"errors"
	"net"
	"os"
	"syscall"
)

type staleResult int

const (
	staleNotExist   staleResult = 0
	staleRecovered  staleResult = 1
	staleLiveServer staleResult = 2
)

func runDirAllowsStaleUnlink(runDir string) bool {
	info, err := os.Stat(runDir)
	if err != nil || !info.IsDir() {
		return false
	}
	st, ok := info.Sys().(*syscall.Stat_t)
	if !ok || st.Uid != uint32(os.Geteuid()) {
		return false
	}
	return info.Mode().Perm()&0022 == 0
}

func checkAndRecoverStale(path string, allowStaleUnlink bool) staleResult {
	_, err := os.Stat(path)
	if err != nil {
		return staleNotExist
	}

	conn, err := net.Dial("unixpacket", path)
	if err == nil {
		_ = conn.Close()
		return staleLiveServer
	}

	if errors.Is(err, syscall.ENOENT) {
		return staleNotExist
	}
	if errors.Is(err, syscall.ECONNREFUSED) {
		if !allowStaleUnlink {
			return staleLiveServer
		}
		if removeErr := os.Remove(path); removeErr != nil {
			if os.IsNotExist(removeErr) {
				return staleNotExist
			}
			return staleLiveServer
		}
		return staleRecovered
	}
	return staleLiveServer
}
