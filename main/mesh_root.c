/**
 * @file mesh_root.c
 * @brief Starts either the autonomous ESP-Mesh-Lite root or the
 *        open provisioning fallback AP, decided by provisioning_decide().
 */
#include "mesh_root.h"

#include "device_config.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "provisioning.h"

#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
#include "esp_bridge.h"
#include "esp_mesh_lite.h"
#include "esp_mesh_lite_core.h"
#endif

/* Seconds a silent station may stay associated; see
 * provisioning_set_ap_idle_timeout(). */
#define AP_IDLE_TIMEOUT_S 30

static const char *TAG = "MESH_ROOT";

#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
static esp_err_t start_mesh(const device_config_t *cfg)
{
    ESP_LOGI(TAG, "Starting autonomous ESP-Mesh-Lite root without external router");

    esp_bridge_create_all_netif();

    esp_err_t err = provisioning_pin_own_ap_ip();
    if (err != ESP_OK) {
        return err;
    }

    err = provisioning_clear_sta_wifi();
    if (err != ESP_OK) {
        return err;
    }

    err = provisioning_configure_ap_wifi(cfg->mesh_ssid, cfg->mesh_password, cfg->mesh_channel);
    if (err != ESP_OK) {
        return err;
    }

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

    /* Same reasoning as in mesh_client.c: power save costs latency and
     * reliability on a node that streams continuously, and saves nothing on
     * a mains-powered speaker. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_mesh_lite_core_log_enable(false);
    esp_mesh_lite_start();

    /* After the start: Mesh-Lite reconfigures the SoftAP, so this has to
     * undo its PMF setting rather than pre-empt it. See the header. */
    (void)provisioning_disable_ap_pmf();
    (void)provisioning_set_ap_idle_timeout(AP_IDLE_TIMEOUT_S);

    ESP_LOGI(TAG,
             "Mesh SoftAP configured: SSID=%s, channel=%d, max_level=%d, password=[HIDDEN]",
             cfg->mesh_ssid, cfg->mesh_channel, cfg->mesh_max_level);
    ESP_LOGI(TAG, "Autonomous root started: SoftAP + DHCP + Mesh-Lite level 1");

    return provisioning_arm_grace_window(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED);
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
        return provisioning_start_fallback_ap(reason);
    }

#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
    return start_mesh(&cfg);
#else
    return ESP_OK; /* unreachable: reason is forced above when unavailable */
#endif
}
