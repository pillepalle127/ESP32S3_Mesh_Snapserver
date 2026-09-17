/**
 * @file mesh_client.c
 * @brief Joins the mesh as a non-root relay and starts the Snapcast client
 *        once a parent hands out an IP address.
 */
#include "mesh_client.h"

#include <string.h>

#include "device_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "provisioning.h"
#include "snapclient.h"
#include "snapserver.h"

#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
#include "esp_bridge.h"
#include "esp_mesh_lite.h"
#include "esp_mesh_lite_core.h"
#endif

static const char *TAG = "MESH_CLIENT";

#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE

static bool s_snapclient_started;

static void build_relay_ssid(const char *mesh_ssid, char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        memset(mac, 0, sizeof(mac));
    }
    /*
     * Just the broadcast SSID shown to a Wi-Fi scan for identifying this
     * physical relay; the "_XXXXXX" MAC suffix is 7 bytes, so the mesh_ssid
     * portion is capped to leave room for it within out_len (33 bytes here).
     * Mesh-Lite's own parent matching uses the untruncated cfg->mesh_ssid
     * via esp_mesh_lite_set_softap_info() below, not this display string.
     */
    snprintf(out, out_len, "%.25s_%02X%02X%02X", mesh_ssid, mac[3], mac[4], mac[5]);
}

/*
 * Resolves which Snapserver to connect to: an explicit override from
 * device_config.server_host if set, otherwise ESP-Mesh-Lite's own root-IP
 * tracking, which stays correct across parent changes in a dynamic,
 * multi-hop mesh (unlike the local DHCP gateway, which is only the
 * immediate parent beyond the first hop, and unlike mDNS, whose link-local
 * multicast doesn't cross the NAPT boundary between levels).
 */
static void resolve_server_host(char *out, size_t out_len)
{
    device_config_t cfg;
    device_config_get(&cfg);

    if (cfg.server_host[0] != '\0') {
        strlcpy(out, cfg.server_host, out_len);
        return;
    }

    esp_ip_addr_t root_ip;
    if (esp_mesh_lite_get_root_ip(ESP_IPADDR_TYPE_V4, &root_ip) == ESP_OK) {
        esp_ip4addr_ntoa(&root_ip.u_addr.ip4, out, (int)out_len);
        return;
    }

    ESP_LOGW(TAG, "Root IP unavailable, falling back to %s", PROVISIONING_AP_IP_ADDR);
    strlcpy(out, PROVISIONING_AP_IP_ADDR, out_len);
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    char host[IP4ADDR_STRLEN_MAX];
    resolve_server_host(host, sizeof(host));

    ESP_LOGI(TAG, "Got mesh IP, Snapserver resolved to %s:%u", host, (unsigned)SNAPSERVER_PORT);
    snapclient_set_network_available(true);

    if (!s_snapclient_started) {
        s_snapclient_started = true;
        const esp_err_t result = snapclient_start(host, SNAPSERVER_PORT);
        if (result != ESP_OK) {
            s_snapclient_started = false;
            ESP_LOGE(TAG, "Starting snapclient failed: %s", esp_err_to_name(result));
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Lost parent, aborting any open Snapserver connection");
        snapclient_set_network_available(false);
    }
}

static esp_err_t start_client_mesh(const device_config_t *cfg)
{
    ESP_LOGI(TAG, "Joining mesh as non-root relay");

    esp_bridge_create_all_netif();

    esp_err_t err = provisioning_clear_sta_wifi();
    if (err != ESP_OK) {
        return err;
    }

    char relay_ssid[33];
    build_relay_ssid(cfg->mesh_ssid, relay_ssid, sizeof(relay_ssid));

    /*
     * Channel 1 here is a placeholder: once this node's STA side associates
     * with a parent, esp_wifi keeps AP and STA on the same radio channel
     * automatically (single-radio concurrent AP+STA), so whatever the
     * parent actually uses wins regardless of what's configured up front.
     */
    err = provisioning_configure_ap_wifi(relay_ssid, cfg->mesh_password, 1);
    if (err != ESP_OK) {
        return err;
    }

    esp_mesh_lite_config_t config = ESP_MESH_LITE_DEFAULT_INIT();
    config.join_mesh_ignore_router_status = true;
    config.join_mesh_without_configured_wifi = true;
    config.leaf_node = false;
    config.max_level = cfg->mesh_max_level;

    esp_mesh_lite_init(&config);

    err = esp_mesh_lite_set_softap_info(cfg->mesh_ssid, cfg->mesh_password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not set Mesh-Lite SoftAP info: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_mesh_lite_set_disallowed_level(1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not disallow root level: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Tuned for a tube-shaped, highly dynamic mesh with frequent
     * rearrangements: retry the current parent quickly a few times (2 s
     * apart, up to 3 tries), then fall back to a full rescan every 5 s
     * rather than the (slower) library defaults of 5 s / 2 tries / 10 s.
     */
    esp_mesh_lite_set_wifi_reconnect_interval(2, 3, 5);

    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                              &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Registering Wi-Fi event handler failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &ip_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Registering IP event handler failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_mesh_lite_core_log_enable(false);
    esp_mesh_lite_connect();
    esp_mesh_lite_start();

    ESP_LOGI(TAG, "Mesh client started: relay SSID=%s, mesh identity=%s",
             relay_ssid, cfg->mesh_ssid);

    return provisioning_arm_grace_window(IP_EVENT, IP_EVENT_STA_GOT_IP);
}
#endif

esp_err_t mesh_client_start(void)
{
    device_config_t cfg;
    device_config_get(&cfg);
    provisioning_reason_t reason = provisioning_decide(&cfg);

#if !CONFIG_SNAPSERVER_ENABLE_MESH_LITE
    if (reason == PROVISIONING_REASON_NONE) {
        ESP_LOGW(TAG, "Mesh-Lite not compiled into this firmware, forcing provisioning AP");
        reason = PROVISIONING_REASON_MESH_DISABLED;
    }
#endif

    if (reason != PROVISIONING_REASON_NONE) {
        return provisioning_start_fallback_ap(reason);
    }

#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
    return start_client_mesh(&cfg);
#else
    return ESP_OK; /* unreachable: reason is forced above when unavailable */
#endif
}
