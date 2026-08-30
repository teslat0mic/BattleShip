/**
 * port_net.c — non-blocking UDP for the debug P2P netplay path (see port_net.h).
 *
 * Winsock on Windows, BSD sockets elsewhere. One socket, one peer.
 */

#include "port_net.h"
#include "port_log.h"

#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET port_socket_t;
#define PORT_NET_INVALID INVALID_SOCKET
#define port_net_errno() WSAGetLastError()
#define PORT_NET_WOULDBLOCK WSAEWOULDBLOCK
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int port_socket_t;
#define PORT_NET_INVALID (-1)
#define port_net_errno() errno
#define PORT_NET_WOULDBLOCK EWOULDBLOCK
#endif

static port_socket_t sPortNetSocket = PORT_NET_INVALID;
static struct sockaddr_in sPortNetPeer;
static int sPortNetLastError = 0;

/* Split "host:port" at the LAST colon so a bare IPv4 address keeps working and
 * a stray colon in the host part fails the numeric parse below rather than
 * silently truncating. */
static const char *port_net_find_port_separator(const char *text)
{
	const char *colon = NULL;
	const char *p;

	for (p = text; *p != '\0'; p++) {
		if (*p == ':') {
			colon = p;
		}
	}
	return colon;
}

static int port_net_parse(const char *spec, struct sockaddr_in *out)
{
	const char *colon;
	long port;
	char *end;
	size_t host_len;
	char host[64];

	if (spec == NULL || out == NULL) {
		return 0;
	}
	colon = port_net_find_port_separator(spec);

	if (colon == NULL || colon == spec || *(colon + 1) == '\0') {
		return 0;
	}
	host_len = (size_t)(colon - spec);

	if (host_len >= sizeof(host)) {
		return 0;
	}
	memcpy(host, spec, host_len);
	host[host_len] = '\0';

	port = strtol(colon + 1, &end, 10);

	if (*end != '\0' || port <= 0 || port > 65535) {
		return 0;
	}
	memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port = htons((unsigned short)port);

	if (strcmp(host, "*") == 0 || strcmp(host, "0.0.0.0") == 0) {
		out->sin_addr.s_addr = htonl(INADDR_ANY);
		return 1;
	}
	if (inet_pton(AF_INET, host, &out->sin_addr) != 1) {
		return 0;
	}
	return 1;
}

int port_net_parse_address(const char *spec)
{
	struct sockaddr_in probe;

	return port_net_parse(spec, &probe);
}

#ifdef _WIN32
/* WSAStartup once per process; WSACleanup is deliberately not called — the
 * process exits through TerminateProcess on the rig path, and Winsock is
 * torn down by the OS either way. */
static int port_net_winsock_startup(void)
{
	static int started = 0;
	WSADATA wsa;

	if (started) {
		return 0;
	}
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		sPortNetLastError = WSAGetLastError();
		port_log("SSB64 Net: WSAStartup failed err=%d\n", sPortNetLastError);
		return -1;
	}
	started = 1;
	return 0;
}
#endif

int port_net_open(const char *bind_spec, const char *peer_spec)
{
	struct sockaddr_in bind_address;
	int reuse = 1;

	if (sPortNetSocket != PORT_NET_INVALID) {
		return 0;
	}
	if (!port_net_parse(bind_spec, &bind_address) || !port_net_parse(peer_spec, &sPortNetPeer)) {
		port_log("SSB64 Net: invalid bind/peer address (expected IPv4 host:port)\n");
		return -1;
	}
#ifdef _WIN32
	if (port_net_winsock_startup() != 0) {
		return -1;
	}
#endif
	sPortNetSocket = socket(AF_INET, SOCK_DGRAM, 0);

	if (sPortNetSocket == PORT_NET_INVALID) {
		sPortNetLastError = port_net_errno();
		port_log("SSB64 Net: socket failed err=%d\n", sPortNetLastError);
		return -1;
	}
	setsockopt(sPortNetSocket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

	if (bind(sPortNetSocket, (struct sockaddr *)&bind_address, sizeof(bind_address)) != 0) {
		sPortNetLastError = port_net_errno();
		port_log("SSB64 Net: bind failed err=%d\n", sPortNetLastError);
		port_net_close();
		return -1;
	}
#ifdef _WIN32
	{
		u_long nonblocking = 1;

		if (ioctlsocket(sPortNetSocket, FIONBIO, &nonblocking) != 0) {
			sPortNetLastError = port_net_errno();
			port_log("SSB64 Net: non-blocking setup failed err=%d\n", sPortNetLastError);
			port_net_close();
			return -1;
		}
	}
#else
	{
		int flags = fcntl(sPortNetSocket, F_GETFL, 0);

		if (flags < 0 || fcntl(sPortNetSocket, F_SETFL, flags | O_NONBLOCK) != 0) {
			sPortNetLastError = port_net_errno();
			port_log("SSB64 Net: non-blocking setup failed err=%d\n", sPortNetLastError);
			port_net_close();
			return -1;
		}
	}
#endif
	return 0;
}

void port_net_close(void)
{
	if (sPortNetSocket != PORT_NET_INVALID) {
#ifdef _WIN32
		closesocket(sPortNetSocket);
#else
		close(sPortNetSocket);
#endif
		sPortNetSocket = PORT_NET_INVALID;
	}
}

int port_net_is_open(void)
{
	return (sPortNetSocket != PORT_NET_INVALID) ? 1 : 0;
}

int port_net_send(const void *buf, int len)
{
	int sent;

	if (sPortNetSocket == PORT_NET_INVALID || buf == NULL || len <= 0) {
		return -1;
	}
	sent = (int)sendto(sPortNetSocket, (const char *)buf, len, 0,
	                   (struct sockaddr *)&sPortNetPeer, sizeof(sPortNetPeer));

	if (sent < 0) {
		sPortNetLastError = port_net_errno();
	}
	return sent;
}

int port_net_recv(void *buf, int len)
{
	int received;

	if (sPortNetSocket == PORT_NET_INVALID || buf == NULL || len <= 0) {
		return -1;
	}
	received = (int)recvfrom(sPortNetSocket, (char *)buf, len, 0, NULL, NULL);

	if (received >= 0) {
		return received;
	}
	sPortNetLastError = port_net_errno();

	if (sPortNetLastError == PORT_NET_WOULDBLOCK
#ifndef _WIN32
	    || sPortNetLastError == EAGAIN || sPortNetLastError == EINTR
#endif
	) {
		return 0; /* nothing queued: the normal per-frame case */
	}
#ifdef _WIN32
	if (sPortNetLastError == WSAECONNRESET) {
		/* Windows reports an ICMP port-unreachable from a previous send as an
		 * error on the NEXT recvfrom of a UDP socket. The peer simply is not
		 * listening yet (both sides start independently) — not fatal. */
		return 0;
	}
#endif
	return -1;
}

void port_net_sleep_usec(int usec)
{
	if (usec <= 0) {
		return;
	}
#ifdef _WIN32
	/* Sleep() takes milliseconds and rounds to the scheduler tick; the callers
	 * use this only to pace bootstrap retries, so the coarseness is fine. */
	Sleep((DWORD)((usec + 999) / 1000));
#else
	usleep((useconds_t)usec);
#endif
}

int port_net_last_error(void)
{
	return sPortNetLastError;
}
