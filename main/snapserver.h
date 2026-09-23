/*
 * ESP32-S3 Mini Snapserver - audio/binary protocol (port 1704)
 *
 * Besides starting the streaming server this header exposes a snapshot API
 * for the connected clients so that the JSON-RPC control server on port 1705
 * can report them in Server.GetStatus.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SNAPSERVER_PORT 1704

/* Upper bound for snapserver_get_clients(). */
#define SNAPSERVER_MAX_CLIENTS 10

/*
 * Snapshot of one connected client, filled from its Hello message plus
 * runtime state. Strings are always NUL terminated.
 */
typedef struct {
    char id[64];          /* Hello "ID", usually the MAC              */
    char name[64];        /* Hello "ClientName"                       */
    char hostname[64];    /* Hello "HostName"                         */
    char mac[24];         /* Hello "MAC"                              */
    char arch[24];        /* Hello "Arch"                             */
    char os[32];          /* Hello "OS"                               */
    char version[24];     /* Hello "Version"                          */
    char ip[16];          /* peer address of the TCP connection       */
    int32_t instance;     /* Hello "Instance", defaults to 1          */
    int32_t protocol_ver; /* Hello "SnapStreamProtocolVersion"        */

    bool connected;       /* always true for entries returned here    */
    int32_t volume_percent;
    bool muted;
    int32_t latency_ms;

    bool is_snapmesh;     /* one of our own clients (Hello "SnapMesh") */
    /*
     * Mesh hops between this server and the client: 1 for a direct child
     * of the root AP, one more per relay below it. From the level our own
     * clients report in their Hello; a foreign client only gets 1 when it
     * sits on the root AP itself, -1 (unknown) otherwise.
     */
    int32_t hops;

    int32_t last_seen_sec;  /* wall clock of the last received message */
    int32_t last_seen_usec;
} snapserver_client_info_t;

/* Starts the streaming server and audio tasks. Idempotent. */
esp_err_t snapserver_start(void);

/*
 * Hash over the connected client set, for cheap change detection. Changes
 * whenever a client joins, leaves or reports a different id, and whenever
 * a client's name, volume, mute, latency or mesh level changes.
 */
uint32_t snapserver_client_set_hash(void);

/*
 * Copies up to max_clients entries of currently connected clients into out.
 * Returns the number of entries written. Safe to call from other tasks.
 */
size_t snapserver_get_clients(snapserver_client_info_t *out,
                              size_t max_clients);

/*
 * Number of connected clients -- the same ones snapserver_get_clients()
 * lists -- and, in *own if not NULL, how many of them are ours (SnapMesh).
 * Cheap: counts under the lock without copying anything.
 */
size_t snapserver_client_count(size_t *own);

/*
 * IPs of currently connected clients that are direct children of this
 * node's own AP -- i.e. a matching MAC exists in
 * esp_wifi_ap_get_sta_list(), the same "level 1" test stats_task already
 * uses for its per-client RSSI. Used by voice_announce.c to restrict
 * announcement fan-out to the one hop that cannot stall mid-rearrangement
 * for seconds at a time; a client one or more levels down is deliberately
 * excluded, not queried differently. Returns the number of entries written.
 */
size_t snapserver_get_level1_client_ips(char ips[][16], size_t max_ips);

/*
 * Announcement state (voice_announce.c). While active, ServerSettings
 * carries "announcement":true, and our own clients (they say so in their
 * Hello) silence their music for its duration wherever they sit in the
 * mesh, playing the announcement if it reaches them. The control app's own
 * mute and volume are untouched and keep applying to both.
 *
 * A foreign Snapcast client knows no such flag, so for it the announcement
 * is folded into muted=true unless it is level 1 -- decided by MAC, not IP,
 * since behind NAPT a deeper client shows up at its parent's IP.
 *
 * snapserver_set_announcement() only flips the flag; it is cheap and safe
 * from any task, and a client that connects from then on gets the right
 * state in its handshake. snapserver_refresh_announcement() pushes
 * ServerSettings to every connected client whose announcement mute differs
 * from what it was last sent -- after a flip, and periodically while an
 * announcement runs so a client that changes level is caught. It may block
 * on each client's send mutex, so call it from a task with room to wait,
 * not from the JSON-RPC handler.
 */
void snapserver_set_announcement(bool active);
void snapserver_refresh_announcement(void);

/*
 * The three setters below apply a change requested via the control
 * protocol or the web device list, and store the result so the client gets
 * it again when it reconnects (client_store.h). That store writes flash:
 * call them only from a task whose stack is in internal RAM.
 */

/*
 * Applies a volume change requested via the control protocol.
 * Returns true if a client with this id exists.
 */
bool snapserver_set_client_volume(const char *id,
                                  int32_t percent,
                                  bool muted);

/*
 * Applies a latency change requested via the control protocol.
 * Returns true if a client with this id exists.
 */
bool snapserver_set_client_latency(const char *id, int32_t latency_ms);

/*
 * Applies a name change requested via the control protocol.
 * Returns true if a client with this id exists.
 */
bool snapserver_set_client_name(const char *id, const char *name);

/*
 * Sends a JSON request to one of our own clients over its Snapcast
 * connection and waits for its JSON answer -- the only way to reach a
 * client that sits behind another node's NAPT. The client answers from
 * webconfig_handle_remote_request(). On success *reply holds the answer,
 * NUL-terminated, for the caller to free().
 *
 * ESP_ERR_NOT_FOUND: no connected SnapMesh client with this id.
 * ESP_ERR_TIMEOUT: no answer within timeout_ms.
 * ESP_ERR_INVALID_STATE: the connection closed before the answer came.
 * One request at a time; a second caller waits for the first.
 */
esp_err_t snapserver_remote_request(const char *id,
                                    const char *json,
                                    char **reply,
                                    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
