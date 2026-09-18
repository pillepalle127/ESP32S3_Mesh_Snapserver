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

    int32_t last_seen_sec;  /* wall clock of the last received message */
    int32_t last_seen_usec;
} snapserver_client_info_t;

/* Starts the streaming server and audio tasks. Idempotent. */
esp_err_t snapserver_start(void);

/*
 * Copies up to max_clients entries of currently connected clients into out.
 * Returns the number of entries written. Safe to call from other tasks.
 */
/*
 * Hash over the connected client set, for cheap change detection. Changes
 * whenever a client joins, leaves or reports a different id.
 */
uint32_t snapserver_client_set_hash(void);

size_t snapserver_get_clients(snapserver_client_info_t *out,
                              size_t max_clients);

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

#ifdef __cplusplus
}
#endif
