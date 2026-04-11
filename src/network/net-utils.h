#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
typedef SOCKET socket_t;
#define INVALID_SOCK INVALID_SOCKET
#define sock_errno WSAGetLastError()
#define SOCK_EAGAIN WSAEWOULDBLOCK
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int socket_t;
#define INVALID_SOCK (-1)
#define closesocket close
#define sock_errno errno
#define SOCK_EAGAIN EAGAIN
#endif

/* Initialize network subsystem (Winsock on Windows) */
bool net_init(void);
void net_cleanup(void);

/* Create a TCP listening socket */
socket_t net_tcp_listen(uint16_t port, int backlog);

/* Create a UDP socket bound to a port */
socket_t net_udp_bind(uint16_t port);

/* Set socket to non-blocking mode */
bool net_set_nonblocking(socket_t sock);

/* Set socket options for reuse */
bool net_set_reuse(socket_t sock);

/* Join a multicast group */
bool net_join_multicast(socket_t sock, const char *group_addr);

/* Get the MAC address of the primary interface */
bool net_get_mac_address(uint8_t mac[6]);

/* Get the primary IPv4 address */
bool net_get_ipv4(char *buf, size_t buf_size);

/* Send all bytes */
bool net_send_all(socket_t sock, const void *data, size_t len);

/* Receive exactly n bytes */
bool net_recv_exact(socket_t sock, void *buf, size_t len);

/* Read a 16-bit big-endian value */
static inline uint16_t read_be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

/* Read a 32-bit big-endian value */
static inline uint32_t read_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Write a 16-bit big-endian value */
static inline void write_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v);
}

/* Write a 32-bit big-endian value */
static inline void write_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)(v);
}
