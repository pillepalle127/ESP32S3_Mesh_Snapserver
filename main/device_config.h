/**
 * @file device_config.h
 * @brief NVS-backed runtime configuration (mesh, DSP, Opus).
 *
 * Every field here used to be a Kconfig compile-time constant. The Kconfig
 * values now only seed the defaults used on first boot, after a factory
 * reset, or when a stored blob fails its version check; from then on the
 * values in NVS are authoritative and are edited via the web config page
 * (see webconfig.c).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_CONFIG_VERSION 1

typedef struct {
    uint32_t version;

    /* Mesh / Wi-Fi. Applying a change requires a reboot. */
    bool     mesh_enable;
    char     mesh_ssid[33];
    char     mesh_password[65];
    uint8_t  mesh_channel;
    uint8_t  mesh_max_level;

    /* DSP / crossover. Applies live, no reboot. */
    bool     dsp_bypass;
    uint16_t crossover_hz;
    float    sub_gain_db;
    float    wideband_gain_db;
    uint8_t  sub_channel;
    uint8_t  wideband_channel;

    /* Opus. Applies live, no reboot. */
    uint32_t opus_bitrate;
    uint8_t  opus_complexity;

    /* Provisioning bookkeeping, not user-editable via the config page. */
    uint8_t  boot_fail_count;

    uint8_t  reserved[32];
} device_config_t;

/*
 * Loads the config from NVS into the in-RAM cache. If no config is stored
 * yet, or the stored blob's version doesn't match DEVICE_CONFIG_VERSION,
 * seeds Kconfig-derived defaults and persists them, and records that this
 * was a first-boot-equivalent load (see device_config_is_first_boot()).
 * Must be called once, before any other device_config_*() call.
 */
esp_err_t device_config_load(void);

/*
 * Validates every field, writes it to NVS, and updates the in-RAM cache.
 * Always resets boot_fail_count to 0: a deliberate save is a fresh
 * reconfiguration attempt and should not be held back by a streak that led
 * up to it. Use device_config_set_boot_fail_count() for the provisioning
 * module's own bookkeeping instead, which does not have this side effect.
 */
esp_err_t device_config_save(const device_config_t *cfg);

/*
 * Persists only the boot-fail counter, bypassing device_config_save()'s
 * reset-to-0 side effect and full field validation. Used exclusively by
 * provisioning.c to record consecutive mesh-join failures/successes.
 */
esp_err_t device_config_set_boot_fail_count(uint8_t count);

/*
 * Erases the stored config so the next boot's device_config_load() takes the
 * first-boot path again. Does not reboot; the caller does that.
 */
esp_err_t device_config_factory_reset(void);

/* Cached accessor; valid only after device_config_load() has returned. */
const device_config_t *device_config_get(void);

/*
 * True if the most recent device_config_load() found no usable stored
 * config (first-ever boot, post-factory-reset boot, or a version mismatch).
 */
bool device_config_is_first_boot(void);

#ifdef __cplusplus
}
#endif
