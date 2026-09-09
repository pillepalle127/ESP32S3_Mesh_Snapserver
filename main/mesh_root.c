/**
 * @file mesh_root.c
 * @brief Starts an autonomous ESP-Mesh-Lite root with SoftAP and DHCP.
 */
#include "mesh_root.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"

#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
#include "esp_bridge.h"
#include "esp_mesh_lite.h"
#include "esp_mesh_lite_core.h"
#endif

static const char *TAG = "MESH_ROOT";

#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
static void configure_routerless_wifi(void)
{
    wifi_config_t sta_config = {0};
    esp_bridge_wifi_set_config(WIFI_IF_STA, &sta_config);

    wifi_config_t ap_config = {
        .ap = {
            .password = CONFIG_MESH_SOFTAP_PASSWORD,
            .channel = CONFIG_MESH_CHANNEL,
            .max_connection = 10,
        },
    };

    strlcpy((char *)ap_config.ap.ssid,
            CONFIG_MESH_SOFTAP_SSID_PREFIX,
            sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    ap_config.ap.authmode = strlen(CONFIG_MESH_SOFTAP_PASSWORD) >= 8U
                                ? WIFI_AUTH_WPA2_PSK
                                : WIFI_AUTH_OPEN;

    esp_bridge_wifi_set_config(WIFI_IF_AP, &ap_config);
}

static esp_err_t configure_mesh_softap_info(void)
{
    const esp_err_t err = esp_mesh_lite_set_softap_info(
        CONFIG_MESH_SOFTAP_SSID_PREFIX,
        CONFIG_MESH_SOFTAP_PASSWORD);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not set Mesh-Lite SoftAP info: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "Mesh SoftAP configured: SSID=%s, channel=%d, password=[HIDDEN]",
             CONFIG_MESH_SOFTAP_SSID_PREFIX,
             CONFIG_MESH_CHANNEL);
    return ESP_OK;
}
#endif

esp_err_t mesh_root_start(void)
{
#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
    ESP_LOGI(TAG, "Starting autonomous ESP-Mesh-Lite root without external router");

    esp_bridge_create_all_netif();
    configure_routerless_wifi();

    esp_mesh_lite_config_t config = ESP_MESH_LITE_DEFAULT_INIT();
    config.join_mesh_ignore_router_status = true;
    config.join_mesh_without_configured_wifi = false;
    config.leaf_node = false;

    esp_mesh_lite_init(&config);

    esp_err_t err = configure_mesh_softap_info();
    if (err != ESP_OK) {
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
             "Autonomous root started: SoftAP + DHCP + Mesh-Lite level 1");
#else
    ESP_LOGI(TAG,
             "Mesh-Lite disabled: use ordinary Wi-Fi/LAN for protocol test");
#endif

    return ESP_OK;
}
