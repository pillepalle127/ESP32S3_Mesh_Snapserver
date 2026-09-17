/**
 * @file webconfig.h
 * @brief Config web UI: HTTP server, JSON API, mDNS.
 *
 * Serves the embedded config page and a small JSON API on port 80. Runs for
 * the whole lifetime of the device, regardless of whether the currently
 * active AP is the provisioning fallback or the normal mesh SoftAP -- both
 * share the same static IP (see provisioning.h), so this needs no
 * reconfiguration across a mode switch.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEBCONFIG_PORT 80

/* Starts the HTTP server and mDNS. Idempotent. */
esp_err_t webconfig_start(void);

#ifdef __cplusplus
}
#endif
