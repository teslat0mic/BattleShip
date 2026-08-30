# Debug P2P netplay was compiled out on Windows (raw BSD sockets in decomp code)

**Date:** 2026-08-29
**Status:** FIXED (outer `port/port_net.{h,c}` + `CMakeLists.txt`; decomp `port-patches` `src/sys/netpeer.c`)
**Class:** host facility used directly from portable game code → whole feature unavailable on one platform

## Symptom

On Windows, `SSB64_NETPLAY=1` logged

```
SSB64 NetPeer: debug UDP netplay is not implemented on Windows yet
```

and nothing else: no socket, no bootstrap, no input exchange. Every peer-to-peer path in
`src/sys/netpeer.c` sat behind `#if defined(PORT) && !defined(_WIN32)` (10 blocks, plus the POSIX
include block and one `#if !defined(_WIN32)` in the config path). Since Windows is the most common
platform for this port, the netcode work had no way to be exercised end to end there.

## Root cause

`netpeer.c` called BSD sockets directly — `socket`/`bind`/`fcntl(O_NONBLOCK)`/`sendto`/`recvfrom`/
`close`/`inet_pton`, plus `usleep` — and stored `struct sockaddr_in` in file globals. None of that
exists (or has the same spelling) in Winsock, and `windows.h`/`winsock2.h` must not be included
from decomp C. So the platform guard was the only option at the time.

## Fix

A narrow port-layer shim, `port/port_net.h` + `port/port_net.c`: one non-blocking UDP socket, one
peer, `host:port` string specs (`*`/`0.0.0.0` meaning "any"), Winsock on Windows and BSD sockets
elsewhere. `netpeer.c` now declares the six shim functions the way it already declared `port_log`,
keeps its packet formats and protocol exactly as they were, and every
`#if defined(PORT) && !defined(_WIN32)` becomes `#ifdef PORT`. The parsing helpers
(`syNetPeerParseIPv4Address`, `syNetPeerFindPortSeparator`, `syNetPeerStringEquals`) moved into the
shim and were deleted from `netpeer.c`; address specs are stored as strings
(`sSYNetPeerBindSpec`/`sSYNetPeerPeerSpec`) and validated with `port_net_parse_address()`.

Two Windows-specific details are handled in the shim rather than leaking into game code:

- `ioctlsocket(FIONBIO)` instead of `fcntl(O_NONBLOCK)`.
- `WSAECONNRESET` from `recvfrom`: Windows reports an earlier send's ICMP port-unreachable as an
  error on the *next* receive of a UDP socket. Both peers start independently, so this is normal
  during connect-up; the shim maps it to "nothing queued" instead of counting a dropped packet.
  Also note `port_net_recv()` returns 0 for "nothing queued", so the receive loops in `netpeer.c`
  break on `<= 0` (a `< 0`-only test would spin forever).

`CMakeLists.txt` links `ws2_32` on Windows. `port/*.c` is already globbed, so `cmake -B build` must
be re-run once to pick the new file up.

## Verification

- **Windows ↔ Windows, loopback:** two instances (host `127.0.0.1:41234`, client `:41235`,
  bootstrap on, input delay 2, no controller so both peers send neutral local inputs while four
  CPU fighters make the match non-trivial). Bootstrap metadata exchange, BATTLE_READY/BATTLE_START
  barrier, release on `client-ack`, then continuous input exchange. The two per-tick gameplay-state
  traces (`docs/state_trace.md`) are **identical on all 1781 common ticks**.
- **Windows (MSVC) ↔ WSL Ubuntu (clang), real UDP over the WSL virtual NIC:** same setup,
  **identical on all 1175 common ticks** — two compilers, two operating systems, one simulation.
  This is the first end-to-end demonstration that the port's simulation is deterministic across
  hosts *over the network*, not just in offline replay comparisons.
- The trace-length difference between peers in each run is only the `SSB64_MAX_FRAMES` cap landing
  at different wall-clock moments (one process starts ~300 ms earlier); the compared range is
  every tick both peers simulated.
- Replay battery (26 files) unchanged on both compilers with netplay disabled.

## Audit hook

Any host facility called directly from `decomp/src` (sockets, threads, time, files) will end up
behind a platform `#if` and silently disable a feature on some platform. The port layer already
owns logging, exit, window and audio; sockets now join it. `grep -rn "include <sys/socket.h>\|
<arpa/inet.h>\|<winsock" decomp/src` should stay empty.
