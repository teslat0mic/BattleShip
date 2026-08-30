#pragma once

/**
 * port_net.h — minimal non-blocking UDP socket for the debug P2P netplay path.
 *
 * The decomp's `src/sys/netpeer.c` used BSD sockets directly, which meant the
 * whole netplay path was compiled out on Windows (`#if defined(PORT) &&
 * !defined(_WIN32)`). Sockets are a host facility, so they belong here in the
 * port layer, behind one narrow interface: one UDP socket, one peer, no
 * blocking, no threads. Winsock on Windows, BSD sockets elsewhere.
 *
 * Address specs are the same `host:port` strings the SSB64_NETPLAY_BIND /
 * SSB64_NETPLAY_PEER env vars already used; `*` and `0.0.0.0` mean "any" for
 * the bind host. IPv4 only, matching the original.
 *
 * All functions are safe to call before open (they report failure) and are
 * called only from the game coroutine, so no locking is needed.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Validate a "host:port" spec without touching the socket. 1 = valid, 0 = not. */
int port_net_parse_address(const char *spec);

/* Open the UDP socket: bind to `bind_spec`, remember `peer_spec` as the
 * destination for port_net_send(). Non-blocking, SO_REUSEADDR. Returns 0 on
 * success, -1 on failure (the reason is logged through port_log). Calling it
 * while already open returns 0. */
int port_net_open(const char *bind_spec, const char *peer_spec);

/* Close the socket if open. Safe to call repeatedly. */
void port_net_close(void);

/* 1 while the socket is open. */
int port_net_is_open(void);

/* Send `len` bytes to the configured peer. Returns bytes sent, or -1. */
int port_net_send(const void *buf, int len);

/* Receive one datagram, at most `len` bytes. Returns the byte count, 0 when
 * nothing is queued (the non-blocking "would block" case), or -1 on a real
 * error. Datagrams from any source are accepted, as before. */
int port_net_recv(void *buf, int len);

/* Sleep the calling thread for `usec` microseconds (bootstrap retry pacing);
 * Sleep() on Windows, usleep() elsewhere. */
void port_net_sleep_usec(int usec);

/* Last error code from the platform's socket layer (errno / WSAGetLastError),
 * for logging only. */
int port_net_last_error(void);

#ifdef __cplusplus
}
#endif
