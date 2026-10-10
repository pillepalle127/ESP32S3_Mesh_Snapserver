/**
 * @file wled_sync.h
 * @brief Finds WLEDs on this device's access point, makes them reachable
 *        from the whole mesh and feeds them what this device plays
 *        (AudioReactive, sync mode "Receive").
 *
 * Nothing to switch on: every device asks each station on its own SoftAP
 * for WLED's /json/info. Only once a WLED answers does the device analyse
 * its output -- at the moment it plays, so the lights match the speaker,
 * not the stream 3 s ahead -- and send it one 44-byte packet per 20 ms
 * frame, straight to the WLED's address on port 11988. Phones and mesh
 * nodes on the same access point get nothing. Each WLED is also pinged
 * twice a second; one that stops answering gets no packets until it does
 * again, so a WLED that is unplugged or restarting does not tie up the
 * radio the music shares.
 *
 * A WLED behind a client sits behind that client's NAPT and could not be
 * reached from the rest of the mesh. So each WLED gets a port from its MAC,
 * every client from its own node up to the second level forwards that port
 * one hop down, and the server takes connections on it at its own address
 * and passes them on (wled_proxy.c): its access point does not carry
 * traffic between two of its stations, but every station reaches the
 * server. The link is therefore always http://<server>:<port>/; each
 * device lists its WLEDs with it in its status (wled_sync_list()).
 *
 * The audio path only copies the frame (wled_sync_feed()); the analysis
 * runs in its own low-priority task, so a late analysis costs a WLED
 * packet, never audio.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts looking for WLEDs. The analysis buffers (about 8 KB internal RAM,
 * 4 KB PSRAM) are only taken once the first WLED shows up. Its task runs on
 * core 0 for the server (core 1 carries the Opus encoder), on core 1 for a
 * client.
 */
esp_err_t wled_sync_start(bool server_role);

/* A WLED on this device's access point, for the status page. */
#define WLED_SYNC_URL_LEN 48
typedef struct {
    char name[33];
    char url[WLED_SYNC_URL_LEN]; /* reachable from the whole mesh, "" = not yet */
} wled_sync_info_t;

/* The WLEDs on this device's access point; returns how many went to out. */
size_t wled_sync_list(wled_sync_info_t *out, size_t max);

/*
 * A node below asks to have `port` forwarded to it (POST /api/wled-forward,
 * see wled_sync.c): sets the forward on our mesh address, passes it further
 * up if needed, and fills url with where the mesh then reaches it. The node
 * below renews it every 30 s; unrenewed it lapses after 90 s.
 */
esp_err_t wled_sync_relay_forward(uint32_t child_ip, uint16_t port, char *url, size_t url_len);

/* Hands one frame of the output, before the volume, to the analysis.
 * Called from the audio path; copies and returns, never blocks. Does
 * nothing while no WLED is around. */
void wled_sync_feed(const int16_t *mono, size_t samples);

#ifdef __cplusplus
}
#endif
