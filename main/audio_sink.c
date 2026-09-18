/**
 * @file audio_sink.c
 * @brief Client-role source arbitration, ring buffer, playback scheduler
 *        and player task.
 */
#include "audio_sink.h"

#include <math.h>
#include <string.h>

#include "audio_i2s.h"
#include "audio_resample.h"
#include "device_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO_SINK";

#define PLAYER_TASK_STACK   8192
#define PLAYER_TASK_PRIORITY   5
#define PLAYER_TASK_CORE       1

/*
 * How long the local input must stay below the threshold before AUTO mode
 * releases it back to the network -- avoids flapping on a brief dropout or
 * on program material with quiet passages. Attack (a signal counts as
 * "present") is intentionally fast: two consecutive frames, ~40 ms.
 */
#define LOCAL_SIGNAL_RELEASE_HOLD_US (800LL * 1000LL)
#define LOCAL_SIGNAL_ATTACK_FRAMES 2

#define STATS_INTERVAL_US (5LL * 1000000LL)

/*
 * Percentage of the target fill (== buffer_ms worth of audio) that must be
 * buffered before playback switches onto the network source. Without this
 * gate the ring only ever holds a few tens of ms in practice -- playback
 * starts the instant any data arrives and drains it about as fast as it
 * fills, so the cushion meant to absorb mesh rearrangements/jitter never
 * accumulates and every minor timing hiccup is an audible underrun. Once
 * already playing from the network, transient dips below this level do not
 * re-trigger prebuffering -- only a real drop out of the network source
 * (disconnect, or local input pre-empting it) does.
 */
#define NETWORK_PREBUFFER_PERCENT 80U

/*
 * The ring is sized well above buffer_ms rather than at it: buffer_ms is
 * the *steady-state* occupancy (the server timestamps a chunk at capture
 * time and the client plays it buffer_ms later, so that much audio is
 * in flight permanently), not a maximum. A ring of exactly buffer_ms would
 * sit at its limit continuously and drop whatever arrives while the
 * scheduler is holding playback back.
 */
#define RING_CAPACITY_FACTOR 2U

/*
 * A chunk timestamp that misses the buffered stream's continuation by more
 * than this is a discontinuity. The buffered audio is kept and only the
 * timeline follows the shift -- the usual cause is the *server* correcting
 * its own capture timeline (TIMESTAMP_RESYNC_THRESHOLD_US in audio_i2s.c),
 * which relabels its timestamps without interrupting the audio itself.
 * Measured on device: with four clients the server's capture loop ran
 * ~450 us per frame behind, hit its 100 ms threshold every ~4.5 s and
 * re-anchored. Dropping the buffer for that meant re-prebuffering 2.4 s
 * every 4.5 s, i.e. near-permanent silence.
 */
#define STREAM_DISCONTINUITY_US (100LL * 1000LL)

/*
 * Beyond this the stream is treated as a genuinely new one and the buffer
 * is dropped: no plausible capture re-anchor is this large, whereas the
 * server adopting a PC/Android client's wall clock mid-session moves its
 * timestamps by months (see handle_time() in snapserver.c). Reconnects
 * don't rely on this -- they already flush when the source leaves
 * AUDIO_SINK_SOURCE_NETWORK.
 */
#define STREAM_RESTART_US (1000LL * 1000LL)

/*
 * Above this scheduling error, correcting by resampling would take far too
 * long (200 ppm moves 200 us per second), so the timeline is realigned in
 * one step instead: skip ahead when late, hold silence when early.
 */
#define HARD_RESYNC_THRESHOLD_US (100LL * 1000LL)

/*
 * PI gains for the fine correction, in ppm per us of error and ppm per
 * (us * s) of accumulated error. Deliberately slow: a 10 ms error asks for
 * ~15 ppm, which is inaudible and still closes that gap in well under a
 * minute. The integral term only exists to cancel the constant crystal
 * offset between the two boards (+-20 ppm each).
 */
#define CONTROL_KP 0.0015f
#define CONTROL_KI 0.00002f
#define CONTROL_INTEGRAL_CLAMP_PPM 100.0f
/* Maximum ppm change per 20 ms frame, so corrections ramp instead of step. */
#define CONTROL_SLEW_PPM_PER_FRAME 5.0f

/* Worst-case input samples for one output frame at +AUDIO_RESAMPLE_MAX_PPM,
 * plus the interpolation lookahead and a little slack. */
#define STAGE_CAPACITY (AUDIO_SINK_FRAME_SAMPLES + 16U)

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t write_pos;
    size_t read_pos;
    size_t fill;
    SemaphoreHandle_t lock;
} byte_ring_t;

static byte_ring_t s_ring;
static bool s_started;

/*
 * buffer_ms worth of bytes: what the prebuffer gate aims for and roughly
 * where the scheduler keeps the fill in steady state. Derived from the
 * local config at start (the ring is sized from it); the scheduler itself
 * uses the server-announced bufferMs, which is the same value whenever
 * both ends are configured alike.
 */
static size_t s_target_fill_bytes;

static volatile bool s_network_active;
static volatile uint8_t s_source_mode = SOURCE_MODE_AUTO;
static volatile int8_t s_local_threshold_db = -40;
static volatile int16_t s_delay_trim_ms;

static volatile audio_sink_source_t s_active_source = AUDIO_SINK_SOURCE_NONE;
/*
 * True once the ring has reached NETWORK_PREBUFFER_PERCENT during the
 * current network-active session, i.e. playback is allowed to (keep)
 * running from the network source without re-buffering first. Reset
 * whenever the network is no longer active, so a fresh connection
 * re-buffers from scratch instead of starting on whatever few bytes have
 * trickled in so far.
 */
static bool s_network_ready;

static bool s_local_signal_present;
static uint32_t s_local_attack_count;
static int64_t s_local_last_active_us;

static uint64_t s_network_bytes_fed;
static uint64_t s_network_bytes_dropped;
static uint64_t s_network_underrun_samples;
static int64_t s_last_stats_us;

/*
 * Playback timeline. s_head_ts_us is the server-clock timestamp of the
 * sample currently at the ring's read position; it is anchored from a
 * WireChunk timestamp and then advanced by exactly the number of samples
 * consumed (s_head_ts_remainder carries the sub-microsecond rest of the
 * 1e6/48000 division so this cannot drift). Guarded by the ring lock.
 */
static int64_t s_head_ts_us;
static int64_t s_head_ts_remainder;
static bool s_head_ts_valid;

/* server_clock - local esp_timer clock, from snapclient.c's time sync. */
static volatile int64_t s_server_offset_us;
static volatile bool s_server_offset_valid;

/* From ServerSettings; defaults match the server's own seed values until
 * the real message arrives. */
static volatile int64_t s_stream_buffer_us = 3000000;
static volatile int64_t s_stream_latency_us;

/*
 * Output gain from the server's per-client volume, as a ready multiplier so
 * the hot path does no arithmetic beyond the multiply. Deliberately a
 * single value rather than a gain plus a "bypass" flag: it is written from
 * the snapclient task and read from the player task, and one variable
 * cannot be read half-updated.
 */
static volatile float s_volume_gain = 1.0f;

static audio_resample_t s_resample;
static float s_control_integral;
static float s_control_ppm;
static int16_t s_stage[STAGE_CAPACITY];
static size_t s_stage_count;

/*
 * Set by the feeding task when it re-anchors the timeline; the player task
 * owns s_stage/s_resample/the PI state and clears them when it sees this,
 * so neither buffer is ever touched from two tasks at once.
 */
static volatile bool s_timeline_reset_pending;

/* Diagnostics for the periodic stats line. */
static int64_t s_last_error_us;
static uint32_t s_resync_count;
static uint64_t s_discontinuity_count;
static uint64_t s_timeline_shift_count;

/*
 * Loudest captured local-input frame within the current stats window,
 * recorded on every frame regardless of which source is playing. Without
 * this the local input's level is only visible once it has already won the
 * arbitration, which makes "the source delivers nothing" and "the detector
 * never triggers" impossible to tell apart.
 */
static float s_local_peak_db = -120.0f;

static esp_err_t ring_init(byte_ring_t *ring, size_t capacity)
{
    ring->data = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ring->data == NULL) {
        ring->data = heap_caps_malloc(capacity, MALLOC_CAP_8BIT);
    }
    if (ring->data == NULL) {
        ESP_LOGE(TAG, "No memory for %u B playback ring buffer", (unsigned)capacity);
        return ESP_ERR_NO_MEM;
    }

    ring->capacity = capacity;
    ring->write_pos = 0;
    ring->read_pos = 0;
    ring->fill = 0;
    ring->lock = xSemaphoreCreateMutex();
    if (ring->lock == NULL) {
        heap_caps_free(ring->data);
        ring->data = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* Returns bytes actually written; short on overflow rather than wrapping
 * over unread data. */
static size_t ring_write(byte_ring_t *ring, const uint8_t *data, size_t len)
{
    xSemaphoreTake(ring->lock, portMAX_DELAY);

    const size_t space = ring->capacity - ring->fill;
    const size_t to_write = (len < space) ? len : space;

    for (size_t i = 0; i < to_write; ++i) {
        ring->data[ring->write_pos] = data[i];
        ring->write_pos = (ring->write_pos + 1U) % ring->capacity;
    }
    ring->fill += to_write;

    xSemaphoreGive(ring->lock);
    return to_write;
}

/* Returns bytes actually read; short if the ring holds less than len. */
static size_t ring_read(byte_ring_t *ring, uint8_t *out, size_t len)
{
    xSemaphoreTake(ring->lock, portMAX_DELAY);

    const size_t to_read = (len < ring->fill) ? len : ring->fill;
    for (size_t i = 0; i < to_read; ++i) {
        out[i] = ring->data[ring->read_pos];
        ring->read_pos = (ring->read_pos + 1U) % ring->capacity;
    }
    ring->fill -= to_read;

    xSemaphoreGive(ring->lock);
    return to_read;
}

static void ring_flush(byte_ring_t *ring)
{
    xSemaphoreTake(ring->lock, portMAX_DELAY);
    ring->write_pos = 0;
    ring->read_pos = 0;
    ring->fill = 0;
    s_head_ts_valid = false;
    xSemaphoreGive(ring->lock);
}

static int64_t samples_to_us(int64_t samples)
{
    return (samples * 1000000LL) / AUDIO_I2S_SAMPLE_RATE;
}

static int64_t us_to_samples(int64_t microseconds)
{
    return (microseconds * AUDIO_I2S_SAMPLE_RATE) / 1000000LL;
}

/* Caller must hold the ring lock. */
static void timeline_advance_locked(size_t samples)
{
    const int64_t numerator =
        (int64_t)samples * 1000000LL + s_head_ts_remainder;
    s_head_ts_us += numerator / AUDIO_I2S_SAMPLE_RATE;
    s_head_ts_remainder = numerator % AUDIO_I2S_SAMPLE_RATE;
}

/* Caller must hold the ring lock. */
static void timeline_anchor_locked(int64_t chunk_ts_us)
{
    s_head_ts_us = chunk_ts_us;
    s_head_ts_remainder = 0;
    s_head_ts_valid = true;
}

static float rms_dbfs(const int16_t *samples, size_t count)
{
    if (count == 0U) {
        return -120.0f;
    }

    double sum_sq = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double s = samples[i] / 32768.0;
        sum_sq += s * s;
    }

    const double rms = sqrt(sum_sq / (double)count);
    if (rms < 1e-6) {
        return -120.0f;
    }
    return (float)(20.0 * log10(rms));
}

static void update_local_signal_state(float rms_db)
{
    const int64_t now = esp_timer_get_time();

    if (rms_db >= (float)s_local_threshold_db) {
        s_local_last_active_us = now;
        if (!s_local_signal_present) {
            if (++s_local_attack_count >= LOCAL_SIGNAL_ATTACK_FRAMES) {
                s_local_signal_present = true;
                s_local_attack_count = 0;
            }
        }
        return;
    }

    s_local_attack_count = 0;
    if (s_local_signal_present &&
        (now - s_local_last_active_us) > LOCAL_SIGNAL_RELEASE_HOLD_US) {
        s_local_signal_present = false;
    }
}

/*
 * Network source, gated by NETWORK_PREBUFFER_PERCENT: once s_active_source
 * is already NETWORK this just keeps it there (no re-buffering on a
 * transient dip), otherwise it only switches in once the ring has actually
 * accumulated a real cushion. Falls back to NONE (silence) while waiting or
 * while the network isn't active at all.
 */
static audio_sink_source_t network_source_or_none(void)
{
    if (!s_network_active) {
        s_network_ready = false;
        return AUDIO_SINK_SOURCE_NONE;
    }
    if (s_active_source == AUDIO_SINK_SOURCE_NETWORK) {
        return AUDIO_SINK_SOURCE_NETWORK;
    }
    if (!s_network_ready) {
        xSemaphoreTake(s_ring.lock, portMAX_DELAY);
        const size_t fill = s_ring.fill;
        xSemaphoreGive(s_ring.lock);
        if (fill * 100U < s_target_fill_bytes * NETWORK_PREBUFFER_PERCENT) {
            return AUDIO_SINK_SOURCE_NONE;
        }
        s_network_ready = true;
    }
    return AUDIO_SINK_SOURCE_NETWORK;
}

static audio_sink_source_t decide_source(void)
{
    const uint8_t mode = s_source_mode;

    if (mode == SOURCE_MODE_NETWORK_ONLY) {
        return network_source_or_none();
    }
    if (mode == SOURCE_MODE_LOCAL_ONLY) {
        return AUDIO_SINK_SOURCE_LOCAL_INPUT;
    }

    /* SOURCE_MODE_AUTO: local input pre-empts the network stream whenever a
     * signal is present, same rule the ESP32_Mesh_Snapclient reference used
     * for A2DP vs. Snapcast. */
    if (s_local_signal_present) {
        return AUDIO_SINK_SOURCE_LOCAL_INPUT;
    }
    return network_source_or_none();
}

/*
 * Temporary diagnostic (2026-09-17): reports the RMS level of the samples
 * actually handed to audio_i2s_write_mono() every frame, so we can tell from
 * the log alone whether real signal is reaching the DSP/output stage on the
 * client -- as opposed to fed/dropped/underrun, which only ever prove bytes
 * moved through the ring, not that they carried audio.
 */
static void maybe_log_stats(float output_rms_db)
{
    const int64_t now = esp_timer_get_time();
    if (s_last_stats_us == 0) {
        s_last_stats_us = now;
        return;
    }
    if ((now - s_last_stats_us) < STATS_INTERVAL_US) {
        return;
    }

    xSemaphoreTake(s_ring.lock, portMAX_DELAY);
    const size_t fill = s_ring.fill;
    const size_t capacity = s_ring.capacity;
    xSemaphoreGive(s_ring.lock);

    ESP_LOGI(TAG,
             "src=%d ring=%u/%u B fed=%llu B dropped=%llu B underrun=%llu samples "
             "output_rms=%.1f dBFS local_peak=%.1f dBFS(thr %d) sync=%s err=%lld us "
             "ppm=%d resync=%lu disc=%llu shift=%llu",
             (int)s_active_source,
             (unsigned)fill,
             (unsigned)capacity,
             (unsigned long long)s_network_bytes_fed,
             (unsigned long long)s_network_bytes_dropped,
             (unsigned long long)s_network_underrun_samples,
             (double)output_rms_db,
             (double)s_local_peak_db,
             (int)s_local_threshold_db,
             s_server_offset_valid ? "yes" : "no",
             (long long)s_last_error_us,
             (int)audio_resample_get_ppm(&s_resample),
             (unsigned long)s_resync_count,
             (unsigned long long)s_discontinuity_count,
             (unsigned long long)s_timeline_shift_count);

    s_network_bytes_fed = 0;
    s_network_bytes_dropped = 0;
    s_network_underrun_samples = 0;
    s_local_peak_db = -120.0f;
    s_last_stats_us = now;
}

/*
 * Scheduling error of the sample at the head of the playback timeline:
 * positive means it is due later than it would actually be heard (too
 * early, hold), negative means it is overdue (too late, catch up).
 * Returns false while there is nothing to schedule against -- no clock
 * sync yet, or no anchored stream -- in which case the caller just plays
 * the ring buffer out at nominal rate without scheduling.
 */
static bool scheduling_error_us(int64_t *error_us)
{
    if (!s_server_offset_valid) {
        return false;
    }

    xSemaphoreTake(s_ring.lock, portMAX_DELAY);
    const bool anchored = s_head_ts_valid;
    const int64_t due_local_us = (s_head_ts_us - s_server_offset_us) +
                                 s_stream_buffer_us - s_stream_latency_us +
                                 (int64_t)s_delay_trim_ms * 1000LL;
    xSemaphoreGive(s_ring.lock);

    if (!anchored) {
        return false;
    }

    *error_us = due_local_us - (esp_timer_get_time() + AUDIO_I2S_TX_LATENCY_US);
    return true;
}

/*
 * Full restart of the playback pipeline: drops the staged samples and the
 * resampler phase along with the controller. Only for real discontinuities
 * (new stream, hard resync, source change) -- doing this per frame would
 * throw away the staging remainder every time and swallow a sample.
 */
static void control_reset(void)
{
    audio_resample_init(&s_resample);
    s_control_integral = 0.0f;
    s_control_ppm = 0.0f;
    s_stage_count = 0;
}

/*
 * Stops correcting without disturbing the stream: used while there is
 * nothing to schedule against, where playback should simply pass through
 * at nominal rate.
 */
static void control_neutral(void)
{
    s_control_integral = 0.0f;
    s_control_ppm = 0.0f;
    audio_resample_set_ppm(&s_resample, 0);
}

/*
 * Late by -error_us: discard that much audio in one step instead of
 * waiting for a 200 ppm correction to eat it. The samples already staged
 * in front of the ring go with it, so they have to move the timeline too.
 */
static void timeline_resync_late(int64_t error_us)
{
    const size_t staged = s_stage_count;
    const size_t wanted = (size_t)us_to_samples(-error_us);

    xSemaphoreTake(s_ring.lock, portMAX_DELAY);
    const size_t available = s_ring.fill / sizeof(int16_t);
    const size_t skipped = (wanted < available) ? wanted : available;
    s_ring.read_pos = (s_ring.read_pos + skipped * sizeof(int16_t)) % s_ring.capacity;
    s_ring.fill -= skipped * sizeof(int16_t);
    timeline_advance_locked(staged + skipped);
    xSemaphoreGive(s_ring.lock);

    control_reset();
}

/*
 * PI correction inside the fine-control band. The output drives the
 * resampler ratio: playing slightly fast (positive ppm) eats the backlog
 * when late, slightly slow stretches it when early.
 */
static void control_update(int64_t error_us)
{
    const float error = (float)error_us;
    const float frame_seconds =
        (float)AUDIO_SINK_FRAME_SAMPLES / (float)AUDIO_I2S_SAMPLE_RATE;

    s_control_integral += error * frame_seconds;

    const float integral_limit = CONTROL_INTEGRAL_CLAMP_PPM / CONTROL_KI;
    if (s_control_integral > integral_limit) {
        s_control_integral = integral_limit;
    } else if (s_control_integral < -integral_limit) {
        s_control_integral = -integral_limit;
    }

    float target_ppm = -(CONTROL_KP * error + CONTROL_KI * s_control_integral);
    if (target_ppm > (float)AUDIO_RESAMPLE_MAX_PPM) {
        target_ppm = (float)AUDIO_RESAMPLE_MAX_PPM;
    } else if (target_ppm < -(float)AUDIO_RESAMPLE_MAX_PPM) {
        target_ppm = -(float)AUDIO_RESAMPLE_MAX_PPM;
    }

    const float delta = target_ppm - s_control_ppm;
    if (delta > CONTROL_SLEW_PPM_PER_FRAME) {
        s_control_ppm += CONTROL_SLEW_PPM_PER_FRAME;
    } else if (delta < -CONTROL_SLEW_PPM_PER_FRAME) {
        s_control_ppm -= CONTROL_SLEW_PPM_PER_FRAME;
    } else {
        s_control_ppm = target_ppm;
    }

    audio_resample_set_ppm(&s_resample, (int32_t)s_control_ppm);
}

/*
 * Produces one output frame from the network stream: applies the
 * scheduler, then pulls the (resampling-dependent) number of input samples
 * through the staging buffer. Returns false when the frame should be
 * silence instead -- either because the head sample is not due yet or
 * because nothing is buffered.
 */
static bool render_network_frame(int16_t *playout_mono)
{
    if (s_timeline_reset_pending) {
        s_timeline_reset_pending = false;
        control_reset();
    }

    int64_t error_us = 0;
    static bool holding;

    if (scheduling_error_us(&error_us)) {
        s_last_error_us = error_us;

        /*
         * Too early: emit silence and keep everything buffered. Each held
         * frame costs 20 ms of real time, so the schedule catches up on its
         * own without discarding audio.
         *
         * Entering and leaving this state use different thresholds on
         * purpose. Releasing at HARD_RESYNC_THRESHOLD_US would hand the
         * fine controller a residual error of a full threshold width, and
         * at 200 ppm it needs ~400 s to work off 80 ms -- measured on
         * device: err sat at 72-100 ms while ppm pinned at its -200 limit.
         * Holding until the error is actually gone starts playback on time
         * and leaves the controller nothing but real crystal drift to do.
         */
        if (error_us > HARD_RESYNC_THRESHOLD_US || (holding && error_us > 0)) {
            if (!holding) {
                holding = true;
                s_resync_count++;
            }
            return false;
        }
        holding = false;

        if (error_us < -HARD_RESYNC_THRESHOLD_US) {
            timeline_resync_late(error_us);
            s_resync_count++;
        } else {
            control_update(error_us);
        }
    } else {
        holding = false;
        control_neutral();
    }

    const size_t needed =
        audio_resample_input_needed(&s_resample, AUDIO_SINK_FRAME_SAMPLES);
    if (needed > STAGE_CAPACITY) {
        return false;
    }

    if (s_stage_count < needed) {
        const size_t want = needed - s_stage_count;
        const size_t got_bytes = ring_read(&s_ring,
                                           (uint8_t *)(s_stage + s_stage_count),
                                           want * sizeof(int16_t));
        const size_t got = got_bytes / sizeof(int16_t);
        s_stage_count += got;

        if (s_stage_count < needed) {
            const size_t missing = needed - s_stage_count;
            memset(s_stage + s_stage_count, 0, missing * sizeof(int16_t));
            s_network_underrun_samples += missing;
            s_stage_count = needed;
        }
    }

    const size_t consumed =
        audio_resample_process(&s_resample, s_stage, playout_mono, AUDIO_SINK_FRAME_SAMPLES);

    /*
     * The timeline follows what actually leaves the resampler, not what is
     * pulled out of the ring: the staging buffer sits in front of the ring,
     * so anchoring on ring reads would make the head jump a whole frame
     * ahead every time staging is refilled.
     */
    xSemaphoreTake(s_ring.lock, portMAX_DELAY);
    timeline_advance_locked(consumed);
    xSemaphoreGive(s_ring.lock);

    if (consumed < s_stage_count) {
        memmove(s_stage, s_stage + consumed, (s_stage_count - consumed) * sizeof(int16_t));
        s_stage_count -= consumed;
    } else {
        s_stage_count = 0;
    }

    return true;
}

static void player_task(void *arg)
{
    (void)arg;

    int16_t local_mono[AUDIO_SINK_FRAME_SAMPLES];
    int16_t playout_mono[AUDIO_SINK_FRAME_SAMPLES];

    for (;;) {
        const esp_err_t capture_result =
            audio_i2s_capture_mono(local_mono, AUDIO_SINK_FRAME_SAMPLES);
        const bool have_local = (capture_result == ESP_OK);

        const float local_db = have_local
                                   ? rms_dbfs(local_mono, AUDIO_SINK_FRAME_SAMPLES)
                                   : -120.0f;
        if (local_db > s_local_peak_db) {
            s_local_peak_db = local_db;
        }
        update_local_signal_state(local_db);

        const audio_sink_source_t desired = decide_source();
        if (desired != s_active_source) {
            ESP_LOGI(TAG, "Switching source %d -> %d", (int)s_active_source, (int)desired);
            /*
             * Only flush when *leaving* the network source: its queued
             * audio is now stale (there was a gap while something else
             * played) and must not be played back out of order. Do NOT
             * flush when *entering* it -- that ring content is exactly the
             * prebuffer network_source_or_none() just required before
             * allowing this switch, and discarding it here would
             * immediately re-empty the ring and undo the whole point of
             * prebuffering.
             */
            if (s_active_source == AUDIO_SINK_SOURCE_NETWORK) {
                ring_flush(&s_ring);
                control_reset();
            }
            s_active_source = desired;
        }

        const int16_t *chosen;
        if (s_active_source == AUDIO_SINK_SOURCE_LOCAL_INPUT && have_local) {
            chosen = local_mono;
        } else if (s_active_source == AUDIO_SINK_SOURCE_NETWORK) {
            if (!render_network_frame(playout_mono)) {
                memset(playout_mono, 0, sizeof(playout_mono));
            }
            chosen = playout_mono;
        } else {
            memset(playout_mono, 0, sizeof(playout_mono));
            chosen = playout_mono;
        }

        /*
         * Volume last, after the source has been chosen: applying it
         * earlier would bake the level into the ring buffer, and a change
         * would only become audible buffer_ms later. Writing into
         * playout_mono is safe for either source -- it is this task's own
         * buffer, and in the local-input case it is otherwise unused.
         */
        const float gain = s_volume_gain;
        if (gain < 1.0f) {
            /* Attenuation only -- the mapping below never exceeds 1.0, so
             * the result cannot leave int16 range and needs no clipping. */
            for (size_t i = 0; i < AUDIO_SINK_FRAME_SAMPLES; ++i) {
                playout_mono[i] = (int16_t)lrintf((float)chosen[i] * gain);
            }
            chosen = playout_mono;
        }

        audio_i2s_write_mono(chosen, AUDIO_SINK_FRAME_SAMPLES);
        maybe_log_stats(rms_dbfs(chosen, AUDIO_SINK_FRAME_SAMPLES));
    }
}

esp_err_t audio_sink_start(uint16_t buffer_ms)
{
    if (s_started) {
        return ESP_OK;
    }

    const size_t bytes_per_ms = (AUDIO_I2S_SAMPLE_RATE / 1000U) * sizeof(int16_t);
    s_target_fill_bytes = (size_t)buffer_ms * bytes_per_ms;
    const size_t capacity = s_target_fill_bytes * RING_CAPACITY_FACTOR;

    esp_err_t result = ring_init(&s_ring, capacity);
    if (result != ESP_OK) {
        return result;
    }

    s_active_source = AUDIO_SINK_SOURCE_NONE;
    s_local_signal_present = false;
    s_local_attack_count = 0;
    s_local_last_active_us = 0;
    control_reset();

    if (xTaskCreatePinnedToCore(player_task,
                                "audio_sink",
                                PLAYER_TASK_STACK,
                                NULL,
                                PLAYER_TASK_PRIORITY,
                                NULL,
                                PLAYER_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Creating player task failed");
        heap_caps_free(s_ring.data);
        s_ring.data = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "Playback ring buffer ready: target %u ms (%u B), capacity %u B",
             buffer_ms, (unsigned)s_target_fill_bytes, (unsigned)capacity);
    return ESP_OK;
}

void audio_sink_set_network_active(bool active)
{
    s_network_active = active;
}

size_t audio_sink_feed_network(const int16_t *mono_pcm,
                               size_t sample_count,
                               int64_t chunk_ts_us)
{
    /*
     * Accepted while playing from the network AND while merely prebuffering
     * for it (source is NONE, network active, not yet past
     * NETWORK_PREBUFFER_PERCENT -- see network_source_or_none()): the ring
     * must be allowed to fill during that wait, or prebuffering could never
     * complete. Only actually dropped while local input is the active
     * source, matching the original source-arbitration intent of this
     * check.
     */
    if (!s_started || s_active_source == AUDIO_SINK_SOURCE_LOCAL_INPUT) {
        s_network_bytes_dropped += sample_count * sizeof(int16_t);
        return 0;
    }

    /*
     * Anchor or re-anchor the playback timeline before the data lands.
     * Only this task ever writes to the ring, so releasing the lock between
     * the check and the ring_write() below cannot let another writer slip
     * in and invalidate the decision.
     */
    xSemaphoreTake(s_ring.lock, portMAX_DELAY);
    if (!s_head_ts_valid || s_ring.fill == 0U) {
        timeline_anchor_locked(chunk_ts_us);
    } else {
        /*
         * Ignores the one or two samples the player may still hold staged
         * in front of the ring (~40 us) -- irrelevant against a 100 ms
         * threshold, and reading its counter from this task would be a
         * cross-task read for no benefit.
         */
        const int64_t expected_us =
            s_head_ts_us + samples_to_us((int64_t)(s_ring.fill / sizeof(int16_t)));
        const int64_t gap_us = chunk_ts_us - expected_us;
        if (gap_us > STREAM_RESTART_US || gap_us < -STREAM_RESTART_US) {
            s_ring.write_pos = 0;
            s_ring.read_pos = 0;
            s_ring.fill = 0;
            timeline_anchor_locked(chunk_ts_us);
            s_network_ready = false; /* rebuild the prebuffer from scratch */
            s_timeline_reset_pending = true;
            s_discontinuity_count++;
        } else if (gap_us > STREAM_DISCONTINUITY_US || gap_us < -STREAM_DISCONTINUITY_US) {
            /*
             * Follow the shift and keep everything buffered: the audio is
             * still continuous, only its labelling moved. The scheduler
             * sees the error change by gap_us and corrects that much once,
             * instead of the buffer being thrown away and rebuilt.
             */
            s_head_ts_us += gap_us;
            s_timeline_shift_count++;
        }
    }
    xSemaphoreGive(s_ring.lock);

    const size_t written =
        ring_write(&s_ring, (const uint8_t *)mono_pcm, sample_count * sizeof(int16_t));
    s_network_bytes_fed += written;
    if (written < sample_count * sizeof(int16_t)) {
        s_network_bytes_dropped += (sample_count * sizeof(int16_t)) - written;
    }
    return written / sizeof(int16_t);
}

void audio_sink_set_server_time_offset(int64_t offset_us, bool valid)
{
    s_server_offset_us = offset_us;
    s_server_offset_valid = valid;
}

void audio_sink_set_stream_timing(uint32_t buffer_ms, int32_t latency_ms)
{
    s_stream_buffer_us = (int64_t)buffer_ms * 1000LL;
    s_stream_latency_us = (int64_t)latency_ms * 1000LL;
}

void audio_sink_set_volume(int32_t percent, bool muted)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }

    /*
     * Cubic mapping rather than a straight percentage: a linear amplitude
     * scale crowds almost the whole audible range into the top of the
     * control, so half volume would barely sound quieter. The exponent is
     * the usual cheap approximation of perceived loudness and is easy to
     * swap if it turns out too steep in practice.
     */
    const float fraction = (float)percent / 100.0f;
    const float gain = muted ? 0.0f : (fraction * fraction * fraction);

    s_volume_gain = gain;

    ESP_LOGI(TAG, "Volume now %ld%%%s (gain %.3f)",
             (long)percent, muted ? " (muted)" : "", (double)gain);
}

void audio_sink_set_source_mode(uint8_t mode)
{
    s_source_mode = mode;
}

void audio_sink_set_local_input_threshold_db(int8_t threshold_db)
{
    s_local_threshold_db = threshold_db;
}

void audio_sink_set_delay_trim_ms(int16_t delay_trim_ms)
{
    s_delay_trim_ms = delay_trim_ms;
}

audio_sink_source_t audio_sink_current_source(void)
{
    return s_active_source;
}
