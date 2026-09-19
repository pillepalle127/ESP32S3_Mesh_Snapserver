/**
 * @file audio_sink.h
 * @brief Client-role playback: source arbitration (network vs. local I2S
 *        input), a PSRAM ring buffer for the network path, and the player
 *        task that drives audio_i2s_write_mono().
 *
 * Source arbitration follows the ESP32_Mesh_Snapclient reference project
 * (network vs. A2DP there, network vs. local I2S input here): exactly one
 * source is ever fed to the DSP/output stage. A live announcement (see
 * audio_sink_feed_voice()) is not a third source but an overlay: while one
 * plays it replaces the output, and the active source keeps running
 * underneath exactly as on a muted client, so the music carries on
 * seamlessly afterwards. Two rules differ from the reference, both for
 * reasons that only showed up on device:
 *   - Network PCM is accepted while the local input is idle, including
 *     during prebuffering when nothing is playing yet. Dropping it whenever
 *     the network wasn't already the active source made prebuffering
 *     impossible -- the buffer could never fill.
 *   - The ring is flushed when *leaving* the network source, not on every
 *     switch. Flushing on entry would discard the very prebuffer that was
 *     just built up.
 *
 * On top of the arbiter sits a playback scheduler: once snapclient.c has a
 * clock offset, each sample is placed at chunk_ts - offset + bufferMs -
 * latency + delay_trim_ms rather than simply played as soon as it arrives.
 * Large errors are corrected in one step, small ones by trimming the
 * resampling ratio (see audio_resample.h).
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
    /* Only ever reported by audio_sink_current_source(), while an
     * announcement overlays the output; never an arbiter choice. */
    AUDIO_SINK_SOURCE_VOICE,
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
 * Feeds one frame of decoded mono network PCM. chunk_ts_us is the
 * server-clock timestamp of its first sample (from the WireChunk header)
 * and anchors the playback scheduler; a timestamp that doesn't continue
 * the buffered stream re-anchors it. Dropped (and not queued) while the
 * local input is the active source -- see the header comment above.
 * Returns the number of samples actually accepted.
 */
size_t audio_sink_feed_network(const int16_t *mono_pcm,
                               size_t sample_count,
                               int64_t chunk_ts_us);

/*
 * Feeds a piece of live voice-announcement PCM (from voice_announce.c's UDP
 * receive task, a level-1-only client concern). No explicit arm/disarm call
 * exists: receiving here IS the signal, so the announcement overlays the
 * output purely based on how recently this was last called (see the header
 * comment). mono_samples is typically 480 (10 ms); pushed
 * into a small fixed mailbox that is drained, never waited on -- a
 * deliberately different contract from audio_sink_feed_network()'s ring,
 * matching the announcement path's "drop stale audio rather than buffer it"
 * design.
 */
void audio_sink_feed_voice(const int16_t *mono, size_t mono_samples);

/*
 * Clock synchronisation result from snapclient.c: offset_us is
 * server_clock - local_esp_timer_clock. While valid is false the scheduler
 * stays disengaged and playback just follows the ring buffer.
 */
void audio_sink_set_server_time_offset(int64_t offset_us, bool valid);

/* bufferMs/latency from the server's ServerSettings message. Together with
 * delay_trim_ms they define when a chunk is due on the local clock. */
void audio_sink_set_stream_timing(uint32_t buffer_ms, int32_t latency_ms);

/*
 * Per-client volume from the server's ServerSettings message: percent is
 * 0-100, muted overrides it. Applied at the very end of the playback path,
 * so a change is audible immediately instead of buffer_ms later, and to
 * whichever source is playing -- it is this speaker's level, not the
 * network stream's.
 */
void audio_sink_set_volume(int32_t percent, bool muted);

/* SOURCE_MODE_AUTO / _NETWORK_ONLY / _LOCAL_ONLY from device_config.h.
 * Safe to call before audio_sink_start(); the value just isn't used yet. */
void audio_sink_set_source_mode(uint8_t mode);

/* dBFS RMS threshold above which the local I2S input counts as "present"
 * for SOURCE_MODE_AUTO. Safe to call before audio_sink_start(). */
void audio_sink_set_local_input_threshold_db(int8_t threshold_db);

/* Device-local playback delay trim in milliseconds, positive or negative,
 * applied on top of bufferMs/latency by the playback scheduler. Safe to
 * call before audio_sink_start(). */
void audio_sink_set_delay_trim_ms(int16_t delay_trim_ms);

/* Currently active output source (diagnostics/status API). */
audio_sink_source_t audio_sink_current_source(void);

#ifdef __cplusplus
}
#endif
