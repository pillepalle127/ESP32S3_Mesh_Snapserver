/**
 * @file audio_i2s.h
 * @brief Shared full-duplex I2S bus configuration and audio-frame API.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Gemeinsamer Full-Duplex-I2S-Bus:
 * ESP32-S3 = Master, TinySine und PCM5102A = Slaves.
 * RX und TX teilen BCLK und LRCLK. Die Datenleitungen sind getrennt.
 * MCLK wird nicht verwendet.
 */
/*#define AUDIO_I2S_GPIO_BCLK       17 //4
#define AUDIO_I2S_GPIO_LRCLK      8 //6
#define AUDIO_I2S_GPIO_DIN        5   // TinySine SD OUT -> ESP32-S3 
#define AUDIO_I2S_GPIO_DOUT       18 //7   // ESP32-S3 -> PCM5102A DIN */

#define AUDIO_I2S_GPIO_BCLK       4
#define AUDIO_I2S_GPIO_LRCLK      6
#define AUDIO_I2S_GPIO_DIN        5   // TinySine SD OUT -> ESP32-S3 
#define AUDIO_I2S_GPIO_DOUT       7   // ESP32-S3 -> PCM5102A DIN */

#define AUDIO_I2S_SAMPLE_RATE 48000
#define AUDIO_I2S_CHANNELS        2
#define AUDIO_I2S_BITS           16

/*
 * TX DMA geometry. Public because the client's playback scheduler has to
 * know how far ahead of the speaker the I2S write call actually is: a frame
 * handed to i2s_channel_write() is only heard once the queued DMA backlog
 * has drained, i.e. about AUDIO_I2S_TX_LATENCY_US later in steady state.
 *
 * These 40 ms are also the last 40 ms of an announcement's end-to-end
 * delay, so halving them was tried (2026-09-20) and deliberately reverted:
 * the queue is shared with the music, where it is the only cushion against
 * a late frame from this device's own audio task -- measured frame deltas
 * reach ~30 ms against a 20 ms tick. Buying announcement latency with the
 * music's safety margin is the wrong trade; if those 20 ms are ever worth
 * it, they have to be taken for the announcement alone.
 */
#define AUDIO_I2S_DMA_DESC_NUM    8
#define AUDIO_I2S_DMA_FRAME_NUM 240
#define AUDIO_I2S_TX_LATENCY_US \
    ((int64_t)AUDIO_I2S_DMA_DESC_NUM * AUDIO_I2S_DMA_FRAME_NUM * 1000000LL / AUDIO_I2S_SAMPLE_RATE)

/*
 * Lokale Linkwitz-Riley-Frequenzweiche 4. Ordnung.
 * Kconfig liefert nur noch die Startwerte fuer den allerersten Frame, bevor
 * app_main() die tatsaechlich gespeicherte Konfiguration per
 * audio_i2s_set_dsp_params() einspielt (siehe device_config.h).
 */
#include "sdkconfig.h"

/* Trennfrequenz aus menuconfig (int Hz) -> float fuer die Filterberechnung. */
#define AUDIO_CROSSOVER_FREQUENCY_HZ ((float)CONFIG_SNAPSERVER_CROSSOVER_HZ)
/* PCM5102A-Ausgangskanaele, Startwert. Zum Tauschen 0 und 1 vertauschen. */
#define PCM_CHANNEL_LEFT              0
#define PCM_CHANNEL_RIGHT             1
#define SUBWOOFER_OUTPUT_CHANNEL      PCM_CHANNEL_LEFT
#define WIDEBAND_OUTPUT_CHANNEL       PCM_CHANNEL_RIGHT

/*
 * Runtime-DSP-Parameter. bypass=true schaltet die Weiche aus (Mono direkt
 * auf beide Ausgaenge, Unity-Gain). sub_channel/wideband_channel sind 0
 * (links) oder 1 (rechts) und muessen sich unterscheiden.
 */
typedef struct {
    bool  bypass;
    float crossover_hz;
    float sub_gain_db;
    float wideband_gain_db;
    uint8_t sub_channel;
    uint8_t wideband_channel;
} audio_dsp_params_t;

esp_err_t audio_i2s_start(void);

/*
 * Delays the server's *local* speaker output by delay_ms, so it lands on
 * the same instant as the clients' output instead of running bufferMs
 * ahead of them: the server hears a frame right after capturing it, while
 * every client deliberately plays that same chunk bufferMs later (see the
 * scheduler in audio_sink.c). Only affects audio_i2s_read_frame(), i.e.
 * the server path -- the client's own output must not be delayed again,
 * its scheduler already places it in time.
 *
 * The backing buffer is sized once here for max_delay_ms; delay_ms may
 * then be changed freely up to that bound (delay_trim_ms is meant to be
 * adjustable while listening). delay_ms == 0 disables the delay.
 */
esp_err_t audio_i2s_set_output_delay(uint32_t delay_ms, uint32_t max_delay_ms);

/*
 * Validiert und uebernimmt neue DSP-Parameter. Rechnet Koeffizienten und
 * linearen Gain ausserhalb des Hot-Path neu, tauscht sie dann kurz
 * kritisch-section-geschuetzt ein. Aus einem beliebigen Task aufrufbar
 * (z.B. dem HTTP-Config-Handler), nicht nur aus dem audio_task.
 */
esp_err_t audio_i2s_set_dsp_params(const audio_dsp_params_t *params);

/* Liest die aktuell aktiven DSP-Parameter zurueck (z.B. fuer die Config-API). */
void audio_i2s_get_dsp_params(audio_dsp_params_t *out);

/*
 * Local master volume, 0.0 to 1.0 linear, applied at the very end of the
 * output stage. Affects only this device's own speaker: the Opus encoder
 * is fed from a separate copy that does not pass through here, so a server
 * turned down still sends its clients a full-scale stream. It multiplies
 * with the per-client Snapcast volume audio_sink.c applies, so the knob on
 * the box and a listener's control app both keep working.
 *
 * Safe to call from any task and at any rate; the value is ramped across
 * one frame inside the audio task instead of stepping the waveform.
 * Defaults to 1.0, so a device without a knob plays at full volume.
 */
void audio_i2s_set_master_volume(float linear);

/*
 * Liest Stereo vom TinySine und bildet daraus Mono.
 * Das ungefilterte Mono wird dem Snapserver im Puffer "mono" bereitgestellt.
 * Lokal wird dasselbe Mono durch eine Linkwitz-Riley-Weiche 4. Ordnung
 * verarbeitet und als Tiefpass/Hochpass an den PCM5102A ausgegeben.
 *
 * Combines audio_i2s_capture_mono() and audio_i2s_write_mono() (see below)
 * plus the server's own wall-clock capture timeline. Used only by the
 * server role (audio_opus.c); the client role uses the two split calls
 * directly since its output source isn't always the local capture.
 */
esp_err_t audio_i2s_read_frame(int16_t *mono,
                               size_t mono_samples,
                               int64_t *timestamp_us);

/*
 * Reads one stereo frame from the RX side and mixes it to mono, without any
 * DSP or TX output -- just the capture half of audio_i2s_read_frame(). Used
 * by the client role (audio_sink.c) both as the substitute local-input
 * source and as the frame it reads purely to keep pace with the I2S clock
 * and to run its level detector on.
 */
esp_err_t audio_i2s_capture_mono(int16_t *mono, size_t mono_samples);

/*
 * Reads and clears the peak level seen at the crossover output since the
 * last call. Meant to be printed from outside the audio path: logging from
 * the real-time task costs it ~10 ms of blocking UART time per line, which
 * is a large slice of a 20 ms frame budget.
 */
void audio_i2s_take_output_peak(int16_t *left, int16_t *right);

/*
 * Same measurement, separate slot, for the status LED. Kept apart from
 * audio_i2s_take_output_peak() because both readers clear on read and run at
 * very different rates -- one 30 times a second, the other every five.
 */
void audio_i2s_take_led_peak(int16_t *left, int16_t *right);

/*
 * Deviation of this device's I2S clock from AUDIO_I2S_SAMPLE_RATE, in ppm,
 * measured against esp_timer since the first frame written out. Positive means
 * the I2S unit runs fast.
 *
 * Diagnostic for playback sync: server and clients each run their own
 * crystal and divider, and the client's drift control can only correct
 * +-200 ppm of the difference. Comparing this number between a server and
 * its clients says whether a standing offset is a clock difference or
 * something in the timeline.
 */
int32_t audio_i2s_clock_ppm(void);

/*
 * Runs the mono input through the LR4 crossover (or bypasses it) and writes
 * the resulting sub/wideband pair to the TX side -- just the DSP+output half
 * of audio_i2s_read_frame(). Used by the client role (audio_sink.c) to play
 * out whichever source (network or local input) is currently active.
 */
esp_err_t audio_i2s_write_mono(const int16_t *mono, size_t mono_samples);

/*
 * Overrides the server's own local-speaker output only, bypassing the
 * output delay line above -- a voice announcement is meant to come out of
 * this speaker at the lowest latency available, not lined up with clients
 * bufferMs later. Used by voice_announce.c. Deactivating drops whatever is
 * still queued rather than draining it, so a stale tail can never play once
 * an announcement has ended.
 *
 * Capture, its timestamp and the Opus path stay completely unaffected --
 * this only changes what audio_i2s_read_frame() writes to the physical
 * output at its very last step. Client role: unused, the same effect is
 * already reached by feeding audio_i2s_write_mono() from audio_sink.c.
 */
void audio_i2s_set_voice_active(bool active);

/*
 * Queues up to mono_samples of live announcement PCM for the server's local
 * speaker (see audio_i2s_set_voice_active()). Frames may arrive in smaller
 * pieces than one audio_i2s_read_frame() call consumes -- e.g. 480-sample
 * (10 ms) network packets against a 960-sample (20 ms) frame cadence -- and
 * are assembled FIFO; any samples still missing when a frame is due are
 * zero-filled rather than waited for, matching the "never buffer, never
 * wait" design of the whole announcement path.
 */
void audio_i2s_feed_voice(const int16_t *mono, size_t mono_samples);

/*
 * Reads and clears the announcement mailbox counters: samples zero-filled
 * because it ran dry, samples dropped because it overflowed, and how long
 * arriving audio had to wait here before playback (average and worst case
 * since the last call). That wait is the announcement's only variable delay
 * on this device -- everything else in its path is a constant.
 */
void audio_i2s_take_voice_stats(uint32_t *underrun_samples, uint32_t *dropped_samples,
                                uint32_t *wait_avg_ms, uint32_t *wait_max_ms);

