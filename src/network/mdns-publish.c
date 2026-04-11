/*
 * mDNS Publisher using Apple's Bonjour DNS-SD API (dnssd.dll)
 *
 * On Windows, Apple ships Bonjour which includes mDNSResponder.exe
 * service and dnssd.dll. This service handles all mDNS publishing.
 * We register our service via the DNSServiceRegister API.
 *
 * If dnssd.dll is not available at runtime, publisher creation fails
 * gracefully - the AirPlay server still works, but devices won't
 * auto-discover it (they'd need to connect manually).
 */

#include "mdns-publish.h"
#include "net-utils.h"
#include "../log.h"
#include "../../deps/dns_sd.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct mdns_publisher {
	struct mdns_service service;
	DNSServiceRef service_ref;
	bool registered;
};

static void register_callback(DNSServiceRef sdRef,
			      DNSServiceFlags flags,
			      DNSServiceErrorType errorCode,
			      const char *name,
			      const char *regtype,
			      const char *domain,
			      void *context)
{
	(void)sdRef;
	(void)flags;
	(void)regtype;
	(void)domain;
	(void)context;

	if (errorCode == kDNSServiceErr_NoError) {
		ap_info("mDNS service registered: %s.%s%s",
			name ? name : "(null)", regtype ? regtype : "",
			domain ? domain : "");
	} else {
		ap_error("mDNS registration failed with error %d",
			 (int)errorCode);
	}
}

struct mdns_publisher *mdns_publisher_create(const struct mdns_service *svc)
{
	struct mdns_publisher *pub = calloc(1, sizeof(struct mdns_publisher));
	if (!pub)
		return NULL;

	memcpy(&pub->service, svc, sizeof(struct mdns_service));

	/* Build TXT record */
	TXTRecordRef txt;
	uint8_t txt_buf[2048];
	TXTRecordCreate(&txt, sizeof(txt_buf), txt_buf);

	for (int i = 0; i < svc->txt_count; i++) {
		const char *entry = svc->txt_records[i];
		const char *eq = strchr(entry, '=');
		if (!eq)
			continue;

		char key[128];
		size_t key_len = (size_t)(eq - entry);
		if (key_len >= sizeof(key))
			key_len = sizeof(key) - 1;
		memcpy(key, entry, key_len);
		key[key_len] = '\0';

		const char *value = eq + 1;
		size_t value_len = strlen(value);
		if (value_len > 255)
			value_len = 255;

		TXTRecordSetValue(&txt, key, (uint8_t)value_len, value);
	}

	uint16_t txt_len = TXTRecordGetLength(&txt);
	const void *txt_ptr = TXTRecordGetBytesPtr(&txt);

	/* Build regtype: "_airplay._tcp" format (no trailing dot) */
	char regtype[128];
	strncpy(regtype, svc->type, sizeof(regtype) - 1);
	regtype[sizeof(regtype) - 1] = '\0';

	/* Register the service - port must be in network byte order */
	DNSServiceErrorType err = DNSServiceRegister(
		&pub->service_ref,
		0,                    /* flags */
		0,                    /* interfaceIndex = all */
		svc->name,            /* service name */
		regtype,              /* _airplay._tcp */
		"local",              /* domain (NULL for default) */
		NULL,                 /* host (NULL = use default) */
		htons(svc->port),     /* port in network byte order */
		txt_len,
		txt_ptr,
		register_callback,
		pub);

	TXTRecordDeallocate(&txt);

	if (err != kDNSServiceErr_NoError) {
		ap_error("DNSServiceRegister failed: %d (is Bonjour Service running?)",
			 (int)err);
		free(pub);
		return NULL;
	}

	pub->registered = true;
	ap_info("mDNS registration requested for '%s' on %s port %d",
		svc->name, regtype, svc->port);

	return pub;
}

void mdns_publisher_destroy(struct mdns_publisher *pub)
{
	if (!pub)
		return;

	if (pub->registered && pub->service_ref) {
		DNSServiceRefDeallocate(pub->service_ref);
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
