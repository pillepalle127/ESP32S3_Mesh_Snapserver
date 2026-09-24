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

#include "device_config.h"
#include "driver/gpio.h"
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

/* How far ahead of bufferMs the server's own speaker plays, on top of the
 * TX queue. See audio_i2s_set_output_delay() for what it is made of. */
#define SERVER_LEAD_MS 20U

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
 * Server-local voice-announcement override (see audio_i2s_set_voice_active()
 * in the header). A tiny FIFO, not a ring the size of the delay line above:
 * announcement packets arrive in 10 ms pieces against this module's 20 ms
 * frame cadence, so at most about one frame's worth is ever in flight here.
 * Guarded by s_voice_lock, a separate spinlock from s_dsp_lock below since
 * the two are never held together and this one is written from a completely
 * different task (voice_announce.c's UDP receive loop).
 */
static volatile bool s_voice_active;
/*
 * 100 ms. The decoder shares core 1 with the music encoder, so it is
 * preempted and then delivers several packets back to back: with 60 ms the
 * mailbox overflowed on those bursts (2026-09-20, "voice mailbox: dropped"
 * next to underruns in the same 5 s). Only bursts use the extra room, so
 * the prefill below still sets the latency.
 */
static int16_t s_voice_mailbox[MAX_FRAME_SAMPLES * 5];
static volatile size_t s_voice_mailbox_fill;
static int16_t s_voice_frame[MAX_FRAME_SAMPLES];
static portMUX_TYPE s_voice_lock = portMUX_INITIALIZER_UNLOCKED;
/*
 * Prefill and diagnostics, same scheme as audio_sink.c's voice mailbox.
 * One frame (20 ms), not two: the prefill is pure latency on a path whose
 * whole point is to be quick, and it only has to cover the jitter between
 * two packets, not a network outage. Raise it again if the underrun
 * counters in the stats line start climbing.
 */
#define VOICE_PREFILL_SAMPLES (MAX_FRAME_SAMPLES)
/*
 * Upper bound on the queue, i.e. on the latency it adds: 60 ms. Anything
 * beyond is cut back in audio_i2s_feed_voice().
 *
 * 40 ms was tried first and traded badly: packets arrive in bursts, and
 * cutting back that hard threw away exactly the audio that would have
 * covered the gap behind the burst -- 440 ms missing and 460 ms discarded
 * within five seconds, against 40 ms of missing audio before the cap
 * existed at all (2026-09-20). 60 ms still stops the queue from ratcheting
 * up over the course of an announcement, which is what the cap is for.
 */
#define VOICE_TARGET_FILL_SAMPLES (MAX_FRAME_SAMPLES)
/* ~0.5 s of staying too full before the backlog is cut. Long enough that a
 * burst and the gap behind it pass undisturbed, short enough that nobody
 * hears the queue creeping up during an announcement. */
#define VOICE_TRIM_AFTER_FEEDS  25U
static bool s_voice_primed;
static uint32_t s_voice_underrun_samples;
static uint32_t s_voice_dropped_samples;
/* Consecutive feeds that found the mailbox above its target, see there. */
static uint32_t s_voice_above_target;


/* Queueing delay of the announcement in this mailbox, see feed_voice(). */
static uint32_t s_voice_wait_total_ms;
static uint32_t s_voice_wait_count;
static uint32_t s_voice_wait_max_ms;

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
/*
 * Local master volume, fed by the volume knob in pots.c. The target is
 * written from that task under s_dsp_lock; the current value belongs to
 * the audio task alone, which walks it towards the target across one
 * frame. Splitting the two is what lets the knob move at any moment
 * without the ramp ever being touched by two tasks at once.
 *
 * It defaults to 1.0 so a build without a knob -- or one whose ADC failed
 * to start -- plays at full volume rather than silence.
 */
static float s_master_volume_target = 1.0f;
static float s_master_volume_current = 1.0f;
/* audio_i2s_set_user_volume(); multiplies s_master_volume_target. */
static float s_user_volume = 1.0f;
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

/* Clock check, see apply_dsp_and_output() and audio_i2s_clock_ppm(). */
static int64_t s_rate_anchor_us;
static int64_t s_rate_samples;

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

void audio_i2s_set_master_volume(float linear)
{
    /*
     * Written as a lower bound rather than "< 0.0f" so a NaN -- which
     * compares false against everything -- lands on silence instead of
     * being multiplied into every sample of the output.
     */
    if (!(linear >= 0.0f)) {
        linear = 0.0f;
    } else if (linear > 1.0f) {
        linear = 1.0f;
    }

    portENTER_CRITICAL(&s_dsp_lock);
    s_master_volume_target = linear;
    portEXIT_CRITICAL(&s_dsp_lock);
}

void audio_i2s_set_user_volume(uint8_t percent, bool muted)
{
    if (percent > 100U) {
        percent = 100U;
    }
    const float fraction = (float)percent / 100.0f;
    const float linear = muted ? 0.0f : fraction * fraction * fraction;

    portENTER_CRITICAL(&s_dsp_lock);
    s_user_volume = linear;
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

    device_pins_t pins;
    device_config_get_pins(&pins);

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
            .bclk = pins.i2s_bclk,
            .ws = pins.i2s_lrclk,
            .dout = pins.i2s_dout,
            .din = pins.i2s_din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    /*
     * 16-bit audio in 32-bit slots: 48 kHz x 2 x 32 bit = 3.072 MHz BCLK.
     * 24-bit slots (2.304 MHz, the TinySine's own master clock) were tried
     * twice on 2026-09-24. The first trial kept MCLK at 256 x fs: bclk_div
     * 5.33 truncated to 5, LRCLK ran at 51.2 kHz and everything sounded
     * distorted. The second, with 384 x fs, ran at the right rate but did
     * not cure the start-dependent A2DP corruption either.
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

    /*
     * Pull the data input low. The TinySine stops driving its SD output for
     * a few seconds after an A2DP stream ends; left floating, the pin picked
     * up crosstalk from BCLK/LRCLK and the capture turned it into bursts of
     * -33 to -90 dBFS -- crackling for 2-3.5 s after every stop (100 ms
     * level trace, 2026-09-24). A DSP like the ADAU1701 has pull-downs on
     * its serial inputs, which is why the module never did this there. Weak
     * enough (~45 kOhm) to be irrelevant while the module drives the line.
     */
    if (gpio_pulldown_en((gpio_num_t)pins.i2s_din) != ESP_OK) {
        ESP_LOGW(TAG, "Pull-down on I2S DIN (GPIO %u) failed", (unsigned)pins.i2s_din);
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
             pins.i2s_bclk,
             pins.i2s_lrclk,
             pins.i2s_din,
             pins.i2s_dout);
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

    /*
     * Minus the TX queue, which delays this speaker again after the line:
     * a frame written here is heard AUDIO_I2S_TX_LATENCY_US later. The
     * clients take the same queue off their own schedule
     * (audio_sink.c: due_local_us - now - AUDIO_I2S_TX_LATENCY_US), so
     * without this the server's speaker played exactly those 40 ms behind
     * every client -- audible between rooms, and reported as "the server
     * lags the clients" (2026-09-20).
     */
    const size_t tx_queue_samples =
        (size_t)(AUDIO_I2S_TX_LATENCY_US * AUDIO_I2S_SAMPLE_RATE / 1000000LL);
    samples = (samples > tx_queue_samples) ? (samples - tx_queue_samples) : 0U;

    /*
     * And minus SERVER_LEAD_MS on top of that. 20 ms of it are explained: a
     * chunk is timestamped at the instant its frame *starts*, and the
     * clients play it that many ms after that instant, while these samples
     * only reach the delay line once the frame has been captured in full.
     *
     * Nothing beyond that. 30, 40 and 50 ms were each tried by ear and all
     * of them sounded worse, so the derived value stands. Tuning further by
     * ear went nowhere, which is no surprise: the clients' drift control
     * moves their playback by more than these steps -- the error swings by
     * tens of ms and a hard resync shifts it in one go -- so the target was
     * never still. Anything more here needs a measurement of when a known
     * marker actually leaves each speaker, not another listening pass.
     * delay_trim_ms is gone from the server, so this is the only handle.
     */
    const size_t lead_samples =
        (size_t)SERVER_LEAD_MS * (AUDIO_I2S_SAMPLE_RATE / 1000U);
    samples = (samples > lead_samples) ? (samples - lead_samples) : 0U;

    if (samples > s_delay_capacity) {
        samples = s_delay_capacity;
    }
    s_delay_samples = samples;

    ESP_LOGI(TAG,
             "Local output delayed by %u ms (%u samples; %u ms TX queue and "
             "%u ms lead taken off)",
             (unsigned)delay_ms, (unsigned)samples,
             (unsigned)(AUDIO_I2S_TX_LATENCY_US / 1000),
             (unsigned)SERVER_LEAD_MS);
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
    float master_target;
    uint32_t dsp_generation;
    portENTER_CRITICAL(&s_dsp_lock);
    lowpass = s_lowpass;
    highpass = s_highpass;
    dsp = s_dsp_params;
    sub_gain_linear = s_sub_gain_linear;
    wideband_gain_linear = s_wideband_gain_linear;
    master_target = s_master_volume_target * s_user_volume;
    dsp_generation = s_dsp_generation;
    portEXIT_CRITICAL(&s_dsp_lock);

    /*
     * Walk the master volume to its new value across the frame instead of
     * applying it to the first sample. A step in gain is a discontinuity
     * in the waveform, and at 50 knob readings per second that would be a
     * click on every one of them.
     */
    float master = s_master_volume_current;
    const float master_step = (mono_samples > 0U)
                                  ? ((master_target - master) / (float)mono_samples)
                                  : 0.0f;

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

        /*
         * Last thing before the samples leave: the knob attenuates what
         * this speaker plays and nothing else. The Opus encoder is fed
         * from a separate copy that never passes through here, so turning
         * one box down leaves every other box untouched.
         */
        master += master_step;
        subwoofer *= master;
        wideband *= master;

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
     * Land exactly on the target rather than keeping the accumulated sum,
     * so repeated ramps cannot drift away from it through rounding.
     */
    s_master_volume_current = master_target;

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

    /*
     * Clock check, counted here rather than on the capture side: both roles
     * write exactly one frame per audio tick, while a client only *reads*
     * when its player loop gets that far. Counting reads made the clients
     * look 1200-1500 ppm slow where the server showed -180, which was the
     * measurement, not the clock (2026-09-20).
     *
     * What it measures: the I2S unit's own rate against esp_timer. Both
     * derive from the same crystal, so a crystal error cancels out and what
     * is left is the divider's error against 48 kHz. Comparing the number
     * between server and clients says whether a standing playback offset is
     * a clock difference -- the drift control can only correct +-200 ppm of
     * one -- or something in the timeline.
     */
    if (s_rate_anchor_us == 0) {
        s_rate_anchor_us = esp_timer_get_time();
        s_rate_samples = 0;
    } else {
        s_rate_samples += (int64_t)mono_samples;
    }

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

int32_t audio_i2s_clock_ppm(void)
{
    const int64_t anchor = s_rate_anchor_us;
    const int64_t samples = s_rate_samples;
    if (anchor == 0 || samples < AUDIO_I2S_SAMPLE_RATE) {
        return 0; /* less than a second of audio, too short to mean much */
    }

    const int64_t elapsed_us = esp_timer_get_time() - anchor;
    if (elapsed_us <= 0) {
        return 0;
    }

    /* How many samples the timer says should have arrived, against how many
     * did. Positive means the I2S clock runs fast. */
    const int64_t expected = elapsed_us * AUDIO_I2S_SAMPLE_RATE / 1000000LL;
    if (expected == 0) {
        return 0;
    }
    return (int32_t)((samples - expected) * 1000000LL / expected);
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

void audio_i2s_set_voice_active(bool active)
{
    portENTER_CRITICAL(&s_voice_lock);
    s_voice_active = active;
    /* Drop any residue on either transition: entering with stale leftovers
     * would play a moment of old audio, leaving without clearing would let
     * that tail bleed into whatever plays next. */
    s_voice_mailbox_fill = 0;
    s_voice_primed = false;
    portEXIT_CRITICAL(&s_voice_lock);
}

void audio_i2s_take_voice_stats(uint32_t *underrun_samples, uint32_t *dropped_samples,
                                uint32_t *wait_avg_ms, uint32_t *wait_max_ms)
{
    portENTER_CRITICAL(&s_voice_lock);
    *wait_avg_ms = (s_voice_wait_count != 0U)
                   ? (s_voice_wait_total_ms / s_voice_wait_count) : 0U;
    *wait_max_ms = s_voice_wait_max_ms;
    s_voice_wait_total_ms = 0;
    s_voice_wait_count = 0;
    s_voice_wait_max_ms = 0;
    *underrun_samples = s_voice_underrun_samples;
    *dropped_samples = s_voice_dropped_samples;
    s_voice_underrun_samples = 0;
    s_voice_dropped_samples = 0;
    portEXIT_CRITICAL(&s_voice_lock);
}

void audio_i2s_feed_voice(const int16_t *mono, size_t mono_samples)
{
    if (mono == NULL || mono_samples == 0U) {
        return;
    }

    const size_t capacity = sizeof(s_voice_mailbox) / sizeof(s_voice_mailbox[0]);
    if (mono_samples > capacity) {
        mono += mono_samples - capacity;
        mono_samples = capacity;
    }

    portENTER_CRITICAL(&s_voice_lock);
    /*
     * How long this audio will sit here before it is played: everything
     * already queued has to go out first, at 48 kHz. Measured because it is
     * the one part of the announcement's delay that is not a constant --
     * the prefill and Wi-Fi jitter both show up in it.
     */
    const uint32_t wait_ms = (uint32_t)(s_voice_mailbox_fill * 1000U / AUDIO_I2S_SAMPLE_RATE);
    s_voice_wait_total_ms += wait_ms;
    ++s_voice_wait_count;
    if (wait_ms > s_voice_wait_max_ms) {
        s_voice_wait_max_ms = wait_ms;
    }

    /*
     * Full: drop the *oldest* samples, not the new ones. Keeping the old
     * would pin the latency at the mailbox's full depth after one burst of
     * late packets and throw away the freshest speech instead.
     */
    if (s_voice_mailbox_fill + mono_samples > capacity) {
        const size_t drop = s_voice_mailbox_fill + mono_samples - capacity;
        memmove(s_voice_mailbox, s_voice_mailbox + drop,
                (s_voice_mailbox_fill - drop) * sizeof(int16_t));
        s_voice_mailbox_fill -= drop;
        s_voice_dropped_samples += (uint32_t)drop;
    }
    memcpy(s_voice_mailbox + s_voice_mailbox_fill, mono, mono_samples * sizeof(int16_t));
    s_voice_mailbox_fill += mono_samples;

    /*
     * Keep the queue from ratcheting up, but only when it stays too full.
     *
     * Feeding and playback run at the same rate, so a burst of late packets
     * raises the fill once and it never comes back down by itself: measured
     * 21 -> 58 ms average within ten seconds of one announcement. Cutting
     * back the instant a burst arrives was worse, though -- it threw away
     * exactly the audio that covers the gap behind the burst (440 ms
     * missing per five seconds). So a burst may stay, and only a backlog
     * that survives VOICE_TRIM_AFTER_FEEDS feeds in a row is cut back to
     * the target. That costs one audible skip, rarely, which is the trade
     * this whole path is built on: drop rather than buffer.
     */
    if (s_voice_mailbox_fill > VOICE_TARGET_FILL_SAMPLES) {
        ++s_voice_above_target;
    } else {
        s_voice_above_target = 0;
    }
    if (s_voice_above_target >= VOICE_TRIM_AFTER_FEEDS) {
        const size_t drop = s_voice_mailbox_fill - VOICE_TARGET_FILL_SAMPLES;
        memmove(s_voice_mailbox, s_voice_mailbox + drop,
                (s_voice_mailbox_fill - drop) * sizeof(int16_t));
        s_voice_mailbox_fill -= drop;
        s_voice_dropped_samples += (uint32_t)drop;
        s_voice_above_target = 0;
    }
    portEXIT_CRITICAL(&s_voice_lock);
}

/* Drains up to mono_samples from the voice mailbox into s_voice_frame,
 * zero-filling anything not yet received -- never waits, matching the
 * announcement path's "drop rather than buffer" design throughout. */
static const int16_t *assemble_voice_frame(size_t mono_samples)
{
    size_t take = 0;

    portENTER_CRITICAL(&s_voice_lock);
    /* Prefill, see render_voice_frame() in audio_sink.c for why. */
    if (!s_voice_primed && s_voice_mailbox_fill >= VOICE_PREFILL_SAMPLES) {
        s_voice_primed = true;
    }
    if (s_voice_primed) {
        take = (s_voice_mailbox_fill < mono_samples) ? s_voice_mailbox_fill : mono_samples;
        memcpy(s_voice_frame, s_voice_mailbox, take * sizeof(int16_t));
        if (take < s_voice_mailbox_fill) {
            memmove(s_voice_mailbox, s_voice_mailbox + take,
                   (s_voice_mailbox_fill - take) * sizeof(int16_t));
        }
        s_voice_mailbox_fill -= take;
        if (take < mono_samples) {
            s_voice_primed = false;
            s_voice_underrun_samples += (uint32_t)(mono_samples - take);
        }
    }
    portEXIT_CRITICAL(&s_voice_lock);

    if (take < mono_samples) {
        memset(s_voice_frame + take, 0, (mono_samples - take) * sizeof(int16_t));
    }
    return s_voice_frame;
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

    /*
     * A voice announcement overrides the local speaker only, after the
     * delay line -- it is meant to play at the lowest latency this device
     * can manage, not lined up with clients bufferMs later. Capture and the
     * Opus path above are completely unaffected by this.
     */
    if (s_voice_active) {
        output = assemble_voice_frame(mono_samples);
    }

    return apply_dsp_and_output(output, mono_samples);
}
