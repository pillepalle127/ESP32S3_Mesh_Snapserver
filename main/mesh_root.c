/**
 * @file mesh_root.c
 * @brief Starts either the autonomous ESP-Mesh-Lite root or the
 *        open provisioning fallback AP, decided by provisioning_decide().
 */
#include "mesh_root.h"

#include <string.h>

#include "device_config.h"
#include "esp_bridge.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "provisioning.h"

#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
#include "esp_mesh_lite.h"
#include "esp_mesh_lite_core.h"
#endif

static const char *TAG = "MESH_ROOT";

static void clear_sta_wifi(void)
{
    wifi_config_t sta_config = {0};
    esp_bridge_wifi_set_config(WIFI_IF_STA, &sta_config);
}

static void configure_ap_wifi(const char *ssid, const char *password, uint8_t channel)
{
    wifi_config_t ap_config = {
        .ap = {
            .channel = channel,
            .max_connection = 10,
        },
    };

    strlcpy((char *)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid));
    /*
     * Measure what strlcpy actually wrote, not the source: strlcpy always
     * reserves one byte for the NUL terminator within a 32-byte
     * destination, so a 32-char source (the UI allows up to that) is
     * copied as 31 real characters + '\0' at index 31. Measuring the
     * source would set ssid_len=32 and tell the driver to broadcast that
     * trailing NUL byte as part of the SSID instead of just the 31
     * characters that were actually copied.
     */
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);

    if (password != NULL && strlen(password) >= 8U) {
        strlcpy((char *)ap_config.ap.password, password, sizeof(ap_config.ap.password));
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_bridge_wifi_set_config(WIFI_IF_AP, &ap_config);
}

static esp_err_t pin_ap_ip(void)
{
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    return provisioning_pin_ap_ip(ap_netif);
}

static esp_err_t start_provisioning_ap(provisioning_reason_t reason)
{
    char ssid[33];
    provisioning_build_ssid(ssid, sizeof(ssid));

    ESP_LOGW(TAG,
             "Starting provisioning AP (reason=%d): SSID=%s, open, no mesh",
             (int)reason, ssid);

    esp_bridge_create_all_netif();

    esp_err_t err = pin_ap_ip();
    if (err != ESP_OK) {
        return err;
    }

    clear_sta_wifi();
    configure_ap_wifi(ssid, NULL, 1);

    return provisioning_arm_ap_timeout();
}

#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
static esp_err_t start_mesh(const device_config_t *cfg)
{
    ESP_LOGI(TAG, "Starting autonomous ESP-Mesh-Lite root without external router");

    esp_bridge_create_all_netif();

    esp_err_t err = pin_ap_ip();
    if (err != ESP_OK) {
        return err;
    }

    clear_sta_wifi();
    configure_ap_wifi(cfg->mesh_ssid, cfg->mesh_password, cfg->mesh_channel);

    esp_mesh_lite_config_t config = ESP_MESH_LITE_DEFAULT_INIT();
    config.join_mesh_ignore_router_status = true;
    config.join_mesh_without_configured_wifi = false;
    config.leaf_node = false;
    config.max_level = cfg->mesh_max_level;

    esp_mesh_lite_init(&config);

    err = esp_mesh_lite_set_softap_info(cfg->mesh_ssid, cfg->mesh_password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not set Mesh-Lite SoftAP info: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_mesh_lite_set_allowed_level(1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not restrict node to root level 1: %s",
                 esp_err_to_name(err));
        return err;
    }

    esp_mesh_lite_core_log_enable(false);
    esp_mesh_lite_start();

    ESP_LOGI(TAG,
             "Mesh SoftAP configured: SSID=%s, channel=%d, max_level=%d, password=[HIDDEN]",
             cfg->mesh_ssid, cfg->mesh_channel, cfg->mesh_max_level);
    ESP_LOGI(TAG, "Autonomous root started: SoftAP + DHCP + Mesh-Lite level 1");

    return provisioning_arm_grace_window();
}
#endif

esp_err_t mesh_root_start(void)
{
    device_config_t cfg;
    device_config_get(&cfg);
    provisioning_reason_t reason = provisioning_decide(&cfg);

#if !CONFIG_SNAPSERVER_ENABLE_MESH_LITE
    if (reason == PROVISIONING_REASON_NONE) {
        ESP_LOGW(TAG,
                 "Mesh-Lite not compiled into this firmware, forcing provisioning AP");
        reason = PROVISIONING_REASON_MESH_DISABLED;
    }
#endif

    if (reason != PROVISIONING_REASON_NONE) {
        return start_provisioning_ap(reason);
    }

#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
    return start_mesh(&cfg);
#else
    return ESP_OK; /* unreachable: reason is forced above when unavailable */
#endif
}
