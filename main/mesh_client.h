/**
 * @file mesh_client.h
 * @brief Public startup interface for the non-root Mesh-Lite client role.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Joins the mesh as a non-root relay (never a leaf: a leaf can't accept
 * children, which would end a tube-shaped multi-hop topology after one
 * hop -- see the design plan) and starts the Snapcast client once an IP is
 * obtained. Falls back to the open provisioning AP under the same
 * conditions as mesh_root_start() (see provisioning_decide()).
 */
esp_err_t mesh_client_start(void);

#ifdef __cplusplus
}
#endif
