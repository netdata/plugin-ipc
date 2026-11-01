//go:build darwin

package posix

import (
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"syscall"
)

// bindOwnerOnly binds fd to path as owner-only (0600, narrowed by the umask).
// macOS rejects fchmod() on an unbound socket and creates the socket file
// 0777 & ~umask. The socket is bound inside a private 0700 directory
// {serviceName}.d, narrowed to owner-only there, then renamed into place, so
// it is never reachable with a wider mode and the process-global umask is
// untouched. The caller holds the endpoint lock, so a leftover directory
// belongs to a crashed server and is removed first.
func bindOwnerOnly(fd int, runDir, serviceName, path string) error {
	dir := filepath.Join(runDir, serviceName+".d")
	tmp := filepath.Join(dir, "s")
	if len(tmp) >= maxSunPath {
		return ErrPathTooLong
	}
	sockErr := func(op string, err error) error { return wrapErr(ErrSocket, op+": "+err.Error()) }

	// Anything but a real directory is unlinked without following it.
	if info, err := os.Lstat(dir); err == nil {
		if info.IsDir() {
			// unlink, not os.Remove: a directory at tmp is foreign content.
			if err := syscall.Unlink(tmp); err != nil && !errors.Is(err, fs.ErrNotExist) {
				return sockErr("remove leftover socket", err)
			}
		}
		if err := os.Remove(dir); err != nil {
			return sockErr("remove leftover bind dir", err)
		}
	} else if !errors.Is(err, fs.ErrNotExist) {
		return sockErr("stat bind dir", err)
	}

	if err := os.Mkdir(dir, 0o700); err != nil {
		return sockErr("mkdir bind dir", err)
	}
	defer func() { _ = os.Remove(dir) }()
	// Chmod ignores the umask, so a umask without owner write cannot make the
	// directory unusable; the socket file below still gets it.
	if err := os.Chmod(dir, 0o700); err != nil {
		return sockErr("chmod bind dir", err)
	}
	if err := syscall.Bind(fd, &syscall.SockaddrUnix{Name: tmp}); err != nil {
		return sockErr("bind", err)
	}
	info, err := os.Stat(tmp)
	if err == nil {
		err = os.Chmod(tmp, info.Mode().Perm()&0o600)
	}
	if err == nil {
		err = os.Rename(tmp, path)
	}
	if err != nil {
		_ = os.Remove(tmp)
		return sockErr("place socket", err)
	}
	return nil
}
