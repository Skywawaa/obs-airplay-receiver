#pragma once

#include "net-utils.h"
#include <stdbool.h>
#include <stdint.h>

#define HTTP_MAX_HEADERS  64
#define HTTP_MAX_URI      1024
#define HTTP_MAX_BODY     (1024 * 1024)  /* 1 MB max body */

struct http_header {
	char name[256];
	char value[512];
};

struct http_request {
	char method[16];
	char uri[HTTP_MAX_URI];
	char protocol[16];
	struct http_header headers[HTTP_MAX_HEADERS];
	int header_count;
	uint8_t *body;
	size_t body_length;
	size_t content_length;
};

struct http_response {
	int status_code;
	const char *status_text;
	struct http_header headers[HTTP_MAX_HEADERS];
	int header_count;
	uint8_t *body;
	size_t body_length;
};

/* Callback for handling HTTP requests */
typedef void (*http_request_handler)(void *userdata, socket_t client,
				     const struct http_request *req,
				     struct http_response *resp);

struct http_server;

struct http_server_config {
	uint16_t port;
	http_request_handler handler;
	void *userdata;
};

/* Create and start an HTTP server */
struct http_server *http_server_create(const struct http_server_config *cfg);

/* Stop and destroy */
void http_server_stop(struct http_server *srv);
void http_server_destroy(struct http_server *srv);

/* Get the actual port (useful if 0 was specified) */
uint16_t http_server_get_port(struct http_server *srv);

/* Helper: add a response header */
void http_response_add_header(struct http_response *resp, const char *name,
			      const char *value);

/* Helper: find request header value */
const char *http_request_get_header(const struct http_request *req,
				    const char *name);

/* Send an HTTP response on a socket */
bool http_send_response(socket_t client, const struct http_response *resp);
