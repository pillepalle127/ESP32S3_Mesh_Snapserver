/**
 * @file mesh_root.h
 * @brief Public startup interface for the autonomous Mesh-Lite root.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mesh_root_start(void);

#ifdef __cplusplus
}
#endif
