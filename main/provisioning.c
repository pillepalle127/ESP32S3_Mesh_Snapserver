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
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

static const char *TAG = "PROVISIONING";

#define PROVISIONING_TASK_STACK 4096
#define PROVISIONING_TASK_PRIORITY 4
#define PROVISIONING_TASK_CORE 0

static TaskHandle_t s_grace_task;
static TaskHandle_t s_ap_timeout_task;
static volatile bool s_grace_cancelled;
static volatile bool s_ap_timeout_cancelled;
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

/*
 * Both timed actions below used to be esp_timer callbacks. That task's
 * stack (CONFIG_ESP_TIMER_TASK_STACK_SIZE, 3584 bytes in this sdkconfig) is
 * too tight for what they actually do -- an NVS blob write here, plus
 * esp_wifi_stop() in the AP-timeout task below -- so each is instead a
 * one-shot FreeRTOS task with its own generous stack, created once when
 * armed and self-deleting when done. Cancellation (see provisioning_
 * cancel_ap_timeout()/_grace_window()) just sets a flag the task checks
 * after waking from its vTaskDelay, rather than tearing the task down from
 * outside: every caller of those cancel functions reboots the device a
 * moment later anyway, so letting the task's own delay run out is simpler
 * and avoids deleting a task that might be mid-way through an NVS write.
 */

static void grace_window_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(PROVISIONING_GRACE_WINDOW_US / 1000LL));

    if (s_sta_connect_handler != NULL) {
        esp_event_handler_instance_unregister(
            WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, s_sta_connect_handler);
        s_sta_connect_handler = NULL;
    }

    if (!s_grace_cancelled) {
        if (s_sta_connected_in_window) {
            ESP_LOGI(TAG, "Station joined within grace window, resetting boot-fail streak");
            device_config_set_boot_fail_count(0);
        } else {
            device_config_t cfg;
            device_config_get(&cfg);
            const uint8_t next = cfg.boot_fail_count + 1U;
            ESP_LOGW(TAG,
                     "No station joined within grace window, boot-fail streak now %u",
                     (unsigned)next);
            device_config_set_boot_fail_count(next);
        }
    }

    s_grace_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t provisioning_arm_grace_window(void)
{
    s_sta_connected_in_window = false;
    s_grace_cancelled = false;

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

    if (xTaskCreatePinnedToCore(grace_window_task,
                                "prov_grace",
                                PROVISIONING_TASK_STACK,
                                NULL,
                                PROVISIONING_TASK_PRIORITY,
                                &s_grace_task,
                                PROVISIONING_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Creating grace-window task failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void ap_timeout_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(PROVISIONING_AP_TIMEOUT_US / 1000LL));

    if (!s_ap_timeout_cancelled) {
        ESP_LOGW(TAG,
                 "Provisioning AP window closed after 3 minutes -- radio is off, "
                 "power-cycle the device to open a new window");
        const esp_err_t result = esp_wifi_stop();
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(result));
        }
    }

    s_ap_timeout_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t provisioning_arm_ap_timeout(void)
{
    s_ap_timeout_cancelled = false;

    if (xTaskCreatePinnedToCore(ap_timeout_task,
                                "prov_ap_timeout",
                                PROVISIONING_TASK_STACK,
                                NULL,
                                PROVISIONING_TASK_PRIORITY,
                                &s_ap_timeout_task,
                                PROVISIONING_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Creating AP-timeout task failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void provisioning_cancel_ap_timeout(void)
{
    s_ap_timeout_cancelled = true;
}

void provisioning_cancel_grace_window(void)
{
    s_grace_cancelled = true;
}
