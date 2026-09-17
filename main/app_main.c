/**
 * @file app_main.c
 * @brief Initializes NVS, networking, Mesh-Lite, audio, streaming and control services.
 */
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "snapcontrol.h"

#include "audio_i2s.h"
#include "audio_opus.h"
#include "device_config.h"
#include "mesh_root.h"
#include "snapserver.h"
#include "webconfig.h"

static const char *TAG = "APP";

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

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32-S3 Mini Snapserver");

    ESP_ERROR_CHECK(initialize_nvs());
    ESP_ERROR_CHECK(initialize_network_stack());
    ESP_ERROR_CHECK(device_config_load());

    esp_err_t result = mesh_root_start();
    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Mesh root initialization failed: %s",
            esp_err_to_name(result));
        ESP_ERROR_CHECK(result);
    }

    /*
     * Bring the config page up before audio: if I2S/Opus init fails below,
     * ESP_ERROR_CHECK() still panics and reboots the device (that failure
     * stays fatal, unchanged), but the panic-and-reboot loop at least
     * leaves a window where the provisioning/mesh AP and config page were
     * already reachable, instead of never existing at all.
     */
    ESP_ERROR_CHECK(webconfig_start());

    result = audio_opus_start();
    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Opus audio initialization failed: %s",
            esp_err_to_name(result));
        ESP_ERROR_CHECK(result);
    }

    /*
     * audio_opus_start()/audio_i2s_start() just seeded compile-time Kconfig
     * defaults; apply whatever was actually loaded from NVS (or seeded as
     * first-boot defaults, which are the same values anyway) on top before
     * any audio flows.
     */
    device_config_t cfg;
    device_config_get(&cfg);
    const audio_dsp_params_t dsp_params = {
        .bypass = cfg.dsp_bypass,
        .crossover_hz = (float)cfg.crossover_hz,
        .sub_gain_db = cfg.sub_gain_db,
        .wideband_gain_db = cfg.wideband_gain_db,
        .sub_channel = cfg.sub_channel,
        .wideband_channel = cfg.wideband_channel,
    };
    ESP_ERROR_CHECK(audio_i2s_set_dsp_params(&dsp_params));
    audio_opus_set_bitrate((int32_t)cfg.opus_bitrate);
    audio_opus_set_complexity((int32_t)cfg.opus_complexity);

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
}
