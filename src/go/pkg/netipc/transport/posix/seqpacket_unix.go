//go:build unix

package posix

import "syscall"

func syscallEADDRINUSE() error      { return syscall.EADDRINUSE }
func syscallECONNREFUSED() error    { return syscall.ECONNREFUSED }
func syscallECONNRESET() error      { return syscall.ECONNRESET }
func syscallENOTSOCK() error        { return syscall.ENOTSOCK }
func syscallEACCES() syscall.Errno  { return syscall.EACCES }
func syscallENOTSUP() syscall.Errno { return syscall.ENOTSUP }
func syscallEPROTO() syscall.Errno  { return syscall.EPROTO }
func syscallErrno(v uint32) error   { return syscall.Errno(v) }
