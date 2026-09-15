/**
 * @file app_main.c
 * @brief Initializes NVS, networking, Mesh-Lite, audio, streaming and control services.
 */
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "snapcontrol.h"

#include "audio_opus.h"
#include "mesh_root.h"
#include "snapserver.h"

static const char *TAG = "APP";

/*
 * Initializes NVS, erasing and re-initializing the partition if it is full
 * or was written by an incompatible firmware version.
 */
static esp_err_t initialize_nvs(void)
{
    esp_err_t result = nvs_flash_init();

    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(
            TAG,
            "NVS partition must be erased: %s",
            esp_err_to_name(result));

        result = nvs_flash_erase();
        if (result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "NVS erase failed: %s",
                esp_err_to_name(result));
            return result;
        }

        result = nvs_flash_init();
    }

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "NVS initialization failed: %s",
            esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(TAG, "NVS initialized");
    return ESP_OK;
}

/*
 * Initializes the TCP/IP stack and the default event loop. Tolerates an
 * already existing event loop (ESP_ERR_INVALID_STATE).
 */
static esp_err_t initialize_network_stack(void)
{
    esp_err_t result = esp_netif_init();
    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "TCP/IP stack initialization failed: %s",
            esp_err_to_name(result));
        return result;
    }

    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(
            TAG,
            "Default event loop initialization failed: %s",
            esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(TAG, "TCP/IP stack initialized");
    return ESP_OK;
}

/*
 * Boot sequence: NVS -> TCP/IP stack -> Mesh-Lite root -> audio/Opus ->
 * streaming server (1704) -> JSON-RPC control server (1705).
 */
void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32-S3 Mini Snapserver");
    ESP_LOGI(TAG, "Freier Heap beim Start: %u Bytes", esp_get_free_heap_size());

    ESP_ERROR_CHECK(initialize_nvs());
    ESP_ERROR_CHECK(initialize_network_stack());

    esp_err_t result = mesh_root_start();
    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Mesh root initialization failed: %s",
            esp_err_to_name(result));
        ESP_ERROR_CHECK(result);
    }

    result = audio_opus_start();
    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Opus audio initialization failed: %s",
            esp_err_to_name(result));
        ESP_ERROR_CHECK(result);
    }

    result = snapserver_start();
	
    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Snapserver initialization failed: %s",
            esp_err_to_name(result));
        ESP_ERROR_CHECK(result);
    }
	ESP_ERROR_CHECK(snapcontrol_start());

    ESP_LOGI(
        TAG,
        "ESP32-S3 Snapserver started: TinySine stereo input, PCM5102A stereo output, Snapcast mono Opus");
    ESP_LOGI(TAG, "Freier Heap nach dem Start: %u Bytes", esp_get_free_heap_size());
}
