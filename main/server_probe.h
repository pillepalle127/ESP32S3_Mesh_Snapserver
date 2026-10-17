/**
 * @file server_probe.h
 * @brief Startup check for a device set to server: does another server
 *        already run in the mesh? Then this one joins it as a client.
 *
 * Two servers under the same SSID make two separate meshes, and the
 * clients split between them at random. So before a device set to server
 * opens its mesh, it connects to the mesh SSID as a plain station and asks
 * 192.168.5.1 -- the address only a server's own AP has, reachable through
 * any client that is connected to one -- for /api/status. An answer means
 * a server runs, and this boot becomes a client. No mesh, or only clients
 * without a server (they answer at 192.168.4.1 and have no way on): this
 * one becomes the server.
 *
 * The station exists only for the check: Wi-Fi is set up for it, and torn
 * down again, before the bridge and Mesh-Lite set it up their way.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The role this boot runs, for a device whose stored role is server and
 * that boots into the mesh (not the provisioning AP). DEVICE_ROLE_CLIENT
 * if another server answered, with its device ID in server_id ("" if it
 * did not say). On a handover boot (device_config_take_handover()) the old
 * server is still on its way out: it checks again until it is gone, and
 * only joins as a client if it is still there after the wait.
 */
uint8_t server_probe_decide_role(const device_config_t *cfg, char *server_id, size_t server_id_len);

#ifdef __cplusplus
}
#endif
