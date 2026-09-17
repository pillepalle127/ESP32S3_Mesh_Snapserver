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
 * Runs the mono input through the LR4 crossover (or bypasses it) and writes
 * the resulting sub/wideband pair to the TX side -- just the DSP+output half
 * of audio_i2s_read_frame(). Used by the client role (audio_sink.c) to play
 * out whichever source (network or local input) is currently active.
 */
esp_err_t audio_i2s_write_mono(const int16_t *mono, size_t mono_samples);
