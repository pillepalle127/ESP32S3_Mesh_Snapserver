/**
 * @file audio_opus.c
 * @brief Captures 20 ms mono PCM frames and encodes them as Opus packets.
 */
#include "audio_opus.h"

#include "audio_i2s.h"
#include "esp_log.h"
#include "opus.h"

static const char *TAG = "OPUS";

#define OPUS_PENDING_NONE (-1)

static OpusEncoder *s_encoder;
static int16_t s_mono_pcm[AUDIO_FRAME_SAMPLES];
static uint8_t s_packet_buffer[AUDIO_MAX_OPUS_PACKET];

/*
 * opus_encoder_ctl() isn't documented safe to call concurrently with
 * opus_encode() from another task, and opus_encode() itself runs too long
 * to hold a lock around. Instead of a lock, the setters below just store a
 * requested value here (a single aligned int32_t store, effectively atomic
 * for this single-writer/single-reader use) and audio_opus_get_packet()
 * -- the only caller of opus_encoder_ctl() -- picks it up and clears it
 * before encoding its next frame.
 */
static volatile int32_t s_pending_bitrate = OPUS_PENDING_NONE;
static volatile int32_t s_pending_complexity = OPUS_PENDING_NONE;

esp_err_t audio_opus_start(void)
{
    esp_err_t result = audio_i2s_start();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "I2S initialization failed: %s", esp_err_to_name(result));
        return result;
    }

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

    const int32_t pending_bitrate = s_pending_bitrate;
    if (pending_bitrate != OPUS_PENDING_NONE) {
        s_pending_bitrate = OPUS_PENDING_NONE;
        const int opus_error =
            opus_encoder_ctl(s_encoder, OPUS_SET_BITRATE(pending_bitrate));
        if (opus_error != OPUS_OK) {
            ESP_LOGW(TAG, "Applying bitrate failed: %s", opus_strerror(opus_error));
        } else {
            ESP_LOGI(TAG, "Bitrate now %d bit/s", (int)pending_bitrate);
        }
    }

    const int32_t pending_complexity = s_pending_complexity;
    if (pending_complexity != OPUS_PENDING_NONE) {
        s_pending_complexity = OPUS_PENDING_NONE;
        const int opus_error =
            opus_encoder_ctl(s_encoder, OPUS_SET_COMPLEXITY(pending_complexity));
        if (opus_error != OPUS_OK) {
            ESP_LOGW(TAG, "Applying complexity failed: %s", opus_strerror(opus_error));
        } else {
            ESP_LOGI(TAG, "Complexity now %d", (int)pending_complexity);
        }
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

esp_err_t audio_opus_set_bitrate(int32_t bitrate)
{
    if (bitrate < 16000 || bitrate > 192000) {
        return ESP_ERR_INVALID_ARG;
    }
    s_pending_bitrate = bitrate;
    return ESP_OK;
}

esp_err_t audio_opus_set_complexity(int32_t complexity)
{
    if (complexity < 0 || complexity > 10) {
        return ESP_ERR_INVALID_ARG;
    }
    s_pending_complexity = complexity;
    return ESP_OK;
}
