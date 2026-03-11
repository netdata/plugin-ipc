# Windows Transport Sources

This directory contains the native Win32 transport implementation used by the
reusable C `netipc` library on Windows.

Files in this directory:

- `netipc_named_pipe.c`: baseline Named Pipe transport plus negotiation.
- `netipc_shm_hybrid_win.c`: negotiated shared-memory fast profiles used after
  the initial Named Pipe handshake.
- `netipc_shm_hybrid_win.h`: private interface shared by the Windows transport
  sources.

The Windows transport is intended to be built from an MSYS2 `mingw64` or
`ucrt64` shell, but it targets native Win32 APIs and must not target the MSYS
runtime.
