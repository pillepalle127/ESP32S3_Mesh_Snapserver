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
void snapclient_set_network_available(bool available);

#ifdef __cplusplus
}
#endif
