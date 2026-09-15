/**
 * @file audio_opus.c
 * @brief Captures 20 ms mono PCM frames and encodes them as Opus packets.
 */
#include "audio_opus.h"

#include "audio_i2s.h"
#include "esp_log.h"
#include "opus.h"

static const char *TAG = "OPUS";

static OpusEncoder *s_encoder;
static int16_t s_mono_pcm[AUDIO_FRAME_SAMPLES];
static uint8_t s_packet_buffer[AUDIO_MAX_OPUS_PACKET];

esp_err_t audio_opus_start(void)
{
    esp_err_t result = audio_i2s_start();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "I2S initialization failed: %s", esp_err_to_name(result));
        return result;
    }

    /*
     * OPUS_APPLICATION_AUDIO ist fuer Musik optimiert; fuer reine Sprache
     * waere OPUS_APPLICATION_VOIP passend. Der Snapcast-Stream ist Musik.
     */
    int opus_error = OPUS_OK;
    s_encoder = opus_encoder_create(
        AUDIO_SAMPLE_RATE,
        AUDIO_CHANNELS,
        OPUS_APPLICATION_AUDIO,
        &opus_error);
    if (s_encoder == NULL || opus_error != OPUS_OK) {
        ESP_LOGE(TAG, "Encoder creation failed: %s", opus_strerror(opus_error));
        return ESP_FAIL;
    }

    opus_error = opus_encoder_ctl(
        s_encoder,
        OPUS_SET_BITRATE(CONFIG_SNAPSERVER_OPUS_BITRATE));
    if (opus_error != OPUS_OK) {
        ESP_LOGE(TAG, "Bitrate configuration failed: %s",
                 opus_strerror(opus_error));
        return ESP_FAIL;
    }

    opus_error = opus_encoder_ctl(
        s_encoder,
        OPUS_SET_COMPLEXITY(CONFIG_SNAPSERVER_OPUS_COMPLEXITY));
    if (opus_error != OPUS_OK) {
        ESP_LOGE(TAG, "Complexity configuration failed: %s",
                 opus_strerror(opus_error));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Encoder ready: 48 kHz, mono, %d bit/s, 20 ms",
             CONFIG_SNAPSERVER_OPUS_BITRATE);
    return ESP_OK;
}

esp_err_t audio_opus_get_packet(audio_opus_packet_t *out)
{
    if (s_encoder == NULL || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t frame_timestamp_us = 0;
    esp_err_t result = audio_i2s_read_frame(
        s_mono_pcm,
        AUDIO_FRAME_SAMPLES,
        &frame_timestamp_us);
    if (result != ESP_OK) {
        return result;
    }

    const int bytes = opus_encode(
        s_encoder,
        s_mono_pcm,
        AUDIO_FRAME_SAMPLES,
        s_packet_buffer,
        sizeof(s_packet_buffer));
    if (bytes < 0) {
        ESP_LOGE(TAG, "Encode failed: %s", opus_strerror(bytes));
        return ESP_FAIL;
    }

    out->data = s_packet_buffer;
    out->size = (size_t)bytes;
    out->timestamp_us = frame_timestamp_us;
    return ESP_OK;
}
