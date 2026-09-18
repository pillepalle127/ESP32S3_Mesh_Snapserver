/**
 * @file snapclient.h
 * @brief Snapcast protocol client: connects to a Snapserver, decodes Opus
 *        WireChunks and feeds the decoded mono PCM to audio_sink.c.
 *
 * Ported from the ESP32_Mesh_Snapclient reference project's
 * snapclient_glue.c, stripped of A2DP/source_arbiter and adapted to this
 * project's mono Snapcast stream (our own server's CodecHeader carries
 * channels=1, see snapserver.c's send_codec_header()) and to audio_sink.c's
 * network-source API instead of a generic multi-source arbiter.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the snapclient task. The actual TCP connect only proceeds once
 * snapclient_set_network_available(true) has been called at least once
 * (see mesh_client.c, which calls it after IP_EVENT_STA_GOT_IP). */
esp_err_t snapclient_start(const char *host, uint16_t port);

/*
 * Reports mesh/IP network availability. false immediately aborts any
 * in-flight connection attempt or open socket via shutdown(); true releases
 * an immediate reconnect. Safe to call from any task/event handler.
 */
/*
 * Points the client at a (possibly different) Snapserver address. The host
 * used to be captured once at snapclient_start() and never revisited, so a
 * node whose first resolution was the fallback, or whose parent changed,
 * kept dialling a stale address forever -- visible as an endless run of
 * "TCP connect aborted (timeout)" while the server was up and reachable.
 * Safe to call from an event handler; it takes effect on the next connect.
 */
void snapclient_set_server_host(const char *host);

/*
 * Registers a resolver the client calls before every connect attempt, so a
 * changed mesh topology is picked up without waiting for a new DHCP lease.
 *
 * Pushing the address on IP_EVENT_STA_GOT_IP alone is not enough: a node
 * that keeps its lease while the path to the root changes underneath it
 * never gets a new event, and was seen retrying one address for minutes --
 * "TCP connect to 192.168.5.1:1704 aborted (timeout)" every two seconds
 * with nothing else in the log.
 */
void snapclient_set_host_resolver(void (*resolver)(char *out, size_t out_len));

void snapclient_set_network_available(bool available);

#ifdef __cplusplus
}
#endif
