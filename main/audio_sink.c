/**
 * @file audio_sink.c
 * @brief Client-role source arbitration, ring buffer and player task.
 */
#include "audio_sink.h"

#include <math.h>
#include <string.h>

#include "audio_i2s.h"
#include "device_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO_SINK";

#define PLAYER_TASK_STACK   4096
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

static volatile bool s_network_active;
static volatile uint8_t s_source_mode = SOURCE_MODE_AUTO;
static volatile int8_t s_local_threshold_db = -40;
static volatile int16_t s_delay_trim_ms;

static volatile audio_sink_source_t s_active_source = AUDIO_SINK_SOURCE_NONE;

static bool s_local_signal_present;
static uint32_t s_local_attack_count;
static int64_t s_local_last_active_us;

static uint64_t s_network_bytes_fed;
static uint64_t s_network_bytes_dropped;
static uint64_t s_network_underrun_samples;
static int64_t s_last_stats_us;

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
    xSemaphoreGive(ring->lock);
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

static audio_sink_source_t decide_source(void)
{
    const uint8_t mode = s_source_mode;

    if (mode == SOURCE_MODE_NETWORK_ONLY) {
        return s_network_active ? AUDIO_SINK_SOURCE_NETWORK : AUDIO_SINK_SOURCE_NONE;
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
    if (s_network_active) {
        return AUDIO_SINK_SOURCE_NETWORK;
    }
    return AUDIO_SINK_SOURCE_NONE;
}

static void maybe_log_stats(void)
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
             "src=%d ring=%u/%u B fed=%llu B dropped=%llu B underrun=%llu samples",
             (int)s_active_source,
             (unsigned)fill,
             (unsigned)capacity,
             (unsigned long long)s_network_bytes_fed,
             (unsigned long long)s_network_bytes_dropped,
             (unsigned long long)s_network_underrun_samples);

    s_network_bytes_fed = 0;
    s_network_bytes_dropped = 0;
    s_network_underrun_samples = 0;
    s_last_stats_us = now;
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

        update_local_signal_state(have_local
                                       ? rms_dbfs(local_mono, AUDIO_SINK_FRAME_SAMPLES)
                                       : -120.0f);

        const audio_sink_source_t desired = decide_source();
        if (desired != s_active_source) {
            ESP_LOGI(TAG, "Switching source %d -> %d", (int)s_active_source, (int)desired);
            /*
             * Only the network path is ever queued ahead of time (see
             * audio_sink_feed_network()'s drop-when-inactive rule below), so
             * flushing here just discards the handful of frames that arrived
             * in the brief window before this switch took effect -- never a
             * multi-second backlog.
             */
            ring_flush(&s_ring);
            s_active_source = desired;
        }

        const int16_t *chosen;
        if (s_active_source == AUDIO_SINK_SOURCE_LOCAL_INPUT && have_local) {
            chosen = local_mono;
        } else if (s_active_source == AUDIO_SINK_SOURCE_NETWORK) {
            const size_t want_bytes = AUDIO_SINK_FRAME_SAMPLES * sizeof(int16_t);
            const size_t got_bytes =
                ring_read(&s_ring, (uint8_t *)playout_mono, want_bytes);
            if (got_bytes < want_bytes) {
                const size_t got_samples = got_bytes / sizeof(int16_t);
                memset(playout_mono + got_samples, 0, want_bytes - got_bytes);
                s_network_underrun_samples += AUDIO_SINK_FRAME_SAMPLES - got_samples;
            }
            chosen = playout_mono;
        } else {
            memset(playout_mono, 0, sizeof(playout_mono));
            chosen = playout_mono;
        }

        audio_i2s_write_mono(chosen, AUDIO_SINK_FRAME_SAMPLES);
        maybe_log_stats();
    }
}

esp_err_t audio_sink_start(uint16_t buffer_ms)
{
    if (s_started) {
        return ESP_OK;
    }

    const size_t bytes_per_ms = (AUDIO_I2S_SAMPLE_RATE / 1000U) * sizeof(int16_t);
    const size_t capacity = (size_t)buffer_ms * bytes_per_ms;

    esp_err_t result = ring_init(&s_ring, capacity);
    if (result != ESP_OK) {
        return result;
    }

    s_active_source = AUDIO_SINK_SOURCE_NONE;
    s_local_signal_present = false;
    s_local_attack_count = 0;
    s_local_last_active_us = 0;

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
    ESP_LOGI(TAG, "Playback ring buffer ready: %u ms, %u B", buffer_ms, (unsigned)capacity);
    return ESP_OK;
}

void audio_sink_set_network_active(bool active)
{
    s_network_active = active;
}

size_t audio_sink_feed_network(const int16_t *mono_pcm, size_t sample_count)
{
    if (!s_started || s_active_source != AUDIO_SINK_SOURCE_NETWORK) {
        s_network_bytes_dropped += sample_count * sizeof(int16_t);
        return 0;
    }

    const size_t written =
        ring_write(&s_ring, (const uint8_t *)mono_pcm, sample_count * sizeof(int16_t));
    s_network_bytes_fed += written;
    if (written < sample_count * sizeof(int16_t)) {
        s_network_bytes_dropped += (sample_count * sizeof(int16_t)) - written;
    }
    return written / sizeof(int16_t);
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
