/*
 * ESP32-S3 Mini Snapserver - Control Server (JSON-RPC, port 1705)
 *
 * Minimal Snapcast control interface so that GUI controllers (e.g. the
 * Android "Snapcast" app / Snapdroid, or the web UI) can connect, read the
 * server status and switch from "disconnected" to "connected".
 *
 * The control server does not write to the audio socket, but it queries the
 * Snapserver client registry to build status responses. Call
 * snapcontrol_start() after networking and snapserver initialization.
 */
#ifndef SNAPCONTROL_H
#define SNAPCONTROL_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SNAPCONTROL_PORT 1705

/* Starts the JSON-RPC control server task. Idempotent. */
esp_err_t snapcontrol_start(void);

#ifdef __cplusplus
}
#endif

#endif /* SNAPCONTROL_H */
