#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Auxiliary port listeners for AirPlay:
 *
 *   eventPort  (TCP) - receives event messages from client
 *   timingPort (UDP) - NTP-like timing sync
 *
 * iOS connects to both of these after the initial SETUP response.
 * If they are not listening, iOS silently times out.
 *
 * Full implementation requires handling the NTP protocol and
 * parsing event bplists. For initial connectivity, we just accept
 * the connections and consume data.
 */

struct aux_port_listener;

struct aux_port_listener *aux_tcp_listener_start(uint16_t port);
struct aux_port_listener *aux_udp_listener_start(uint16_t port);

void aux_listener_stop(struct aux_port_listener *l);
