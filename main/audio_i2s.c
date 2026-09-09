/**
 * @file audio_i2s.c
 * @brief Full-duplex I2S input, mono mix and local LR4 crossover output.
 */
#include "audio_i2s.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "AUDIO_I2S";

#define DMA_DESC_NUM          8
#define DMA_FRAME_NUM       240
#define MAX_FRAME_SAMPLES   960
#define BIQUAD_BUTTERWORTH_Q 0.7071067811865476f
#define OUTPUT_CHANNELS       2U

/*
 * If the sample counter based timestamp drifts further than this from the
 * hardware timer, the stream is re-anchored. That only happens after a real
 * discontinuity such as a DMA overflow, not during normal operation.
 */
#define TIMESTAMP_RESYNC_THRESHOLD_US 100000LL

#if SUBWOOFER_OUTPUT_CHANNEL == WIDEBAND_OUTPUT_CHANNEL
#error "Subwoofer and wideband must use different PCM5102A channels"
#endif

#if (SUBWOOFER_OUTPUT_CHANNEL > PCM_CHANNEL_RIGHT) || \
    (WIDEBAND_OUTPUT_CHANNEL > PCM_CHANNEL_RIGHT)
#error "Invalid PCM5102A output channel"
#endif

typedef struct {
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
    float z1;
    float z2;
} biquad_t;

typedef struct {
    biquad_t stage1;
    biquad_t stage2;
} lr4_filter_t;

static i2s_chan_handle_t s_tx_channel;
static i2s_chan_handle_t s_rx_channel;
static bool s_started;

static int16_t s_input_stereo[MAX_FRAME_SAMPLES * AUDIO_I2S_CHANNELS];
static int16_t s_output_stereo[MAX_FRAME_SAMPLES * OUTPUT_CHANNELS];

static lr4_filter_t s_lowpass;
static lr4_filter_t s_highpass;

/*
 * Capture timeline.
 *
 * The timestamp must describe when the samples were captured, not when the
 * call happened to return. i2s_channel_read() hands out data from a DMA ring
 * that may hold several frames, and the subsequent blocking write can stall
 * this task, so reading the hardware timer after the fact yields a value that
 * jumps by whole DMA frames. Derive the timestamp from a continuous sample
 * count anchored at stream start instead: it is monotonic, evenly spaced and
 * immune to DMA fill level and TX back pressure.
 */
static int64_t s_stream_anchor_us;
static int64_t s_samples_captured;

static int16_t float_to_int16(float sample)
{
    if (sample > 32767.0f) {
        return INT16_MAX;
    }
    if (sample < -32768.0f) {
        return INT16_MIN;
    }
    return (int16_t)lrintf(sample);
}

static float biquad_process(biquad_t *filter, float input)
{
    const float output = filter->b0 * input + filter->z1;
    filter->z1 = filter->b1 * input - filter->a1 * output + filter->z2;
    filter->z2 = filter->b2 * input - filter->a2 * output;
    return output;
}

static float lr4_process(lr4_filter_t *filter, float input)
{
    return biquad_process(&filter->stage2,
                          biquad_process(&filter->stage1, input));
}

static void configure_biquad_lowpass(biquad_t *filter,
                                     float sample_rate,
                                     float cutoff_hz)
{
    const float omega = 2.0f * (float)M_PI * cutoff_hz / sample_rate;
    const float cosine = cosf(omega);
    const float sine = sinf(omega);
    const float alpha = sine / (2.0f * BIQUAD_BUTTERWORTH_Q);
    const float a0 = 1.0f + alpha;

    filter->b0 = ((1.0f - cosine) * 0.5f) / a0;
    filter->b1 = (1.0f - cosine) / a0;
    filter->b2 = filter->b0;
    filter->a1 = (-2.0f * cosine) / a0;
    filter->a2 = (1.0f - alpha) / a0;
    filter->z1 = 0.0f;
    filter->z2 = 0.0f;
}

static void configure_biquad_highpass(biquad_t *filter,
                                      float sample_rate,
                                      float cutoff_hz)
{
    const float omega = 2.0f * (float)M_PI * cutoff_hz / sample_rate;
    const float cosine = cosf(omega);
    const float sine = sinf(omega);
    const float alpha = sine / (2.0f * BIQUAD_BUTTERWORTH_Q);
    const float a0 = 1.0f + alpha;

    filter->b0 = ((1.0f + cosine) * 0.5f) / a0;
    filter->b1 = (-(1.0f + cosine)) / a0;
    filter->b2 = filter->b0;
    filter->a1 = (-2.0f * cosine) / a0;
    filter->a2 = (1.0f - alpha) / a0;
    filter->z1 = 0.0f;
    filter->z2 = 0.0f;
}

static esp_err_t configure_crossover(void)
{
    const float nyquist_hz = (float)AUDIO_I2S_SAMPLE_RATE * 0.5f;

    if (AUDIO_CROSSOVER_FREQUENCY_HZ <= 0.0f ||
        AUDIO_CROSSOVER_FREQUENCY_HZ >= nyquist_hz) {
        ESP_LOGE(TAG,
                 "Invalid crossover frequency: %.1f Hz",
                 (double)AUDIO_CROSSOVER_FREQUENCY_HZ);
        return ESP_ERR_INVALID_ARG;
    }

    configure_biquad_lowpass(&s_lowpass.stage1,
                             (float)AUDIO_I2S_SAMPLE_RATE,
                             AUDIO_CROSSOVER_FREQUENCY_HZ);
    configure_biquad_lowpass(&s_lowpass.stage2,
                             (float)AUDIO_I2S_SAMPLE_RATE,
                             AUDIO_CROSSOVER_FREQUENCY_HZ);

    configure_biquad_highpass(&s_highpass.stage1,
                              (float)AUDIO_I2S_SAMPLE_RATE,
                              AUDIO_CROSSOVER_FREQUENCY_HZ);
    configure_biquad_highpass(&s_highpass.stage2,
                              (float)AUDIO_I2S_SAMPLE_RATE,
                              AUDIO_CROSSOVER_FREQUENCY_HZ);

    return ESP_OK;
}

esp_err_t audio_i2s_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    esp_err_t result = configure_crossover();
    if (result != ESP_OK) {
        return result;
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_0,
        I2S_ROLE_MASTER);
    channel_config.dma_desc_num = DMA_DESC_NUM;
    channel_config.dma_frame_num = DMA_FRAME_NUM;
    channel_config.auto_clear = true;

    result = i2s_new_channel(
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

    /* 16-Bit-Audiodaten in 32-Bit-I2S-Slots:
     * 48 kHz x 2 Kanaele x 32 Bit = 3,072 MHz BCLK.
     */
    standard_config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    standard_config.slot_cfg.ws_width = 32;

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

    s_stream_anchor_us = 0;
    s_samples_captured = 0;
    s_started = true;

    ESP_LOGI(TAG,
             "I2S full duplex ready: master, 48 kHz, 16-bit data in 32-bit slots, stereo, "
             "BCLK=%d LRCLK=%d DIN=%d DOUT=%d, no MCLK",
             AUDIO_I2S_GPIO_BCLK,
             AUDIO_I2S_GPIO_LRCLK,
             AUDIO_I2S_GPIO_DIN,
             AUDIO_I2S_GPIO_DOUT);
    ESP_LOGI(TAG,
             "Local LR4 crossover ready: %.1f Hz, sub=%s, wideband=%s",
             (double)AUDIO_CROSSOVER_FREQUENCY_HZ,
             SUBWOOFER_OUTPUT_CHANNEL == PCM_CHANNEL_LEFT ? "left" : "right",
             WIDEBAND_OUTPUT_CHANNEL == PCM_CHANNEL_LEFT ? "left" : "right");

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

    esp_err_t result = i2s_channel_read(
        s_rx_channel,
        s_input_stereo,
        stereo_bytes,
        &bytes_read,
        portMAX_DELAY);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(result));
        return result;
    }

    if (bytes_read != stereo_bytes) {
        ESP_LOGE(TAG,
                 "Short I2S read: %u of %u bytes",
                 (unsigned)bytes_read,
                 (unsigned)stereo_bytes);
        return ESP_FAIL;
    }

    const int64_t frame_duration_us =
        ((int64_t)mono_samples * 1000000LL) / AUDIO_I2S_SAMPLE_RATE;

    /*
     * Anchor the timeline on the first frame. The data returned now was
     * captured one frame duration ago at the earliest, so shift the anchor
     * back by exactly that amount.
     */
    if (s_stream_anchor_us == 0) {
        s_stream_anchor_us = esp_timer_get_time() - frame_duration_us;
        s_samples_captured = 0;
    }

    int64_t frame_timestamp_us =
        s_stream_anchor_us +
        (s_samples_captured * 1000000LL) / AUDIO_I2S_SAMPLE_RATE;

    /*
     * Guard against a lost sync, e.g. after a DMA overflow: if the counted
     * timeline runs away from the hardware timer, re-anchor once instead of
     * accumulating the error forever.
     */
    const int64_t expected_now_us = frame_timestamp_us + frame_duration_us;
    const int64_t drift_us = esp_timer_get_time() - expected_now_us;

    if (drift_us > TIMESTAMP_RESYNC_THRESHOLD_US ||
        drift_us < -TIMESTAMP_RESYNC_THRESHOLD_US) {
        ESP_LOGW(TAG,
                 "Capture timeline drifted %lld us, re-anchoring",
                 (long long)drift_us);

        s_stream_anchor_us = esp_timer_get_time() - frame_duration_us;
        s_samples_captured = 0;
        frame_timestamp_us = s_stream_anchor_us;
    }

    *timestamp_us = frame_timestamp_us;
    s_samples_captured += (int64_t)mono_samples;

    for (size_t i = 0; i < mono_samples; ++i) {
        const int32_t left = s_input_stereo[2U * i];
        const int32_t right = s_input_stereo[2U * i + 1U];
        const int16_t mono_sample = (int16_t)((left + right) / 2);

        mono[i] = mono_sample;

        const float subwoofer = lr4_process(&s_lowpass, (float)mono_sample);
        const float wideband = lr4_process(&s_highpass, (float)mono_sample);

        s_output_stereo[2U * i + SUBWOOFER_OUTPUT_CHANNEL] =
            float_to_int16(subwoofer);
        s_output_stereo[2U * i + WIDEBAND_OUTPUT_CHANNEL] =
            float_to_int16(wideband);
    }

    size_t bytes_written = 0;
    result = i2s_channel_write(
        s_tx_channel,
        s_output_stereo,
        stereo_bytes,
        &bytes_written,
        portMAX_DELAY);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(result));
        return result;
    }

    if (bytes_written != stereo_bytes) {
        ESP_LOGE(TAG,
                 "Short I2S write: %u of %u bytes",
                 (unsigned)bytes_written,
                 (unsigned)stereo_bytes);
        return ESP_FAIL;
    }

    return ESP_OK;
}
