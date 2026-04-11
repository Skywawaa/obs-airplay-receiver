/*
 * Minimal HTTP server for AirPlay control channel
 *
 * Handles the HTTP endpoints that Apple devices use to negotiate
 * and control AirPlay sessions (GET /info, POST /pair-setup, etc.)
 */

#include "http-server.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#endif

struct http_server {
	struct http_server_config config;
	socket_t listen_sock;
	volatile bool running;
	uint16_t port;

#ifdef _WIN32
	HANDLE thread;
#else
	pthread_t thread;
#endif
};

/* ---------- HTTP Parsing ---------- */

static bool parse_request_line(const char *line, struct http_request *req)
{
	/* "METHOD /uri HTTP/1.1" */
	const char *p = line;
	const char *end;

	/* Method */
	end = strchr(p, ' ');
	if (!end)
		return false;
	int len = (int)(end - p);
	if (len >= (int)sizeof(req->method))
		return false;
	memcpy(req->method, p, len);
	req->method[len] = '\0';

	/* URI */
	p = end + 1;
	end = strchr(p, ' ');
	if (!end)
		return false;
	len = (int)(end - p);
	if (len >= HTTP_MAX_URI)
		return false;
	memcpy(req->uri, p, len);
	req->uri[len] = '\0';

	/* Protocol */
	p = end + 1;
	/* Trim \r\n */
	len = (int)strlen(p);
	while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == '\n'))
		len--;
	if (len >= (int)sizeof(req->protocol))
		return false;
	memcpy(req->protocol, p, len);
	req->protocol[len] = '\0';

	return true;
}

static bool parse_header(const char *line, struct http_header *hdr)
{
	const char *colon = strchr(line, ':');
	if (!colon)
		return false;

	int name_len = (int)(colon - line);
	if (name_len >= (int)sizeof(hdr->name))
		return false;
	memcpy(hdr->name, line, name_len);
	hdr->name[name_len] = '\0';

	/* Skip ": " */
	const char *val = colon + 1;
	while (*val == ' ')
		val++;

	/* Trim trailing \r\n */
	int val_len = (int)strlen(val);
	while (val_len > 0 && (val[val_len - 1] == '\r' || val[val_len - 1] == '\n'))
		val_len--;

	if (val_len >= (int)sizeof(hdr->value))
		val_len = (int)sizeof(hdr->value) - 1;
	memcpy(hdr->value, val, val_len);
	hdr->value[val_len] = '\0';

	return true;
}

static bool read_http_request(socket_t client, struct http_request *req)
{
	memset(req, 0, sizeof(*req));

	/* Read headers (up to 8KB) */
	char header_buf[8192];
	int total = 0;
	bool found_end = false;

	while (total < (int)sizeof(header_buf) - 1) {
		int n = recv(client, header_buf + total, 1, 0);
		if (n <= 0)
			return false;
		total += n;
		header_buf[total] = '\0';

		/* Look for \r\n\r\n */
		if (total >= 4 &&
		    memcmp(header_buf + total - 4, "\r\n\r\n", 4) == 0) {
			found_end = true;
			break;
		}
	}

	if (!found_end)
		return false;

	/* Parse request line and headers */
	char *line = strtok(header_buf, "\n");
	if (!line)
		return false;

	if (!parse_request_line(line, req))
		return false;

	while ((line = strtok(NULL, "\n")) != NULL) {
		if (line[0] == '\r' || line[0] == '\0')
			break;

		if (req->header_count < HTTP_MAX_HEADERS) {
			parse_header(line, &req->headers[req->header_count]);
			req->header_count++;
		}
	}

	/* Check for Content-Length and read body */
	const char *cl = http_request_get_header(req, "Content-Length");
	if (cl) {
		req->content_length = (size_t)atol(cl);
		if (req->content_length > 0 &&
		    req->content_length <= HTTP_MAX_BODY) {
			req->body = malloc(req->content_length);
			if (req->body) {
				req->body_length = 0;
				while (req->body_length < req->content_length) {
					int n = recv(client,
						     (char *)req->body +
							     req->body_length,
						     (int)(req->content_length -
							   req->body_length),
						     0);
					if (n <= 0) {
						free(req->body);
						req->body = NULL;
						return false;
					}
					req->body_length += n;
				}
			}
		}
	}

	return true;
}

/* ---------- HTTP Response ---------- */

void http_response_add_header(struct http_response *resp, const char *name,
			      const char *value)
{
	if (resp->header_count < HTTP_MAX_HEADERS) {
		strncpy(resp->headers[resp->header_count].name, name,
			sizeof(resp->headers[0].name) - 1);
		strncpy(resp->headers[resp->header_count].value, value,
			sizeof(resp->headers[0].value) - 1);
		resp->header_count++;
	}
}

const char *http_request_get_header(const struct http_request *req,
				    const char *name)
{
	for (int i = 0; i < req->header_count; i++) {
#ifdef _WIN32
		if (_stricmp(req->headers[i].name, name) == 0)
#else
		if (strcasecmp(req->headers[i].name, name) == 0)
#endif
			return req->headers[i].value;
	}
	return NULL;
}

bool http_send_response(socket_t client, const struct http_response *resp)
{
	char header_buf[4096];
	int pos = snprintf(header_buf, sizeof(header_buf),
			   "HTTP/1.1 %d %s\r\n", resp->status_code,
			   resp->status_text ? resp->status_text : "OK");

	for (int i = 0; i < resp->header_count; i++) {
		pos += snprintf(header_buf + pos, sizeof(header_buf) - pos,
				"%s: %s\r\n", resp->headers[i].name,
				resp->headers[i].value);
	}

	if (resp->body && resp->body_length > 0) {
		pos += snprintf(header_buf + pos, sizeof(header_buf) - pos,
				"Content-Length: %zu\r\n",
				resp->body_length);
	}

	pos += snprintf(header_buf + pos, sizeof(header_buf) - pos, "\r\n");

	if (!net_send_all(client, header_buf, pos))
		return false;

	if (resp->body && resp->body_length > 0) {
		if (!net_send_all(client, resp->body, resp->body_length))
			return false;
	}

	return true;
}

/* ---------- Server thread ---------- */

static void handle_client(struct http_server *srv, socket_t client)
{
	/* Set a read timeout */
#ifdef _WIN32
	DWORD tv = 10000; /* 10s */
	setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv,
		   sizeof(tv));
#else
	struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
	setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

	/* AirPlay may send multiple requests on the same connection */
	while (srv->running) {
		struct http_request req = {0};
		if (!read_http_request(client, &req))
			break;

		struct http_response resp = {0};
		resp.status_code = 200;
		resp.status_text = "OK";

		/* Call the request handler */
		if (srv->config.handler) {
			srv->config.handler(srv->config.userdata, client, &req,
					    &resp);
		}

		/* Send the response */
		http_send_response(client, &resp);

		/* Free request body */
		if (req.body)
			free(req.body);

		/* Free response body if allocated */
		if (resp.body)
			free(resp.body);
	}

	closesocket(client);
}

#ifdef _WIN32
struct client_thread_data {
	struct http_server *srv;
	socket_t client;
};

static DWORD WINAPI client_thread_func(LPVOID arg)
{
	struct client_thread_data *ctd = (struct client_thread_data *)arg;
	handle_client(ctd->srv, ctd->client);
	free(ctd);
	return 0;
}
#else
struct client_thread_data {
	struct http_server *srv;
	socket_t client;
};

static void *client_thread_func(void *arg)
{
	struct client_thread_data *ctd = (struct client_thread_data *)arg;
	handle_client(ctd->srv, ctd->client);
	free(ctd);
	return NULL;
}
#endif

static void *server_thread_func(void *arg)
{
	struct http_server *srv = (struct http_server *)arg;

	/* Set accept timeout */
#ifdef _WIN32
	DWORD tv = 1000;
	setsockopt(srv->listen_sock, SOL_SOCKET, SO_RCVTIMEO,
		   (const char *)&tv, sizeof(tv));
#else
	struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
	setsockopt(srv->listen_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

	while (srv->running) {
		struct sockaddr_in client_addr;
		socklen_t addr_len = sizeof(client_addr);

		socket_t client = accept(srv->listen_sock,
					 (struct sockaddr *)&client_addr,
					 &addr_len);

		if (client == INVALID_SOCK)
			continue;

		/* Handle each client in a separate thread */
		struct client_thread_data *ctd =
			malloc(sizeof(struct client_thread_data));
		if (!ctd) {
			closesocket(client);
			continue;
		}
		ctd->srv = srv;
		ctd->client = client;

#ifdef _WIN32
		HANDLE t = CreateThread(NULL, 0, client_thread_func, ctd, 0,
					NULL);
		if (t)
			CloseHandle(t);
		else {
			closesocket(client);
			free(ctd);
		}
#else
		pthread_t t;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&t, &attr, client_thread_func, ctd) != 0) {
			closesocket(client);
			free(ctd);
		}
		pthread_attr_destroy(&attr);
#endif
	}

	return NULL;
}

/* ---------- Public API ---------- */

struct http_server *http_server_create(const struct http_server_config *cfg)
{
	struct http_server *srv = calloc(1, sizeof(struct http_server));
	if (!srv)
		return NULL;

	net_init();

	srv->config = *cfg;
	srv->port = cfg->port;

	srv->listen_sock = net_tcp_listen(cfg->port, 10);
	if (srv->listen_sock == INVALID_SOCK) {
		free(srv);
		return NULL;
	}

	/* Get actual port if 0 was specified */
	if (cfg->port == 0) {
		struct sockaddr_in addr;
		socklen_t len = sizeof(addr);
		if (getsockname(srv->listen_sock, (struct sockaddr *)&addr,
				&len) == 0) {
			srv->port = ntohs(addr.sin_port);
		}
	}

	srv->running = true;

#ifdef _WIN32
	srv->thread = CreateThread(NULL, 0,
				   (LPTHREAD_START_ROUTINE)server_thread_func,
				   srv, 0, NULL);
	if (!srv->thread) {
		closesocket(srv->listen_sock);
		free(srv);
		return NULL;
	}
#else
	if (pthread_create(&srv->thread, NULL, server_thread_func, srv) != 0) {
		closesocket(srv->listen_sock);
		free(srv);
		return NULL;
	}
#endif

	return srv;
}

void http_server_stop(struct http_server *srv)
{
	if (!srv)
		return;

	srv->running = false;

	/* Close the listening socket to unblock accept() */
	if (srv->listen_sock != INVALID_SOCK) {
		closesocket(srv->listen_sock);
		srv->listen_sock = INVALID_SOCK;
	}

#ifdef _WIN32
	WaitForSingleObject(srv->thread, 5000);
	CloseHandle(srv->thread);
#else
	pthread_join(srv->thread, NULL);
#endif
}

void http_server_destroy(struct http_server *srv)
{
	if (!srv)
		return;

	http_server_stop(srv);
	free(srv);
}

uint16_t http_server_get_port(struct http_server *srv)
{
	return srv ? srv->port : 0;
}
