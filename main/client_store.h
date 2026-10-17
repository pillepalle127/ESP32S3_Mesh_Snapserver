/**
 * @file client_store.h
 * @brief Per-client volume, mute, latency and name, kept across reconnects.
 *
 * The server used to start every connection at 100 %, unmuted, latency 0,
 * named after the client's hostname -- so every setting made in a control
 * app or on the device list was lost the moment a client reconnected, and
 * in a mesh that happens whenever a node changes parent. Stock Snapserver
 * keeps these per client in server.json; this is the same, in NVS, keyed
 * by the client's Snapcast ID (its MAC for every real client).
 *
 * All entries are read into RAM once by client_store_init(), and lookups
 * only ever touch that copy: the Hello that needs them is handled in a
 * task whose stack lives in PSRAM, and flash access from such a task is
 * not allowed. client_store_put() leaves the flash write to its own task
 * (internal-RAM stack); only if that task could not be created does it
 * write synchronously, so callers should still have an internal-RAM stack
 * (the JSON-RPC connection tasks and the HTTP server do).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int32_t volume_percent;
    bool muted;
    int32_t latency_ms;
    char name[64]; /* empty = not renamed, use what the client reports */
} client_store_entry_t;

/*
 * How many clients are remembered. More than SNAPSERVER_MAX_CLIENTS, so a
 * speaker that is switched off for a while is still known when it comes
 * back; past this, the entry written longest ago makes room.
 */
#define CLIENT_STORE_MAX 24

/* Loads every stored entry into RAM. Call once before the server starts. */
esp_err_t client_store_init(void);

/* Copies the stored settings for id into *out. False if none are stored. */
bool client_store_get(const char *id, client_store_entry_t *out);

/*
 * Stores the settings for id: in RAM at once, in NVS once nothing changed
 * for a few seconds (see STORE_WRITE_DELAY_MS). Skips it if nothing changed.
 */
void client_store_put(const char *id, const client_store_entry_t *entry);

/*
 * Forgets every stored client, in NVS and in RAM. For the factory reset,
 * which is followed by a reboot anyway.
 */
void client_store_erase_all(void);

/* One stored client, for handing the store to the next server. */
typedef struct {
    char id[64];
    client_store_entry_t entry;
} client_store_record_t;

/* Copies up to max stored clients into out, most recently written first. */
size_t client_store_list(client_store_record_t *out, size_t max);

/*
 * Writes records straight to NVS, for a client that is about to restart as
 * the server and has no store running (client_store_init() is server
 * only). first is the position of records[0] in the old server's list, so
 * the list can come in parts; 0 forgets everything stored so far. Writes
 * flash: call from a task with an internal-RAM stack.
 */
esp_err_t client_store_import(const client_store_record_t *records, size_t count, size_t first);
