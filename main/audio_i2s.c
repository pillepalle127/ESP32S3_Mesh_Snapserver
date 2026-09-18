/**
 * @file audio_i2s.c
 * @brief Full-duplex I2S input, mono mix, local LR4 crossover output and
 *        the server's output delay line.
 */
#include "audio_i2s.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "AUDIO_I2S";

#define DMA_DESC_NUM        AUDIO_I2S_DMA_DESC_NUM
#define DMA_FRAME_NUM       AUDIO_I2S_DMA_FRAME_NUM
#define MAX_FRAME_SAMPLES   960
#define BIQUAD_BUTTERWORTH_Q 0.7071067811865476f
#define OUTPUT_CHANNELS       2U

/*
 * If the sample counter based timestamp drifts further than this from the
 * hardware timer, the stream is re-anchored. That only happens after a real
 * discontinuity such as a DMA overflow, not during normal operation.
 */
#define TIMESTAMP_RESYNC_THRESHOLD_US 100000LL

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
static int16_t s_delayed_mono[MAX_FRAME_SAMPLES];

/*
 * s_lowpass/s_highpass hold both the biquad coefficients (b0,b1,b2,a1,a2,
 * changed only by audio_i2s_set_dsp_params(), from whatever task calls it)
 * and the running filter state z1/z2 (changed only by audio_i2s_read_frame()
 * in the audio task, every sample). s_dsp_lock guards both, but
 * audio_i2s_read_frame() only holds it twice per frame -- once to snapshot
 * everything into locals before the 960-sample loop, once to write the
 * updated z1/z2 back afterwards -- never per sample, so the hot path never
 * blocks on a task that might be preempted mid-update.
 */
static lr4_filter_t s_lowpass;
static lr4_filter_t s_highpass;
static audio_dsp_params_t s_dsp_params;
static float s_sub_gain_linear = 1.0f;
static float s_wideband_gain_linear = 1.0f;
static portMUX_TYPE s_dsp_lock = portMUX_INITIALIZER_UNLOCKED;
/*
 * Bumped under s_dsp_lock every time audio_i2s_set_dsp_params() installs a
 * fresh (zeroed) filter state. audio_i2s_read_frame() records the
 * generation at snapshot time and only writes its own z1/z2 back if it is
 * still current: otherwise a param change that landed mid-frame would have
 * its clean reset immediately overwritten by this frame's stale filter
 * memory computed from the old coefficients.
 */
static uint32_t s_dsp_generation;

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

/*
 * Output delay line for the server's own speaker (see
 * audio_i2s_set_output_delay()). Samples are written at s_delay_write and
 * read s_delay_samples behind it, so changing the delay just moves the read
 * offset -- no reallocation, and the trim stays adjustable while playing.
 * Written and read only from the audio task inside audio_i2s_read_frame().
 */
static int16_t *s_delay_line;
static size_t s_delay_capacity;
static size_t s_delay_write;
static volatile size_t s_delay_samples;

/*
 * Diagnostic: peak level of what actually leaves the crossover, per output
 * channel, reported every 5 s. audio_sink.c's own output_rms is measured on
 * the mono signal *before* the DSP, so it cannot tell "no audio reaching
 * the DAC" apart from "DSP or channel assignment eats it". This closes that
 * gap -- everything after it is wiring and the DAC itself.
 */
static int16_t s_peak_left;
static int16_t s_peak_right;
/*
 * A second, independent pair for the status LED. Both readers clear what
 * they read, so sharing one pair would mean each takes level away from the
 * other -- the LED samples 30 times a second, the diagnostic every five
 * seconds, and neither would see the real peak.
 */
static int16_t s_led_peak_left;
static int16_t s_led_peak_right;

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

esp_err_t audio_i2s_set_dsp_params(const audio_dsp_params_t *params)
{
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const float nyquist_hz = (float)AUDIO_I2S_SAMPLE_RATE * 0.5f;
    if (params->crossover_hz <= 0.0f || params->crossover_hz >= nyquist_hz) {
        ESP_LOGE(TAG,
                 "Invalid crossover frequency: %.1f Hz",
                 (double)params->crossover_hz);
        return ESP_ERR_INVALID_ARG;
    }
    if (params->sub_channel > PCM_CHANNEL_RIGHT ||
        params->wideband_channel > PCM_CHANNEL_RIGHT ||
        params->sub_channel == params->wideband_channel) {
        ESP_LOGE(TAG,
                 "Invalid output channel assignment: sub=%u wideband=%u",
                 (unsigned)params->sub_channel,
                 (unsigned)params->wideband_channel);
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Coefficient math (sinf/cosf/division) and the dB->linear conversion
     * happen here, off the hot path, before anything is made visible to
     * audio_i2s_read_frame(). configure_biquad_lowpass()/highpass() also
     * reset z1/z2 to 0, so every param change starts the filter from a
     * clean state -- a brief transient on the rare Save action, traded for
     * not having to reconcile old filter memory against new coefficients.
     */
    lr4_filter_t new_lowpass;
    lr4_filter_t new_highpass;
    configure_biquad_lowpass(&new_lowpass.stage1, (float)AUDIO_I2S_SAMPLE_RATE, params->crossover_hz);
    configure_biquad_lowpass(&new_lowpass.stage2, (float)AUDIO_I2S_SAMPLE_RATE, params->crossover_hz);
    configure_biquad_highpass(&new_highpass.stage1, (float)AUDIO_I2S_SAMPLE_RATE, params->crossover_hz);
    configure_biquad_highpass(&new_highpass.stage2, (float)AUDIO_I2S_SAMPLE_RATE, params->crossover_hz);

    const float sub_gain_linear = powf(10.0f, params->sub_gain_db / 20.0f);
    const float wideband_gain_linear = powf(10.0f, params->wideband_gain_db / 20.0f);

    portENTER_CRITICAL(&s_dsp_lock);
    s_lowpass = new_lowpass;
    s_highpass = new_highpass;
    s_dsp_params = *params;
    s_sub_gain_linear = sub_gain_linear;
    s_wideband_gain_linear = wideband_gain_linear;
    s_dsp_generation++;
    portEXIT_CRITICAL(&s_dsp_lock);

    return ESP_OK;
}

void audio_i2s_get_dsp_params(audio_dsp_params_t *out)
{
    if (out == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_dsp_lock);
    *out = s_dsp_params;
    portEXIT_CRITICAL(&s_dsp_lock);
}

esp_err_t audio_i2s_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    const audio_dsp_params_t default_params = {
        .bypass = false,
        .crossover_hz = AUDIO_CROSSOVER_FREQUENCY_HZ,
        .sub_gain_db = 0.0f,
        .wideband_gain_db = 0.0f,
        .sub_channel = SUBWOOFER_OUTPUT_CHANNEL,
        .wideband_channel = WIDEBAND_OUTPUT_CHANNEL,
    };
    esp_err_t result = audio_i2s_set_dsp_params(&default_params);
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

esp_err_t audio_i2s_set_output_delay(uint32_t delay_ms, uint32_t max_delay_ms)
{
    if (delay_ms > max_delay_ms) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_delay_line == NULL && max_delay_ms > 0U) {
        const size_t capacity =
            (size_t)max_delay_ms * (AUDIO_I2S_SAMPLE_RATE / 1000U);
        const size_t bytes = capacity * sizeof(int16_t);

        s_delay_line = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_delay_line == NULL) {
            s_delay_line = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        }
        if (s_delay_line == NULL) {
            ESP_LOGE(TAG, "No memory for %u B output delay line", (unsigned)bytes);
            return ESP_ERR_NO_MEM;
        }

        memset(s_delay_line, 0, bytes);
        s_delay_capacity = capacity;
        s_delay_write = 0;
    }

    size_t samples = (size_t)delay_ms * (AUDIO_I2S_SAMPLE_RATE / 1000U);
    if (samples > s_delay_capacity) {
        samples = s_delay_capacity;
    }
    s_delay_samples = samples;

    ESP_LOGI(TAG, "Local output delayed by %u ms (%u samples)",
             (unsigned)delay_ms, (unsigned)samples);
    return ESP_OK;
}

/*
 * Pushes the captured frame through the delay line and writes the samples
 * from s_delay_samples ago to `out`. Deliberately not in place: the input
 * buffer is what the caller hands to the Opus encoder, and the network
 * stream must stay undelayed -- only the local speaker is held back.
 * Returns false when no delay is configured (the client's case, and the
 * server's until app_main() sets one up), leaving `out` untouched.
 */
static bool apply_output_delay(const int16_t *in, int16_t *out, size_t mono_samples)
{
    const size_t delay = s_delay_samples;
    if (s_delay_line == NULL || delay == 0U) {
        return false;
    }

    /*
     * Wrapping by compare-and-subtract, not by %: the capacity is
     * delay_ms * 48 and thus never a power of two, so each modulo compiled
     * to a real 32-bit division -- two per sample, 1920 per frame, on the
     * order of 250 us of the 20 ms frame budget. Both indices advance by
     * exactly one per iteration and stay inside [0, capacity), so a single
     * comparison is equivalent.
     */
    size_t read_pos = s_delay_write + s_delay_capacity - delay;
    if (read_pos >= s_delay_capacity) {
        read_pos -= s_delay_capacity;
    }

    for (size_t i = 0; i < mono_samples; ++i) {
        out[i] = s_delay_line[read_pos];
        s_delay_line[s_delay_write] = in[i];

        if (++read_pos >= s_delay_capacity) {
            read_pos = 0;
        }
        if (++s_delay_write >= s_delay_capacity) {
            s_delay_write = 0;
        }
    }

    return true;
}

static esp_err_t capture_mono_from_rx(int16_t *mono, size_t mono_samples)
{
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

    for (size_t i = 0; i < mono_samples; ++i) {
        const int32_t left = s_input_stereo[2U * i];
        const int32_t right = s_input_stereo[2U * i + 1U];
        mono[i] = (int16_t)((left + right) / 2);
    }

    return ESP_OK;
}

static esp_err_t apply_dsp_and_output(const int16_t *mono, size_t mono_samples)
{
    /*
     * Snapshot the whole DSP state once per frame (not per sample) so the
     * per-sample loop below runs lock-free. See the s_lowpass/s_highpass
     * comment above for why only z1/z2 get written back afterwards.
     */
    lr4_filter_t lowpass;
    lr4_filter_t highpass;
    audio_dsp_params_t dsp;
    float sub_gain_linear;
    float wideband_gain_linear;
    uint32_t dsp_generation;
    portENTER_CRITICAL(&s_dsp_lock);
    lowpass = s_lowpass;
    highpass = s_highpass;
    dsp = s_dsp_params;
    sub_gain_linear = s_sub_gain_linear;
    wideband_gain_linear = s_wideband_gain_linear;
    dsp_generation = s_dsp_generation;
    portEXIT_CRITICAL(&s_dsp_lock);

    for (size_t i = 0; i < mono_samples; ++i) {
        const int16_t mono_sample = mono[i];

        float subwoofer;
        float wideband;
        if (dsp.bypass) {
            subwoofer = (float)mono_sample;
            wideband = (float)mono_sample;
        } else {
            subwoofer = lr4_process(&lowpass, (float)mono_sample) * sub_gain_linear;
            wideband = lr4_process(&highpass, (float)mono_sample) * wideband_gain_linear;
        }

        const int16_t sub_sample = float_to_int16(subwoofer);
        const int16_t wide_sample = float_to_int16(wideband);
        s_output_stereo[2U * i + dsp.sub_channel] = sub_sample;
        s_output_stereo[2U * i + dsp.wideband_channel] = wide_sample;

        /* Folded into this loop rather than a second pass over the frame --
         * a diagnostic must not cost another 960 iterations. */
        const int16_t abs_sub = (sub_sample < 0) ? (int16_t)(-(int32_t)sub_sample) : sub_sample;
        const int16_t abs_wide = (wide_sample < 0) ? (int16_t)(-(int32_t)wide_sample) : wide_sample;
        int16_t *peak_sub = (dsp.sub_channel == PCM_CHANNEL_LEFT) ? &s_peak_left : &s_peak_right;
        int16_t *peak_wide = (dsp.wideband_channel == PCM_CHANNEL_LEFT) ? &s_peak_left : &s_peak_right;
        if (abs_sub > *peak_sub) {
            *peak_sub = abs_sub;
        }
        if (abs_wide > *peak_wide) {
            *peak_wide = abs_wide;
        }

        int16_t *led_sub = (dsp.sub_channel == PCM_CHANNEL_LEFT)
                               ? &s_led_peak_left : &s_led_peak_right;
        int16_t *led_wide = (dsp.wideband_channel == PCM_CHANNEL_LEFT)
                                ? &s_led_peak_left : &s_led_peak_right;
        if (abs_sub > *led_sub) {
            *led_sub = abs_sub;
        }
        if (abs_wide > *led_wide) {
            *led_wide = abs_wide;
        }
    }

    /*
     * Write the updated filter memory back so the next frame continues from
     * here -- but only if s_dsp_generation is still what it was at snapshot
     * time. Coefficients are intentionally never touched here regardless:
     * if audio_i2s_set_dsp_params() swapped them in concurrently, this
     * frame's (now-stale) copy must not clobber the fresh ones. Checking
     * the generation additionally guards the z1/z2 write itself: without
     * it, this frame's filter memory (computed from the coefficients that
     * were live *before* the concurrent change) would overwrite the clean
     * zeroed state audio_i2s_set_dsp_params() just installed, reintroducing
     * exactly the coefficient/state mismatch the fresh reset exists to
     * avoid.
     */
    portENTER_CRITICAL(&s_dsp_lock);
    if (s_dsp_generation == dsp_generation) {
        s_lowpass.stage1.z1 = lowpass.stage1.z1;
        s_lowpass.stage1.z2 = lowpass.stage1.z2;
        s_lowpass.stage2.z1 = lowpass.stage2.z1;
        s_lowpass.stage2.z2 = lowpass.stage2.z2;
        s_highpass.stage1.z1 = highpass.stage1.z1;
        s_highpass.stage1.z2 = highpass.stage1.z2;
        s_highpass.stage2.z1 = highpass.stage2.z1;
        s_highpass.stage2.z2 = highpass.stage2.z2;
    }
    portEXIT_CRITICAL(&s_dsp_lock);

    const size_t stereo_bytes =
        mono_samples * OUTPUT_CHANNELS * sizeof(int16_t);
    size_t bytes_written = 0;
    esp_err_t result = i2s_channel_write(
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

void audio_i2s_take_led_peak(int16_t *left, int16_t *right)
{
    portENTER_CRITICAL(&s_dsp_lock);
    *left = s_led_peak_left;
    *right = s_led_peak_right;
    s_led_peak_left = 0;
    s_led_peak_right = 0;
    portEXIT_CRITICAL(&s_dsp_lock);
}

void audio_i2s_take_output_peak(int16_t *left, int16_t *right)
{
    portENTER_CRITICAL(&s_dsp_lock);
    *left = s_peak_left;
    *right = s_peak_right;
    s_peak_left = 0;
    s_peak_right = 0;
    portEXIT_CRITICAL(&s_dsp_lock);
}

esp_err_t audio_i2s_capture_mono(int16_t *mono, size_t mono_samples)
{
    if (!s_started || mono == NULL ||
        mono_samples == 0U || mono_samples > MAX_FRAME_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }

    return capture_mono_from_rx(mono, mono_samples);
}

esp_err_t audio_i2s_write_mono(const int16_t *mono, size_t mono_samples)
{
    if (!s_started || mono == NULL ||
        mono_samples == 0U || mono_samples > MAX_FRAME_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }

    return apply_dsp_and_output(mono, mono_samples);
}

esp_err_t audio_i2s_read_frame(int16_t *mono,
                               size_t mono_samples,
                               int64_t *timestamp_us)
{
    if (!s_started || mono == NULL || timestamp_us == NULL ||
        mono_samples == 0U || mono_samples > MAX_FRAME_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = capture_mono_from_rx(mono, mono_samples);
    if (result != ESP_OK) {
        return result;
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

    /*
     * The encoder gets `mono` as captured; only what goes to this device's
     * own speaker is held back, so it lines up with the clients playing the
     * same chunk bufferMs later.
     */
    const int16_t *output = mono;
    if (apply_output_delay(mono, s_delayed_mono, mono_samples)) {
        output = s_delayed_mono;
    }

    return apply_dsp_and_output(output, mono_samples);
}
