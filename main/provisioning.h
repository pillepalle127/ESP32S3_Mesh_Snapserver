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
#include <stdint.h>

#include "device_config.h"
#include "esp_err.h"
#include "esp_event_base.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Consecutive mesh boots without any station joining before falling back. */
#define PROVISIONING_BOOT_FAIL_THRESHOLD 7

/* How long a normal mesh boot waits for a station join before it counts
 * that boot as failed. */
#define PROVISIONING_GRACE_WINDOW_US (60LL * 1000000LL)

/* How long the open provisioning AP accepts connections before the radio
 * shuts off entirely, regardless of which reason brought it up. There is no
 * reboot and no fallback to a protected AP: only a power cycle re-opens a
 * window. */
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

/* Convenience wrapper around provisioning_pin_ap_ip() that looks up this
 * device's own "WIFI_AP_DEF" netif itself. Used by both mesh_root.c (the
 * server's mesh AP) and mesh_client.c (the provisioning fallback only --
 * the client's mesh-relay AP is left unpinned, its address is assigned by
 * ESP-Mesh-Lite/NAPT per level). */
esp_err_t provisioning_pin_own_ap_ip(void);

/* Clears any STA Wi-Fi credentials from the driver. Both roles start from a
 * clean STA config before setting up their own AP: the server never uses
 * STA at all, and the client's STA side is driven entirely by ESP-Mesh-Lite's
 * own parent search rather than a fixed SSID/password. */
esp_err_t provisioning_clear_sta_wifi(void);

/* Configures this device's own SoftAP (SSID/password/channel). password ==
 * NULL or shorter than 8 characters falls back to WIFI_AUTH_OPEN. Shared by
 * the provisioning AP, the server's mesh-root AP and the client's mesh-relay
 * AP -- all three are just a physical SoftAP with different SSID/password. */
esp_err_t provisioning_configure_ap_wifi(const char *ssid, const char *password, uint8_t channel);

/* Full provisioning-AP fallback flow: builds the SSID, brings up netifs,
 * pins the AP IP, clears STA config, configures the open AP and arms the
 * 3-minute timeout. Shared by mesh_root.c and mesh_client.c -- both call
 * this identically whenever provisioning_decide() returns a reason other
 * than PROVISIONING_REASON_NONE. */
esp_err_t provisioning_start_fallback_ap(provisioning_reason_t reason);

/* Arms the post-boot join grace window. Call only for a PROVISIONING_REASON_
 * NONE boot, once the mesh AP's Wi-Fi driver is up. On expiry, resets or
 * increments the persisted boot-fail counter depending on whether any
 * station joined in the meantime.
 *
 * success_event_base/success_event_id name the event that counts as "a
 * station successfully joined this boot": the server passes
 * (WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED) since it waits for a child to
 * join its own AP, the client passes (IP_EVENT, IP_EVENT_STA_GOT_IP) since
 * it waits for itself to join a parent. */
esp_err_t provisioning_arm_grace_window(esp_event_base_t success_event_base,
                                        int32_t success_event_id);

/* Arms the provisioning AP's unconditional 3-minute timeout. Call once the
 * provisioning AP is up, for any reason. On expiry, stops the Wi-Fi radio
 * (esp_wifi_stop()) -- it does not reboot and does not fall back to any
 * other AP. The device stays unreachable over Wi-Fi until either a power
 * cycle re-opens a provisioning window, or a save made in time (see
 * webconfig.c) reboots into the normal mesh-vs-provisioning decision. */
esp_err_t provisioning_arm_ap_timeout(void);

/* Cancels a pending AP timeout, e.g. right before a Save-triggered reboot
 * supersedes it. Safe to call even if no timeout is armed. Does not stop
 * the underlying task early -- it just skips its action when it wakes up,
 * which is fine since every caller reboots shortly after cancelling. */
void provisioning_cancel_ap_timeout(void);

/* Cancels a pending grace-window callback, e.g. right before a factory
 * reset that must not have its NVS erase silently undone by the grace
 * window writing device_config back a moment later. Safe to call even if
 * no grace window is armed. Same "skips on wake, doesn't stop early"
 * semantics as provisioning_cancel_ap_timeout(). */
void provisioning_cancel_grace_window(void);

#ifdef __cplusplus
}
#endif
