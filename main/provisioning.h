/**
 * @file provisioning.h
 * @brief Decides mesh vs. provisioning-AP boot mode and owns the timers that
 *        drive that decision (post-boot join grace window, AP timeout).
 *
 * The device always boots into exactly one of two AP modes: its configured
 * mesh SoftAP, or an open, unprotected "ESP32_provisioning_XXXXXX" fallback
 * AP used to reach the config page. provisioning_decide() answers which one,
 * from the already-loaded device_config_t; mesh_root.c acts on the answer.
 */
#pragma once

#include <stddef.h>

#include "device_config.h"
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Consecutive mesh boots without any station joining before falling back. */
#define PROVISIONING_BOOT_FAIL_THRESHOLD 7

/* How long a normal mesh boot waits for a station join before it counts
 * that boot as failed. */
#define PROVISIONING_GRACE_WINDOW_US (60LL * 1000000LL)

/* How long the provisioning AP stays up before the device reboots and
 * re-decides, regardless of which reason brought it up. */
#define PROVISIONING_AP_TIMEOUT_US (180LL * 1000000LL)

#define PROVISIONING_AP_IP_ADDR "192.168.5.1"

typedef enum {
    PROVISIONING_REASON_NONE = 0,       /* normal mesh boot */
    PROVISIONING_REASON_NO_CONFIG,      /* first boot or just factory-reset */
    PROVISIONING_REASON_BOOT_FAIL_STREAK,
    PROVISIONING_REASON_MESH_DISABLED,
} provisioning_reason_t;

/*
 * Which AP mode this boot should use. Also caches the result for
 * provisioning_get_active_reason(), since a later device_config_save() (e.g.
 * DSP-only, or mesh settings not yet rebooted into) must not change what
 * webconfig.c reports as the currently running mode.
 */
provisioning_reason_t provisioning_decide(const device_config_t *cfg);

/* The reason cached by the most recent provisioning_decide() call, i.e.
 * the AP mode this boot is actually running, not a live recomputation. */
provisioning_reason_t provisioning_get_active_reason(void);

/* Builds "ESP32_provisioning_XXXXXX" from the device's own SoftAP MAC. */
void provisioning_build_ssid(char *out, size_t out_len);

/* Pins the given AP netif to PROVISIONING_AP_IP_ADDR, restarting its DHCP
 * server so clients get an address in that range. Used for both the
 * provisioning AP and the normal mesh AP, so the config page is always
 * reachable at the same address. */
esp_err_t provisioning_pin_ap_ip(esp_netif_t *ap_netif);

/* Arms the post-boot join grace window. Call only for a PROVISIONING_REASON_
 * NONE boot, once the mesh AP's Wi-Fi driver is up. On expiry, resets or
 * increments the persisted boot-fail counter depending on whether any
 * station joined in the meantime. */
esp_err_t provisioning_arm_grace_window(void);

/* Arms the provisioning AP's unconditional 3-minute timeout. Call once the
 * provisioning AP is up, for any reason. On expiry, reboots the device. */
esp_err_t provisioning_arm_ap_timeout(void);

/* Cancels a pending AP timeout, e.g. right before a Save-triggered reboot
 * supersedes it. Safe to call even if no timeout is armed. */
void provisioning_cancel_ap_timeout(void);

#ifdef __cplusplus
}
#endif
