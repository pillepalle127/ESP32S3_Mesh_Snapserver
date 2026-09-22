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

#define DEVICE_CONFIG_VERSION 2

/* device_config_t.role. Applying a change requires a reboot. */
#define DEVICE_ROLE_SERVER 0U
#define DEVICE_ROLE_CLIENT 1U

/*
 * device_config_t.source_mode (client role only). Applies live, no reboot.
 * AUTO gives the local I2S input priority whenever a signal is present,
 * falling back to the network stream otherwise -- the same rule the
 * reference ESP32_Mesh_Snapclient project used for A2DP vs. Snapcast.
 */
#define SOURCE_MODE_AUTO         0U
#define SOURCE_MODE_NETWORK_ONLY 1U
#define SOURCE_MODE_LOCAL_ONLY   2U

/* Range of delay_trim_ms. Also bounds how much headroom the server's local
 * output delay line has to reserve so the trim stays adjustable at runtime. */
#define DEVICE_CONFIG_DELAY_TRIM_MAX_MS 2000

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

    /* Server/client role. Applying a change requires a reboot. */
    uint8_t  role;

    /*
     * Client role only. Applies live, no reboot.
     *
     * buffer_ms is also used by the server as the announced Snapcast
     * bufferMs (see snapserver.c) -- both roles read it from the same
     * field so a server and its clients agree on one end-to-end latency
     * budget without a separate setting.
     */
    uint16_t buffer_ms;
    int16_t  delay_trim_ms;
    uint8_t  source_mode;
    int8_t   local_input_threshold_db;
    /* Empty = auto-discover the server via esp_mesh_lite_get_root_ip(). */
    char     server_host[32];

    uint8_t  reserved[16];
} device_config_t;

/*
 * Potentiometer inputs (see pots.h). Kept out of device_config_t on
 * purpose: that struct is stored as one versioned blob, and a stored blob
 * whose version does not match is replaced by defaults -- adding fields
 * there would have wiped every device's configuration on update. This one
 * lives under its own NVS key and falls back to defaults field by field.
 *
 * A gpio of 0 means "no knob". Pin changes need a reboot, the delay range
 * applies live.
 */
typedef struct {
    uint8_t  volume_gpio;
    uint8_t  delay_gpio;
    uint16_t delay_range_ms;
} device_pots_t;

/* Volume stays on the pin it had while that was a Kconfig setting, so an
 * update changes nothing on boards already wired. Delay starts unset: an
 * open delay input would read an end stop, not the centre. */
#define DEVICE_POTS_DEFAULT_VOLUME_GPIO    10U
#define DEVICE_POTS_DEFAULT_DELAY_GPIO     0U
#define DEVICE_POTS_DEFAULT_DELAY_RANGE_MS 200U
#define DEVICE_POTS_DELAY_RANGE_MIN_MS     10U

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

/*
 * Copies the cached config into *out under a short critical section. Valid
 * only after device_config_load() has returned. Copy-out rather than a
 * pointer to the live struct: the cache can be written from a task other
 * than the caller's (the provisioning grace-window task, or another HTTP
 * request), so a returned pointer could be read mid-write.
 */
void device_config_get(device_config_t *out);

/*
 * True if the most recent device_config_load() found no usable stored
 * config (first-ever boot, post-factory-reset boot, or a version mismatch).
 */
bool device_config_is_first_boot(void);

/* Current potentiometer settings (defaults if none are stored). */
void device_config_get_pots(device_pots_t *out);

/*
 * Validates and stores the potentiometer settings: each pin 0 or one that
 * pots_pin_blocked_reason() accepts, the two pins different unless both
 * are 0, range between DEVICE_POTS_DELAY_RANGE_MIN_MS and
 * DEVICE_CONFIG_DELAY_TRIM_MAX_MS. Returns ESP_ERR_INVALID_ARG otherwise.
 */
esp_err_t device_config_save_pots(const device_pots_t *pots);

/* The check device_config_save_pots() applies, for callers that want to
 * reject a request before saving anything else. */
bool device_config_pots_valid(const device_pots_t *pots);

#ifdef __cplusplus
}
#endif
