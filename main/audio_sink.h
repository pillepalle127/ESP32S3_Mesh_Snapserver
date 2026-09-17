/**
 * @file audio_sink.h
 * @brief Client-role playback: source arbitration (network vs. local I2S
 *        input), a PSRAM ring buffer for the network path, and the player
 *        task that drives audio_i2s_write_mono().
 *
 * Mirrors the source-arbiter pattern from the ESP32_Mesh_Snapclient
 * reference project (network vs. A2DP there, network vs. local I2S input
 * here): exactly one source is ever fed to the DSP/output stage, PCM fed by
 * an inactive source is dropped rather than queued, and switching sources
 * flushes the ring buffer so a stale backlog can't suddenly play out.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One 20 ms mono frame at 48 kHz -- matches the Opus frame size used
 * end-to-end by both the server's encoder and this sink's own I2S cadence. */
#define AUDIO_SINK_FRAME_SAMPLES 960

typedef enum {
    AUDIO_SINK_SOURCE_NONE = 0,
    AUDIO_SINK_SOURCE_NETWORK,
    AUDIO_SINK_SOURCE_LOCAL_INPUT,
} audio_sink_source_t;

/*
 * Starts the ring buffer (sized from buffer_ms, PSRAM-backed) and the
 * player task that reads audio_i2s_capture_mono() every frame, arbitrates
 * the source and writes the result via audio_i2s_write_mono(). Requires
 * audio_i2s_start() to already have succeeded. Client role only.
 */
esp_err_t audio_sink_start(uint16_t buffer_ms);

/* Reports whether the Snapcast connection currently has decoded audio to
 * offer. AUDIO_SINK_SOURCE_NETWORK is only ever chosen while this is true. */
void audio_sink_set_network_active(bool active);

/*
 * Feeds one frame of decoded mono network PCM. Dropped (and not queued) if
 * the network path isn't the currently active source -- see the header
 * comment above. Returns the number of samples actually accepted.
 */
size_t audio_sink_feed_network(const int16_t *mono_pcm, size_t sample_count);

/* SOURCE_MODE_AUTO / _NETWORK_ONLY / _LOCAL_ONLY from device_config.h.
 * Safe to call before audio_sink_start(); the value just isn't used yet. */
void audio_sink_set_source_mode(uint8_t mode);

/* dBFS RMS threshold above which the local I2S input counts as "present"
 * for SOURCE_MODE_AUTO. Safe to call before audio_sink_start(). */
void audio_sink_set_local_input_threshold_db(int8_t threshold_db);

/* Device-local playback delay trim in milliseconds, positive or negative.
 * Recorded here for the Stufe-2 playback scheduler to apply; has no
 * observable effect yet on its own. Safe to call before audio_sink_start(). */
void audio_sink_set_delay_trim_ms(int16_t delay_trim_ms);

/* Currently active output source (diagnostics/status API). */
audio_sink_source_t audio_sink_current_source(void);

#ifdef __cplusplus
}
#endif
