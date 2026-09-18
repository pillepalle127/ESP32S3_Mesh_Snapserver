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
#include "audio_sink.h"
#include "device_config.h"
#include "mesh_client.h"
#include "mesh_root.h"
#include "snapserver.h"
#include "status_led.h"
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

    device_config_t cfg;
    device_config_get(&cfg);
    /* Up before the radio, so the very first thing the LED shows is that
     * the board is alive and the GPIO is right. */
    (void)status_led_start();

    const bool client_role = (cfg.role == DEVICE_ROLE_CLIENT);

    esp_err_t result = client_role ? mesh_client_start() : mesh_root_start();
    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Mesh %s initialization failed: %s",
            client_role ? "client" : "root",
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

    /*
     * Both roles need the physical I2S bus (the client plays out over the
     * same DSP/output stage the server uses, and also captures its own
     * local-input substitute source from it). audio_i2s_start() is
     * idempotent, so the server's subsequent audio_opus_start() -- which
     * also calls it -- is a harmless no-op the second time.
     */
    ESP_ERROR_CHECK(audio_i2s_start());

    const audio_dsp_params_t dsp_params = {
        .bypass = cfg.dsp_bypass,
        .crossover_hz = (float)cfg.crossover_hz,
        .sub_gain_db = cfg.sub_gain_db,
        .wideband_gain_db = cfg.wideband_gain_db,
        .sub_channel = cfg.sub_channel,
        .wideband_channel = cfg.wideband_channel,
    };
    ESP_ERROR_CHECK(audio_i2s_set_dsp_params(&dsp_params));

    if (client_role) {
        ESP_ERROR_CHECK(audio_sink_start(cfg.buffer_ms));
        audio_sink_set_source_mode(cfg.source_mode);
        audio_sink_set_local_input_threshold_db(cfg.local_input_threshold_db);
        audio_sink_set_delay_trim_ms(cfg.delay_trim_ms);

        ESP_LOGI(
            TAG,
            "ESP32-S3 Snapclient started: mesh relay, local I2S input as "
            "alternate source, Snapcast mono Opus playback");
        return;
    }

    /*
     * Hold this device's own speaker back by the same amount every client
     * delays the stream, otherwise the server -- which hears a frame right
     * after capturing it -- runs a full bufferMs ahead of them in the same
     * room. The clients' scheduler uses chunk_ts + bufferMs - latency +
     * delay_trim_ms; latency is per-client and always 0 today, so the same
     * rule here is buffer_ms + delay_trim_ms. The buffer is sized for the
     * full delay_trim_ms range so the trim stays adjustable while playing.
     */
    const int32_t local_delay_ms = (int32_t)cfg.buffer_ms + cfg.delay_trim_ms;
    ESP_ERROR_CHECK(audio_i2s_set_output_delay(
        (uint32_t)(local_delay_ms > 0 ? local_delay_ms : 0),
        (uint32_t)cfg.buffer_ms + DEVICE_CONFIG_DELAY_TRIM_MAX_MS));

    result = audio_opus_start();
    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Opus audio initialization failed: %s",
            esp_err_to_name(result));
        ESP_ERROR_CHECK(result);
    }
    audio_opus_set_bitrate((int32_t)cfg.opus_bitrate);
    audio_opus_set_complexity((int32_t)cfg.opus_complexity);

    result = snapserver_start();
    if (result == ESP_OK) {
        /* The server always plays its own local output, so green from here
         * on and the brightness follows what leaves its crossover. */
        status_led_set_state(STATUS_LED_PLAYING);
    }

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
