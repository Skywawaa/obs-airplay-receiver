/*
 * Network utility functions - cross-platform socket helpers
 */

#include "net-utils.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
static bool wsa_initialized = false;
#endif

bool net_init(void)
{
#ifdef _WIN32
	if (!wsa_initialized) {
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
			return false;
		wsa_initialized = true;
	}
#endif
	return true;
}

void net_cleanup(void)
{
#ifdef _WIN32
	if (wsa_initialized) {
		WSACleanup();
		wsa_initialized = false;
	}
#endif
}

socket_t net_tcp_listen(uint16_t port, int backlog)
{
	socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == INVALID_SOCK)
		return INVALID_SOCK;

	net_set_reuse(sock);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		closesocket(sock);
		return INVALID_SOCK;
	}

	if (listen(sock, backlog) < 0) {
		closesocket(sock);
		return INVALID_SOCK;
	}

	return sock;
}

socket_t net_udp_bind(uint16_t port)
{
	socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCK)
		return INVALID_SOCK;

	net_set_reuse(sock);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		closesocket(sock);
		return INVALID_SOCK;
	}

	return sock;
}

bool net_set_nonblocking(socket_t sock)
{
#ifdef _WIN32
	u_long mode = 1;
	return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(sock, F_GETFL, 0);
	if (flags < 0)
		return false;
	return fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool net_set_reuse(socket_t sock)
{
	int yes = 1;
	return setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes,
			  sizeof(yes)) == 0;
}

bool net_join_multicast(socket_t sock, const char *group_addr)
{
	struct ip_mreq mreq;
	memset(&mreq, 0, sizeof(mreq));
	mreq.imr_multiaddr.s_addr = inet_addr(group_addr);
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);

	return setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
			  (const char *)&mreq, sizeof(mreq)) == 0;
}

bool net_get_mac_address(uint8_t mac[6])
{
#ifdef _WIN32
	ULONG buf_len = 15000;
	PIP_ADAPTER_ADDRESSES addrs = (PIP_ADAPTER_ADDRESSES)malloc(buf_len);
	if (!addrs)
		return false;

	DWORD ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX,
					 NULL, addrs, &buf_len);
	if (ret == ERROR_BUFFER_OVERFLOW) {
		free(addrs);
		addrs = (PIP_ADAPTER_ADDRESSES)malloc(buf_len);
		if (!addrs)
			return false;
		ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX,
					   NULL, addrs, &buf_len);
	}

	if (ret != NO_ERROR) {
		free(addrs);
		return false;
	}

	bool found = false;
	for (PIP_ADAPTER_ADDRESSES p = addrs; p != NULL; p = p->Next) {
		if (p->IfType == IF_TYPE_ETHERNET_CSMACD ||
		    p->IfType == IF_TYPE_IEEE80211) {
			if (p->PhysicalAddressLength == 6) {
				memcpy(mac, p->PhysicalAddress, 6);
				found = true;
				break;
			}
		}
	}

	free(addrs);
	return found;
#else
	/* Linux/macOS: read from /sys/class/net or use ioctl */
	FILE *f = fopen("/sys/class/net/eth0/address", "r");
	if (!f)
		f = fopen("/sys/class/net/en0/address", "r");
	if (!f)
		f = fopen("/sys/class/net/wlan0/address", "r");
	if (!f) {
		/* Fallback: generate a random MAC */
		srand((unsigned)time(NULL));
		for (int i = 0; i < 6; i++)
			mac[i] = (uint8_t)(rand() & 0xFF);
		mac[0] = (mac[0] & 0xFE) | 0x02; /* locally administered */
		return true;
	}

	unsigned int m[6];
	if (fscanf(f, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3],
		   &m[4], &m[5]) == 6) {
		for (int i = 0; i < 6; i++)
			mac[i] = (uint8_t)m[i];
		fclose(f);
		return true;
	}

	fclose(f);
	return false;
#endif
}

bool net_get_ipv4(char *buf, size_t buf_size)
{
#ifdef _WIN32
	char hostname[256];
	if (gethostname(hostname, sizeof(hostname)) != 0)
		return false;

	struct addrinfo hints = {0}, *result = NULL;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(hostname, NULL, &hints, &result) != 0)
		return false;

	if (result) {
		struct sockaddr_in *addr =
			(struct sockaddr_in *)result->ai_addr;
		const char *ip =
			inet_ntop(AF_INET, &addr->sin_addr, buf, buf_size);
		freeaddrinfo(result);
		return ip != NULL;
	}

	freeaddrinfo(result);
	return false;
#else
	/* Quick method: create a UDP socket and connect to 8.8.8.8 */
	socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == INVALID_SOCK)
		return false;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(53);
	inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);

	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		closesocket(sock);
		return false;
	}

	struct sockaddr_in local;
	socklen_t local_len = sizeof(local);
	if (getsockname(sock, (struct sockaddr *)&local, &local_len) < 0) {
		closesocket(sock);
		return false;
	}

	closesocket(sock);
	return inet_ntop(AF_INET, &local.sin_addr, buf, buf_size) != NULL;
#endif
}

bool net_send_all(socket_t sock, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	size_t sent = 0;

	while (sent < len) {
		int ret = send(sock, (const char *)(p + sent),
			       (int)(len - sent), 0);
		if (ret <= 0)
			return false;
		sent += ret;
	}

	return true;
}

bool net_recv_exact(socket_t sock, void *buf, size_t len)
{
	uint8_t *p = (uint8_t *)buf;
	size_t received = 0;

	while (received < len) {
		int ret = recv(sock, (char *)(p + received),
			       (int)(len - received), 0);
		if (ret <= 0)
			return false;
		received += ret;
	}

	return true;
}
