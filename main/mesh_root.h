/**
 * @file mesh_root.h
 * @brief Public startup interface for the autonomous Mesh-Lite root.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts the autonomous Mesh-Lite root. No-op when
 * CONFIG_SNAPSERVER_ENABLE_MESH_LITE is disabled.
 */
esp_err_t mesh_root_start(void);

#ifdef __cplusplus
}
#endif
