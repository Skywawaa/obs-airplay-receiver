#pragma once

#include <stdbool.h>
#include <stdint.h>

struct mdns_publisher;

struct mdns_service {
	char name[256];        /* Service instance name */
	char type[128];        /* e.g. "_airplay._tcp" */
	uint16_t port;         /* Service port */
	char **txt_records;    /* NULL-terminated array of "key=value" strings */
	int txt_count;
};

/* Create and start an mDNS publisher */
struct mdns_publisher *mdns_publisher_create(const struct mdns_service *svc);

/* Stop and destroy the publisher */
void mdns_publisher_destroy(struct mdns_publisher *pub);

/* Update the published service name */
bool mdns_publisher_update_name(struct mdns_publisher *pub, const char *name);
