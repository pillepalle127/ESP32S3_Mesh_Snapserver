/**
 * @file server_probe.c
 * @brief Is another server already running? See server_probe.h.
 */
#include "server_probe.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "provisioning.h"

static const char *TAG = "SERVER_PROBE";

/* Scan of every channel, association and DHCP. Without a mesh in range
 * the connect gives up by itself after the scan, well before this. */
#define CONNECT_TIMEOUT_MS 10000
/* A server answers within tens of ms, even a few hops down (rtt 18-34 ms
 * seen); 192.168.5.1 through a client without a server never answers. */
#define HTTP_TIMEOUT_MS    1000
#define STATUS_BODY_MAX    2048
/* Handover: the old server restarts 1.5 s after handing over, so it is
 * normally gone by the first check. */
#define HANDOVER_WAIT_MS   30000
#define HANDOVER_RETRY_MS  3000

#define BIT_GOT_IP BIT0
#define BIT_FAILED BIT1

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)data;
    EventGroupHandle_t bits = (EventGroupHandle_t)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(bits, BIT_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(bits, BIT_GOT_IP);
    }
}

/* GET /api/status at the server address. True if a server answered. */
static bool ask_server(char *server_id, size_t server_id_len)
{
    const esp_http_client_config_t config = {
        .url = "http://" PROVISIONING_AP_IP_ADDR "/api/status",
        .timeout_ms = HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return false;
    }

    bool server = false;
    char *body = NULL;
    if (esp_http_client_open(client, 0) == ESP_OK &&
        esp_http_client_fetch_headers(client) >= 0 &&
        esp_http_client_get_status_code(client) == 200 &&
        (body = malloc(STATUS_BODY_MAX + 1)) != NULL) {
        int total = 0;
        int got;
        while (total < STATUS_BODY_MAX &&
               (got = esp_http_client_read(client, body + total, STATUS_BODY_MAX - total)) > 0) {
            total += got;
        }
        body[total] = '\0';

        cJSON *root = cJSON_Parse(body);
        const cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "device_id");
        /* Only a server's AP has this address; "role" is there to be sure. */
        server = cJSON_IsObject(root) &&
                 !(cJSON_IsNumber(role) && role->valueint != (int)DEVICE_ROLE_SERVER);
        if (server && cJSON_IsString(id)) {
            strlcpy(server_id, id->valuestring, server_id_len);
        }
        cJSON_Delete(root);
    }
    free(body);
    esp_http_client_cleanup(client);
    return server;
}

/* One connect to the mesh SSID and one question. True if a server answered. */
static bool probe_once(const device_config_t *cfg, char *server_id, size_t server_id_len)
{
    server_id[0] = '\0';

    EventGroupHandle_t bits = xEventGroupCreate();
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (bits == NULL || netif == NULL || esp_wifi_init(&init) != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi unavailable for the check, assuming no server");
        if (netif != NULL) {
            esp_netif_destroy_default_wifi(netif);
        }
        if (bits != NULL) {
            vEventGroupDelete(bits);
        }
        return false;
    }

    esp_event_handler_instance_t wifi_handler = NULL;
    esp_event_handler_instance_t ip_handler = NULL;
    (void)esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                              &event_handler, bits, &wifi_handler);
    (void)esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &event_handler, bits, &ip_handler);

    wifi_config_t sta = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };
    strlcpy((char *)sta.sta.ssid, cfg->mesh_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, cfg->mesh_password, sizeof(sta.sta.password));
    sta.sta.threshold.authmode =
        (strlen(cfg->mesh_password) >= 8U) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    bool server = false;
    /* RAM only: the bridge and Mesh-Lite keep their own Wi-Fi settings. */
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) == ESP_OK &&
        esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK &&
        esp_wifi_set_config(WIFI_IF_STA, &sta) == ESP_OK &&
        esp_wifi_start() == ESP_OK &&
        esp_wifi_connect() == ESP_OK) {
        const EventBits_t got = xEventGroupWaitBits(bits, BIT_GOT_IP | BIT_FAILED, pdFALSE,
                                                    pdFALSE, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
        if ((got & BIT_GOT_IP) != 0U) {
            server = ask_server(server_id, server_id_len);
            ESP_LOGI(TAG, "Mesh found, %s", server ? "a server answers" : "no server answers");
        } else {
            ESP_LOGI(TAG, "No mesh \"%s\" in range", cfg->mesh_ssid);
        }
    }

    (void)esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                wifi_handler);
    (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler);
    (void)esp_wifi_disconnect();
    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();
    esp_netif_destroy_default_wifi(netif);
    vEventGroupDelete(bits);
    return server;
}

uint8_t server_probe_decide_role(const device_config_t *cfg, char *server_id, size_t server_id_len)
{
    if (!device_config_take_handover()) {
        if (probe_once(cfg, server_id, server_id_len)) {
            ESP_LOGW(TAG, "Server %s already runs: starting as its client, still set to server",
                     server_id);
            return DEVICE_ROLE_CLIENT;
        }
        return DEVICE_ROLE_SERVER;
    }

    ESP_LOGI(TAG, "Taking over as server, waiting for the old one to go");
    const int64_t deadline_us = esp_timer_get_time() + HANDOVER_WAIT_MS * 1000LL;
    for (;;) {
        if (!probe_once(cfg, server_id, server_id_len)) {
            return DEVICE_ROLE_SERVER;
        }
        if (esp_timer_get_time() >= deadline_us) {
            ESP_LOGW(TAG, "Server %s still runs after %d s: starting as its client",
                     server_id, HANDOVER_WAIT_MS / 1000);
            return DEVICE_ROLE_CLIENT;
        }
        vTaskDelay(pdMS_TO_TICKS(HANDOVER_RETRY_MS));
    }
}
