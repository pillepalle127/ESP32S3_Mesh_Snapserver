#include "audio_i2s.h"

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "AUDIO_I2S";

#define DMA_DESC_NUM       8
#define DMA_FRAME_NUM    240
#define MAX_FRAME_SAMPLES 960

static i2s_chan_handle_t s_tx_channel;
static i2s_chan_handle_t s_rx_channel;
static bool s_started;
static int16_t s_stereo_frame[MAX_FRAME_SAMPLES * AUDIO_I2S_CHANNELS];

esp_err_t audio_i2s_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_0,
        I2S_ROLE_MASTER);
    channel_config.dma_desc_num = DMA_DESC_NUM;
    channel_config.dma_frame_num = DMA_FRAME_NUM;
    channel_config.auto_clear = true;

    esp_err_t result = i2s_new_channel(
        &channel_config,
        &s_tx_channel,
        &s_rx_channel);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(result));
        return result;
    }

    i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_I2S_GPIO_BCLK,
            .ws = AUDIO_I2S_GPIO_LRCLK,
            .dout = AUDIO_I2S_GPIO_DOUT,
            .din = AUDIO_I2S_GPIO_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    result = i2s_channel_init_std_mode(s_tx_channel, &standard_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "TX standard mode failed: %s", esp_err_to_name(result));
        goto fail;
    }

    result = i2s_channel_init_std_mode(s_rx_channel, &standard_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "RX standard mode failed: %s", esp_err_to_name(result));
        goto fail;
    }

    result = i2s_channel_enable(s_tx_channel);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "TX enable failed: %s", esp_err_to_name(result));
        goto fail;
    }

    result = i2s_channel_enable(s_rx_channel);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "RX enable failed: %s", esp_err_to_name(result));
        i2s_channel_disable(s_tx_channel);
        goto fail;
    }

    s_started = true;
    ESP_LOGI(TAG,
             "I2S full duplex ready: master, 48 kHz, 16 bit, stereo, "
             "BCLK=%d LRCLK=%d DIN=%d DOUT=%d, no MCLK",
             AUDIO_I2S_GPIO_BCLK,
             AUDIO_I2S_GPIO_LRCLK,
             AUDIO_I2S_GPIO_DIN,
             AUDIO_I2S_GPIO_DOUT);
    return ESP_OK;

fail:
    if (s_tx_channel != NULL) {
        i2s_del_channel(s_tx_channel);
        s_tx_channel = NULL;
    }
    if (s_rx_channel != NULL) {
        i2s_del_channel(s_rx_channel);
        s_rx_channel = NULL;
    }
    return result;
}

esp_err_t audio_i2s_read_frame(int16_t *mono,
                               size_t mono_samples,
                               int64_t *timestamp_us)
{
    if (!s_started || mono == NULL || timestamp_us == NULL ||
        mono_samples == 0U || mono_samples > MAX_FRAME_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t stereo_bytes =
        mono_samples * AUDIO_I2S_CHANNELS * sizeof(int16_t);
    size_t bytes_read = 0;

    /* Zeitstempel bezeichnet den Anfang des eingelesenen Audioframes. */
    *timestamp_us = esp_timer_get_time();

    esp_err_t result = i2s_channel_read(
        s_rx_channel,
        s_stereo_frame,
        stereo_bytes,
        &bytes_read,
        portMAX_DELAY);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(result));
        return result;
    }
    if (bytes_read != stereo_bytes) {
        ESP_LOGE(TAG, "Short I2S read: %u of %u bytes",
                 (unsigned)bytes_read,
                 (unsigned)stereo_bytes);
        return ESP_FAIL;
    }

    for (size_t i = 0; i < mono_samples; ++i) {
        const int32_t left = s_stereo_frame[2U * i];
        const int32_t right = s_stereo_frame[2U * i + 1U];
        mono[i] = (int16_t)((left + right) / 2);
    }

    size_t bytes_written = 0;
    result = i2s_channel_write(
        s_tx_channel,
        s_stereo_frame,
        stereo_bytes,
        &bytes_written,
        portMAX_DELAY);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(result));
        return result;
    }
    if (bytes_written != stereo_bytes) {
        ESP_LOGE(TAG, "Short I2S write: %u of %u bytes",
                 (unsigned)bytes_written,
                 (unsigned)stereo_bytes);
        return ESP_FAIL;
    }

    return ESP_OK;
}
