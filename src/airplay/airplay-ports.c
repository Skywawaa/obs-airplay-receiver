/*
 * Auxiliary port listeners - TCP event channel and UDP timing channel
 *
 * These exist mainly so that iOS doesn't get a "connection refused"
 * when it tries to reach the ports we advertised in the SETUP response.
 *
 * TCP event: accept connections and drain. iOS sends bplist event
 * messages that we currently discard (logging only).
 *
 * UDP timing: a minimal NTP-like server. iOS sends 8-byte requests,
 * we echo them back with a valid 32-byte NTP response. This is
 * enough for iOS to consider the timing sync "established".
 */

#include "airplay-ports.h"
#include "../log.h"
#include "../network/net-utils.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#endif

struct aux_port_listener {
	socket_t sock;
	volatile bool running;
	bool is_tcp;

#ifdef _WIN32
	HANDLE thread;
#else
	pthread_t thread;
#endif
};

/* ---------- TCP event channel thread ---------- */

static void *tcp_thread_func(void *arg)
{
	struct aux_port_listener *l = (struct aux_port_listener *)arg;

#ifdef _WIN32
	DWORD tv = 1000;
	setsockopt(l->sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv,
		   sizeof(tv));
#else
	struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
	setsockopt(l->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

	while (l->running) {
		struct sockaddr_in client_addr;
		socklen_t addr_len = sizeof(client_addr);

		socket_t client = accept(l->sock,
					 (struct sockaddr *)&client_addr,
					 &addr_len);
		if (client == INVALID_SOCK)
			continue;

		ap_info("event channel: client connected");

		/* Drain incoming data */
		char buf[4096];
#ifdef _WIN32
		DWORD rtv = 2000;
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
			   (const char *)&rtv, sizeof(rtv));
#else
		struct timeval rtv = {.tv_sec = 2, .tv_usec = 0};
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
#endif

		while (l->running) {
			int n = recv(client, buf, sizeof(buf), 0);
			if (n <= 0)
				break;
			ap_info("event channel: %d bytes received", n);
		}

		closesocket(client);
		ap_info("event channel: client disconnected");
	}

	return NULL;
}

/* ---------- UDP timing channel thread ---------- */

/* Get current NTP timestamp (seconds since 1900) */
static uint64_t ntp_time_now(void)
{
#ifdef _WIN32
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	/* FILETIME is 100ns intervals since 1601, NTP is seconds since 1900 */
	t -= 116444736000000000ULL; /* to Unix epoch (1970) */
	t /= 10;                    /* to microseconds */
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	uint64_t t = (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
#endif

	/* Unix epoch to NTP epoch: +2208988800 seconds */
	uint32_t sec = (uint32_t)(t / 1000000ULL) + 2208988800U;
	uint32_t usec = (uint32_t)(t % 1000000ULL);
	uint32_t frac = (uint32_t)(((uint64_t)usec << 32) / 1000000ULL);
	return ((uint64_t)sec << 32) | frac;
}

static void *udp_thread_func(void *arg)
{
	struct aux_port_listener *l = (struct aux_port_listener *)arg;

#ifdef _WIN32
	DWORD tv = 1000;
	setsockopt(l->sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv,
		   sizeof(tv));
#else
	struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
	setsockopt(l->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

	uint8_t buf[2048];

	while (l->running) {
		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);

		int n = recvfrom(l->sock, (char *)buf, sizeof(buf), 0,
				 (struct sockaddr *)&from, &from_len);
		if (n <= 0)
			continue;

		ap_info("timing channel: %d byte UDP request", n);

		/*
		 * Build a minimal NTP-style response.
		 * AirPlay's timing RTP packet is 32 bytes:
		 *   byte 0:    flags (0x80)
		 *   byte 1:    type (0x53 = time response)
		 *   bytes 2-3: sequence
		 *   bytes 4-11:  reference timestamp
		 *   bytes 12-19: received timestamp (origin timestamp from client)
		 *   bytes 20-27: transmit timestamp
		 *   bytes 28-31: zero
		 */
		if (n >= 12) {
			uint8_t reply[32];
			memset(reply, 0, sizeof(reply));
			reply[0] = 0x80;
			reply[1] = 0xd3; /* time response */
			reply[2] = 0x00;
			reply[3] = 0x07;

			uint64_t now = ntp_time_now();

			/* Reference timestamp = now */
			for (int i = 0; i < 8; i++)
				reply[4 + i] = (uint8_t)(now >> (56 - i * 8));

			/* Origin timestamp = echo from request (bytes 24..31) */
			if (n >= 32)
				memcpy(reply + 12, buf + 24, 8);

			/* Transmit timestamp = now */
			for (int i = 0; i < 8; i++)
				reply[20 + i] =
					(uint8_t)(now >> (56 - i * 8));

			sendto(l->sock, (const char *)reply, sizeof(reply), 0,
			       (struct sockaddr *)&from, from_len);
		}
	}

	return NULL;
}

/* ---------- Public API ---------- */

struct aux_port_listener *aux_tcp_listener_start(uint16_t port)
{
	net_init();

	struct aux_port_listener *l = calloc(1, sizeof(*l));
	if (!l)
		return NULL;

	l->sock = net_tcp_listen(port, 2);
	if (l->sock == INVALID_SOCK) {
		ap_error("aux tcp listener: failed to bind port %d", port);
		free(l);
		return NULL;
	}

	l->is_tcp = true;
	l->running = true;

#ifdef _WIN32
	l->thread = CreateThread(NULL, 0,
				 (LPTHREAD_START_ROUTINE)tcp_thread_func, l,
				 0, NULL);
	if (!l->thread) {
		closesocket(l->sock);
		free(l);
		return NULL;
	}
#else
	if (pthread_create(&l->thread, NULL, tcp_thread_func, l) != 0) {
		closesocket(l->sock);
		free(l);
		return NULL;
	}
#endif

	ap_info("event TCP listener started on port %d", port);
	return l;
}

struct aux_port_listener *aux_udp_listener_start(uint16_t port)
{
	net_init();

	struct aux_port_listener *l = calloc(1, sizeof(*l));
	if (!l)
		return NULL;

	l->sock = net_udp_bind(port);
	if (l->sock == INVALID_SOCK) {
		ap_error("aux udp listener: failed to bind port %d", port);
		free(l);
		return NULL;
	}

	l->is_tcp = false;
	l->running = true;

#ifdef _WIN32
	l->thread = CreateThread(NULL, 0,
				 (LPTHREAD_START_ROUTINE)udp_thread_func, l,
				 0, NULL);
	if (!l->thread) {
		closesocket(l->sock);
		free(l);
		return NULL;
	}
#else
	if (pthread_create(&l->thread, NULL, udp_thread_func, l) != 0) {
		closesocket(l->sock);
		free(l);
		return NULL;
	}
#endif

	ap_info("timing UDP listener started on port %d", port);
	return l;
}

void aux_listener_stop(struct aux_port_listener *l)
{
	if (!l)
		return;

	l->running = false;

	if (l->sock != INVALID_SOCK) {
		closesocket(l->sock);
		l->sock = INVALID_SOCK;
	}

#ifdef _WIN32
	if (l->thread) {
		WaitForSingleObject(l->thread, 3000);
		CloseHandle(l->thread);
	}
#else
	pthread_join(l->thread, NULL);
#endif

	free(l);
}
