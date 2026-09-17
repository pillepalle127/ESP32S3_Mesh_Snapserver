/**
 * @file provisioning.c
 * @brief Mesh vs. provisioning-AP decision, boot-fail streak, AP timers.
 */
#include "provisioning.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"

static const char *TAG = "PROVISIONING";

static esp_timer_handle_t s_grace_timer;
static esp_timer_handle_t s_ap_timeout_timer;
static esp_event_handler_instance_t s_sta_connect_handler;
static volatile bool s_sta_connected_in_window;
static provisioning_reason_t s_active_reason = PROVISIONING_REASON_NONE;

provisioning_reason_t provisioning_decide(const device_config_t *cfg)
{
    provisioning_reason_t reason;

    if (device_config_is_first_boot()) {
        reason = PROVISIONING_REASON_NO_CONFIG;
    } else if (!cfg->mesh_enable) {
        reason = PROVISIONING_REASON_MESH_DISABLED;
    } else if (cfg->boot_fail_count >= PROVISIONING_BOOT_FAIL_THRESHOLD) {
        reason = PROVISIONING_REASON_BOOT_FAIL_STREAK;
    } else {
        reason = PROVISIONING_REASON_NONE;
    }

    s_active_reason = reason;
    return reason;
}

provisioning_reason_t provisioning_get_active_reason(void)
{
    return s_active_reason;
}

void provisioning_build_ssid(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        memset(mac, 0, sizeof(mac));
    }
    snprintf(out, out_len, "ESP32_provisioning_%02X%02X%02X",
             mac[3], mac[4], mac[5]);
}

esp_err_t provisioning_pin_ap_ip(esp_netif_t *ap_netif)
{
    if (ap_netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_ip_info_t ip_info = {0};
    ip_info.ip.addr = ipaddr_addr(PROVISIONING_AP_IP_ADDR);
    ip_info.gw.addr = ipaddr_addr(PROVISIONING_AP_IP_ADDR);
    ip_info.netmask.addr = ipaddr_addr("255.255.255.0");

    esp_err_t result = esp_netif_dhcps_stop(ap_netif);
    if (result != ESP_OK && result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcps_stop failed: %s", esp_err_to_name(result));
    }

    result = esp_netif_set_ip_info(ap_netif, &ip_info);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "set_ip_info failed: %s", esp_err_to_name(result));
        return result;
    }

    result = esp_netif_dhcps_start(ap_netif);
    if (result != ESP_OK && result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "dhcps_start failed: %s", esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(TAG, "AP IP pinned to %s", PROVISIONING_AP_IP_ADDR);
    return ESP_OK;
}

static void on_ap_sta_connected(void *arg,
                                esp_event_base_t base,
                                int32_t id,
                                void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    s_sta_connected_in_window = true;
}

static void on_grace_window_expired(void *arg)
{
    (void)arg;

    if (s_sta_connect_handler != NULL) {
        esp_event_handler_instance_unregister(
            WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, s_sta_connect_handler);
        s_sta_connect_handler = NULL;
    }

    if (s_sta_connected_in_window) {
        ESP_LOGI(TAG, "Station joined within grace window, resetting boot-fail streak");
        device_config_set_boot_fail_count(0);
    } else {
        const uint8_t next = device_config_get()->boot_fail_count + 1U;
        ESP_LOGW(TAG,
                 "No station joined within grace window, boot-fail streak now %u",
                 (unsigned)next);
        device_config_set_boot_fail_count(next);
    }
}

esp_err_t provisioning_arm_grace_window(void)
{
    s_sta_connected_in_window = false;

    esp_err_t result = esp_event_handler_instance_register(
        WIFI_EVENT,
        WIFI_EVENT_AP_STACONNECTED,
        &on_ap_sta_connected,
        NULL,
        &s_sta_connect_handler);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Registering AP-STA-connected handler failed: %s",
                 esp_err_to_name(result));
        return result;
    }

    if (s_grace_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = &on_grace_window_expired,
            .name = "prov_grace",
        };
        result = esp_timer_create(&args, &s_grace_timer);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Creating grace-window timer failed: %s",
                     esp_err_to_name(result));
            return result;
        }
    }

    return esp_timer_start_once(s_grace_timer, PROVISIONING_GRACE_WINDOW_US);
}

static void on_ap_timeout_expired(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "Provisioning AP timed out after 3 minutes, rebooting");
    esp_restart();
}

esp_err_t provisioning_arm_ap_timeout(void)
{
    if (s_ap_timeout_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = &on_ap_timeout_expired,
            .name = "prov_ap_timeout",
        };
        const esp_err_t result = esp_timer_create(&args, &s_ap_timeout_timer);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Creating AP-timeout timer failed: %s",
                     esp_err_to_name(result));
            return result;
        }
    }

    return esp_timer_start_once(s_ap_timeout_timer, PROVISIONING_AP_TIMEOUT_US);
}

void provisioning_cancel_ap_timeout(void)
{
    if (s_ap_timeout_timer != NULL) {
        esp_timer_stop(s_ap_timeout_timer);
    }
}
