# Netdata NetIPC platform scope

Current reality of where Netdata builds and uses its vendored NetIPC copy, and
why. Decided by the user on 2026-11-11 (SOW-0071, decisions 4C and 5A).

## Where Netdata builds and uses NetIPC

| Platform | Library built in Netdata | Runtime consumers |
|---|---|---|
| Linux, every architecture Netdata builds | yes | cgroups snapshot (netdata to ebpf.plugin), cgroups lookup (netdata to apps.plugin), apps lookup (apps.plugin to network-viewer.plugin) |
| Windows | yes, linked into libnetdata | none |
| FreeBSD | no | none |
| macOS | no | none |

- The library guards are in Netdata's `src/libnetdata/CMakeLists.txt` (`if(OS_LINUX)` / `elseif(OS_WINDOWS)` around the `netipc` target, and the `libnetdata` link).
- Every consumer is compiled only under `OS_LINUX`: `NetdataPluginApps.cmake` (apps lookup server, cgroups lookup client), `NetdataPluginNetworkViewer.cmake` (apps lookup client), `NetdataOptions.cmake` (`ENABLE_CGROUPS_LOOKUP_SERVER`).
- The vendored Rust crate and Go package have no consumers in Netdata.
- NetIPC itself is built and tested on Linux, FreeBSD, macOS and Windows in this repository. The scope above is a Netdata integration choice, not a library limit.

## Why the consumers stay Linux-only

The apps lookup is the only consumer that could exist off Linux, and today it would show nothing there:

- network-viewer displays only the cgroup fields of an apps lookup item (cgroup status, orchestrator, path, name, labels). Cgroups are Linux-only.
- Lookups are matched by PID plus process start time. `docs/codec-apps-lookup.md` defines `starttime` as Linux jiffies and requires non-Linux encoders to send 0. network-viewer skips the lookup when the start time is 0, and its own start time is 0 on FreeBSD and Windows.
- The OS-neutral fields (pid, ppid, uid, comm) are already read by network-viewer from the local socket tables on each OS.

Making the apps lookup useful off Linux needs a product decision about what network-viewer should show there, followed by a cross-language protocol change (a per-platform start time, a "not applicable" cgroup status, OS-neutral fields worth showing) and Netdata portability work (the client wakes its worker through Linux `eventfd`; the Windows network-viewer does not start the client). None of this is planned.

## Constraint for any future Windows consumer

Profile bit `0x04` is `SHM_FUTEX` on POSIX but `SHM_BUSYWAIT` on Windows (`docs/level1-windows-shm.md`). The current Netdata wrappers prefer `0x04` (`apps-lookup-netipc.c`, `network-viewer-apps-lookup-client.c`). If one of them is ever built for Windows, its profile configuration must change in the same work, or every long-lived session will spin a CPU. `SHM_HYBRID` is the general-purpose Windows profile.
