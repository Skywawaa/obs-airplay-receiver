/*
 * mDNS Publisher - Advertises the AirPlay service via multicast DNS
 *
 * Implements a minimal mDNS responder that publishes:
 *   - _airplay._tcp.local  (AirPlay service)
 *   - SRV, TXT, A records
 */

#include "mdns-publish.h"
#include "net-utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#define MDNS_ADDR  "224.0.0.251"
#define MDNS_PORT  5353

#define DNS_TYPE_A     1
#define DNS_TYPE_PTR   12
#define DNS_TYPE_TXT   16
#define DNS_TYPE_SRV   33
#define DNS_CLASS_IN   1
#define DNS_CACHE_FLUSH 0x8000

#define MAX_MDNS_PACKET 1500
#define DEFAULT_TTL 4500

struct mdns_publisher {
	struct mdns_service service;
	socket_t sock;
	volatile bool running;
	char hostname[256];
	char ip_addr[64];

#ifdef _WIN32
	HANDLE thread;
#else
	pthread_t thread;
#endif
};

/* ---------- DNS name encoding ---------- */

static int dns_encode_name(uint8_t *buf, int max_len, const char *name)
{
	int pos = 0;
	const char *p = name;

	while (*p) {
		const char *dot = strchr(p, '.');
		int label_len = dot ? (int)(dot - p) : (int)strlen(p);

		if (pos + 1 + label_len >= max_len)
			return -1;

		buf[pos++] = (uint8_t)label_len;
		memcpy(buf + pos, p, label_len);
		pos += label_len;

		if (dot)
			p = dot + 1;
		else
			break;
	}

	if (pos < max_len)
		buf[pos++] = 0; /* root label */

	return pos;
}

/* ---------- Build mDNS response packet ---------- */

static int build_mdns_response(struct mdns_publisher *pub, uint8_t *pkt,
			       int max_len)
{
	/* DNS Header */
	memset(pkt, 0, 12);
	/* Flags: response, authoritative */
	pkt[2] = 0x84;
	pkt[3] = 0x00;

	int answer_count = 0;
	int pos = 12;

	/* Service name for encoding */
	char svc_name[512];
	snprintf(svc_name, sizeof(svc_name), "%s.%s.local",
		 pub->service.name, pub->service.type);

	char svc_type_local[256];
	snprintf(svc_type_local, sizeof(svc_type_local), "%s.local",
		 pub->service.type);

	char host_local[256];
	snprintf(host_local, sizeof(host_local), "%s.local", pub->hostname);

	/* --- PTR record: _airplay._tcp.local -> <name>._airplay._tcp.local --- */
	int n = dns_encode_name(pkt + pos, max_len - pos, svc_type_local);
	if (n < 0)
		return -1;
	pos += n;

	/* Type PTR, Class IN */
	write_be16(pkt + pos, DNS_TYPE_PTR);
	pos += 2;
	write_be16(pkt + pos, DNS_CLASS_IN);
	pos += 2;
	/* TTL */
	write_be32(pkt + pos, DEFAULT_TTL);
	pos += 4;

	/* RDATA: the full service name */
	uint8_t rdata_buf[512];
	int rdata_len = dns_encode_name(rdata_buf, sizeof(rdata_buf), svc_name);
	if (rdata_len < 0)
		return -1;

	write_be16(pkt + pos, (uint16_t)rdata_len);
	pos += 2;
	memcpy(pkt + pos, rdata_buf, rdata_len);
	pos += rdata_len;
	answer_count++;

	/* --- SRV record: <name>._airplay._tcp.local -> host:port --- */
	n = dns_encode_name(pkt + pos, max_len - pos, svc_name);
	if (n < 0)
		return -1;
	pos += n;

	write_be16(pkt + pos, DNS_TYPE_SRV);
	pos += 2;
	write_be16(pkt + pos, DNS_CLASS_IN | DNS_CACHE_FLUSH);
	pos += 2;
	write_be32(pkt + pos, DEFAULT_TTL);
	pos += 4;

	/* SRV RDATA: priority(2) + weight(2) + port(2) + target */
	uint8_t srv_rdata[512];
	write_be16(srv_rdata, 0);    /* priority */
	write_be16(srv_rdata + 2, 0); /* weight */
	write_be16(srv_rdata + 4, pub->service.port);
	int target_len =
		dns_encode_name(srv_rdata + 6, sizeof(srv_rdata) - 6, host_local);
	if (target_len < 0)
		return -1;
	int srv_rdata_len = 6 + target_len;

	write_be16(pkt + pos, (uint16_t)srv_rdata_len);
	pos += 2;
	memcpy(pkt + pos, srv_rdata, srv_rdata_len);
	pos += srv_rdata_len;
	answer_count++;

	/* --- TXT record --- */
	n = dns_encode_name(pkt + pos, max_len - pos, svc_name);
	if (n < 0)
		return -1;
	pos += n;

	write_be16(pkt + pos, DNS_TYPE_TXT);
	pos += 2;
	write_be16(pkt + pos, DNS_CLASS_IN | DNS_CACHE_FLUSH);
	pos += 2;
	write_be32(pkt + pos, DEFAULT_TTL);
	pos += 4;

	/* TXT RDATA: length-prefixed strings */
	uint8_t txt_rdata[1024];
	int txt_rdata_len = 0;
	for (int i = 0; i < pub->service.txt_count; i++) {
		int slen = (int)strlen(pub->service.txt_records[i]);
		if (slen > 255)
			slen = 255;
		txt_rdata[txt_rdata_len++] = (uint8_t)slen;
		memcpy(txt_rdata + txt_rdata_len, pub->service.txt_records[i],
		       slen);
		txt_rdata_len += slen;
	}
	if (txt_rdata_len == 0)
		txt_rdata[txt_rdata_len++] = 0; /* empty TXT */

	write_be16(pkt + pos, (uint16_t)txt_rdata_len);
	pos += 2;
	memcpy(pkt + pos, txt_rdata, txt_rdata_len);
	pos += txt_rdata_len;
	answer_count++;

	/* --- A record: hostname.local -> IP --- */
	n = dns_encode_name(pkt + pos, max_len - pos, host_local);
	if (n < 0)
		return -1;
	pos += n;

	write_be16(pkt + pos, DNS_TYPE_A);
	pos += 2;
	write_be16(pkt + pos, DNS_CLASS_IN | DNS_CACHE_FLUSH);
	pos += 2;
	write_be32(pkt + pos, DEFAULT_TTL);
	pos += 4;

	/* A RDATA: 4 bytes IPv4 */
	write_be16(pkt + pos, 4);
	pos += 2;

	struct in_addr in;
	inet_pton(AF_INET, pub->ip_addr, &in);
	memcpy(pkt + pos, &in.s_addr, 4);
	pos += 4;
	answer_count++;

	/* Fix answer count in header */
	write_be16(pkt + 6, (uint16_t)answer_count);

	return pos;
}

/* ---------- Check if an mDNS query matches our service ---------- */

static bool query_matches_service(const uint8_t *pkt, int len,
				  const char *service_type)
{
	if (len < 12)
		return false;

	/* Check it's a query (QR=0) */
	if (pkt[2] & 0x80)
		return false;

	uint16_t qcount = read_be16(pkt + 4);
	if (qcount == 0)
		return false;

	/* Simple check: look for our service type string in the packet */
	char needle[256];
	snprintf(needle, sizeof(needle), "%s", service_type);

	/* Convert the service type labels to search in DNS wire format */
	/* Quick heuristic: just search for the key part of the name */
	const char *key = "_airplay";
	for (int i = 12; i < len - (int)strlen(key); i++) {
		if (memcmp(pkt + i, key + 1, strlen(key) - 1) == 0)
			return true;
	}

	return false;
}

/* ---------- mDNS responder thread ---------- */

static void *mdns_thread_func(void *arg)
{
	struct mdns_publisher *pub = (struct mdns_publisher *)arg;

	uint8_t recv_buf[MAX_MDNS_PACKET];
	uint8_t send_buf[MAX_MDNS_PACKET];

	struct sockaddr_in mcast_addr;
	memset(&mcast_addr, 0, sizeof(mcast_addr));
	mcast_addr.sin_family = AF_INET;
	mcast_addr.sin_port = htons(MDNS_PORT);
	inet_pton(AF_INET, MDNS_ADDR, &mcast_addr.sin_addr);

	/* Send an initial announcement */
	int pkt_len = build_mdns_response(pub, send_buf, sizeof(send_buf));
	if (pkt_len > 0) {
		sendto(pub->sock, (const char *)send_buf, pkt_len, 0,
		       (struct sockaddr *)&mcast_addr, sizeof(mcast_addr));
	}

	/* Set a receive timeout so we can check the running flag */
#ifdef _WIN32
	DWORD tv = 1000;
	setsockopt(pub->sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv,
		   sizeof(tv));
#else
	struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
	setsockopt(pub->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

	int announce_timer = 0;

	while (pub->running) {
		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);
		int n = recvfrom(pub->sock, (char *)recv_buf, sizeof(recv_buf),
				 0, (struct sockaddr *)&from, &from_len);

		if (n > 0) {
			/* Check if this is a query for our service */
			if (query_matches_service(recv_buf, n,
						  pub->service.type)) {
				pkt_len = build_mdns_response(pub, send_buf,
							      sizeof(send_buf));
				if (pkt_len > 0) {
					sendto(pub->sock,
					       (const char *)send_buf, pkt_len,
					       0,
					       (struct sockaddr *)&mcast_addr,
					       sizeof(mcast_addr));
				}
			}
		}

		/* Periodic re-announcement every ~60 seconds */
		announce_timer++;
		if (announce_timer >= 60) {
			announce_timer = 0;
			pkt_len = build_mdns_response(pub, send_buf,
						      sizeof(send_buf));
			if (pkt_len > 0) {
				sendto(pub->sock, (const char *)send_buf,
				       pkt_len, 0,
				       (struct sockaddr *)&mcast_addr,
				       sizeof(mcast_addr));
			}
		}
	}

	return NULL;
}

/* ---------- Public API ---------- */

struct mdns_publisher *mdns_publisher_create(const struct mdns_service *svc)
{
	struct mdns_publisher *pub = calloc(1, sizeof(struct mdns_publisher));
	if (!pub)
		return NULL;

	memcpy(&pub->service, svc, sizeof(struct mdns_service));

	/* Duplicate TXT records */
	if (svc->txt_count > 0 && svc->txt_records) {
		pub->service.txt_records =
			calloc(svc->txt_count, sizeof(char *));
		for (int i = 0; i < svc->txt_count; i++) {
			pub->service.txt_records[i] = strdup(svc->txt_records[i]);
		}
	}

	/* Get hostname and IP */
	if (gethostname(pub->hostname, sizeof(pub->hostname)) != 0) {
		strncpy(pub->hostname, "obs-airplay",
			sizeof(pub->hostname) - 1);
	}

	if (!net_get_ipv4(pub->ip_addr, sizeof(pub->ip_addr))) {
		strncpy(pub->ip_addr, "127.0.0.1", sizeof(pub->ip_addr) - 1);
	}

	/* Create mDNS socket */
	pub->sock = net_udp_bind(MDNS_PORT);
	if (pub->sock == INVALID_SOCK) {
		/* Try with a random port if 5353 is taken */
		pub->sock = net_udp_bind(0);
		if (pub->sock == INVALID_SOCK) {
			free(pub);
			return NULL;
		}
	}

	/* Join multicast group */
	net_join_multicast(pub->sock, MDNS_ADDR);

	/* Enable multicast loopback */
	uint8_t loop = 1;
	setsockopt(pub->sock, IPPROTO_IP, IP_MULTICAST_LOOP,
		   (const char *)&loop, sizeof(loop));

	/* Start responder thread */
	pub->running = true;

#ifdef _WIN32
	pub->thread = CreateThread(NULL, 0,
				   (LPTHREAD_START_ROUTINE)mdns_thread_func,
				   pub, 0, NULL);
	if (!pub->thread) {
		closesocket(pub->sock);
		free(pub);
		return NULL;
	}
#else
	if (pthread_create(&pub->thread, NULL, mdns_thread_func, pub) != 0) {
		closesocket(pub->sock);
		free(pub);
		return NULL;
	}
#endif

	return pub;
}

void mdns_publisher_destroy(struct mdns_publisher *pub)
{
	if (!pub)
		return;

	pub->running = false;

	/* Send a goodbye packet (TTL=0) would be nice, but we just stop */

#ifdef _WIN32
	WaitForSingleObject(pub->thread, 3000);
	CloseHandle(pub->thread);
#else
	pthread_join(pub->thread, NULL);
#endif

	closesocket(pub->sock);

	/* Free TXT records */
	if (pub->service.txt_records) {
		for (int i = 0; i < pub->service.txt_count; i++)
			free(pub->service.txt_records[i]);
		free(pub->service.txt_records);
	}

	free(pub);
}

bool mdns_publisher_update_name(struct mdns_publisher *pub, const char *name)
{
	if (!pub)
		return false;
	strncpy(pub->service.name, name, sizeof(pub->service.name) - 1);
	return true;
}
