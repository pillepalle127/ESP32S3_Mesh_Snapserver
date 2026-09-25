/**
 * @file provisioning.c
 * @brief Mesh vs. provisioning-AP decision, boot-fail streak, AP timers.
 */
#include "provisioning.h"

#include <stdio.h>
#include <string.h>

#include "dhcpserver/dhcpserver.h"
#include "esp_bridge.h"
#include "esp_event.h"
#include "esp_log.h"
#include "status_led.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"

static const char *TAG = "PROVISIONING";

#define PROVISIONING_TASK_STACK 4096
#define PROVISIONING_TASK_PRIORITY 4
#define PROVISIONING_TASK_CORE 0

static TaskHandle_t s_grace_task;
static TaskHandle_t s_ap_timeout_task;
static volatile bool s_grace_cancelled;
static volatile bool s_ap_timeout_cancelled;
static esp_event_handler_instance_t s_grace_success_handler;
static esp_event_base_t s_grace_success_event_base;
static int32_t s_grace_success_event_id;
static volatile bool s_grace_success_seen;
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
    /* Base (STA) MAC, the same digits as the device ID in webconfig.c. */
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        memset(mac, 0, sizeof(mac));
    }
    snprintf(out, out_len, "ESP32_provisioning_%02X%02X%02X",
             mac[3], mac[4], mac[5]);
}

/*
 * The DHCP server keeps its leases in RAM only, so after a restart it hands
 * out .2, .3, ... again from scratch. A station that rode out the restart
 * without asking again -- a phone reassociating quickly keeps its old
 * address -- can then share an address with a newcomer, and one of the two
 * stops getting its packets. So every boot takes the other half of the
 * subnet: an address left over from the previous boot can never be handed
 * out again. The one it keeps is NAKed at its next renewal. Only a device
 * that rides out two restarts in a row without DHCP could still collide.
 *
 * Chosen once per boot and stored right away; a missing key means the
 * previous firmware, which always used the lower half (IDF default).
 */
#define DHCP_HALF_NVS_NAMESPACE "dhcps"
#define DHCP_HALF_NVS_KEY       "half"
#define DHCP_HALF_SIZE          100U  /* DHCPS_MAX_LEASE */

static int s_dhcp_half = -1;

static int dhcp_half_for_this_boot(void)
{
    if (s_dhcp_half >= 0) {
        return s_dhcp_half;
    }
    uint8_t previous = 0U;
    nvs_handle_t handle;
    if (nvs_open(DHCP_HALF_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        (void)nvs_get_u8(handle, DHCP_HALF_NVS_KEY, &previous);
        s_dhcp_half = (previous == 0U) ? 1 : 0;
        if (nvs_set_u8(handle, DHCP_HALF_NVS_KEY, (uint8_t)s_dhcp_half) == ESP_OK) {
            (void)nvs_commit(handle);
        }
        nvs_close(handle);
    } else {
        s_dhcp_half = 1;
    }
    return s_dhcp_half;
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

    /* .2-.101 or .102-.201; set after the IP, which the range is checked against. */
    const uint32_t net = ntohl(ip_info.ip.addr) & 0xFFFFFF00U;
    const uint32_t first = net + 2U + (uint32_t)dhcp_half_for_this_boot() * DHCP_HALF_SIZE;
    dhcps_lease_t pool = { .enable = true };
    pool.start_ip.addr = htonl(first);
    pool.end_ip.addr = htonl(first + DHCP_HALF_SIZE - 1U);
    result = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS,
                                    &pool, sizeof(pool));
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "DHCP range not set, using the default: %s", esp_err_to_name(result));
    }

    result = esp_netif_dhcps_start(ap_netif);
    if (result != ESP_OK && result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "dhcps_start failed: %s", esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(TAG, "AP IP pinned to %s, DHCP range .%u-.%u", PROVISIONING_AP_IP_ADDR,
             (unsigned)(first & 0xFFU), (unsigned)((first + DHCP_HALF_SIZE - 1U) & 0xFFU));
    return ESP_OK;
}

esp_err_t provisioning_pin_own_ap_ip(void)
{
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    return provisioning_pin_ap_ip(ap_netif);
}

esp_err_t provisioning_clear_sta_wifi(void)
{
    wifi_config_t sta_config = {0};
    return esp_bridge_wifi_set_config(WIFI_IF_STA, &sta_config);
}

esp_err_t provisioning_configure_ap_wifi(const char *ssid, const char *password, uint8_t channel)
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

    return esp_bridge_wifi_set_config(WIFI_IF_AP, &ap_config);
}

esp_err_t provisioning_set_ap_idle_timeout(uint16_t seconds)
{
    const esp_err_t err = esp_wifi_set_inactive_time(WIFI_IF_AP, seconds);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not set SoftAP idle timeout: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SoftAP drops silent stations after %u s (was 300)",
             (unsigned)seconds);
    return ESP_OK;
}

esp_err_t provisioning_disable_ap_pmf(void)
{
    wifi_config_t ap_config;

    esp_err_t err = esp_wifi_get_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not read SoftAP config: %s", esp_err_to_name(err));
        return err;
    }

    if (!ap_config.ap.pmf_cfg.capable && !ap_config.ap.pmf_cfg.required) {
        return ESP_OK;
    }

    ap_config.ap.pmf_cfg.capable = false;
    ap_config.ap.pmf_cfg.required = false;

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not disable SoftAP PMF: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SoftAP PMF disabled (mesh stations join without PMF)");
    return ESP_OK;
}

esp_err_t provisioning_start_fallback_ap(provisioning_reason_t reason)
{
    char ssid[33];
    provisioning_build_ssid(ssid, sizeof(ssid));

    ESP_LOGW(TAG,
             "Starting provisioning AP (reason=%d): SSID=%s, open, no mesh",
             (int)reason, ssid);
    status_led_set_state(STATUS_LED_PROVISIONING);

    esp_bridge_create_all_netif();

    esp_err_t err = provisioning_pin_own_ap_ip();
    if (err != ESP_OK) {
        return err;
    }

    err = provisioning_clear_sta_wifi();
    if (err != ESP_OK) {
        return err;
    }

    err = provisioning_configure_ap_wifi(ssid, NULL, 1);
    if (err != ESP_OK) {
        return err;
    }

    return provisioning_arm_ap_timeout();
}

static void on_grace_window_success(void *arg,
                                    esp_event_base_t base,
                                    int32_t id,
                                    void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    s_grace_success_seen = true;
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

    if (s_grace_success_handler != NULL) {
        esp_event_handler_instance_unregister(
            s_grace_success_event_base, s_grace_success_event_id, s_grace_success_handler);
        s_grace_success_handler = NULL;
    }

    if (!s_grace_cancelled) {
        if (s_grace_success_seen) {
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

esp_err_t provisioning_arm_grace_window(esp_event_base_t success_event_base,
                                        int32_t success_event_id)
{
    s_grace_success_seen = false;
    s_grace_cancelled = false;
    s_grace_success_event_base = success_event_base;
    s_grace_success_event_id = success_event_id;

    esp_err_t result = esp_event_handler_instance_register(
        success_event_base,
        success_event_id,
        &on_grace_window_success,
        NULL,
        &s_grace_success_handler);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Registering grace-window success handler failed: %s",
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
