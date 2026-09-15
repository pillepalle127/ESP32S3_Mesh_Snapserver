/**
 * @file audio_i2s.h
 * @brief Shared full-duplex I2S bus configuration and audio-frame API.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Gemeinsamer Full-Duplex-I2S-Bus:
 * ESP32-S3 = Master, TinySine und PCM5102A = Slaves.
 * RX und TX teilen BCLK und LRCLK. Die Datenleitungen sind getrennt.
 * MCLK wird nicht verwendet.
 *
 * Format: 48 kHz, 16-Bit-Daten in 32-Bit-Slots, stereo.
 */
#define AUDIO_I2S_GPIO_BCLK       4
#define AUDIO_I2S_GPIO_LRCLK      6
#define AUDIO_I2S_GPIO_DIN        5   /* TinySine SD OUT -> ESP32-S3 */
#define AUDIO_I2S_GPIO_DOUT       7   /* ESP32-S3 -> PCM5102A DIN */

#define AUDIO_I2S_SAMPLE_RATE 48000
#define AUDIO_I2S_CHANNELS        2
#define AUDIO_I2S_BITS           16

/*
 * Lokale Linkwitz-Riley-Frequenzweiche 4. Ordnung.
 * Die Trennfrequenz kann im menuconfig zentral angepasst werden.
 */
#include "sdkconfig.h"

/* Trennfrequenz aus menuconfig (int Hz) -> float fuer die Filterberechnung. */
#define AUDIO_CROSSOVER_FREQUENCY_HZ ((float)CONFIG_SNAPSERVER_CROSSOVER_HZ)
/* PCM5102A-Ausgangskanaele. Zum Tauschen einfach 0 und 1 vertauschen. */
#define PCM_CHANNEL_LEFT              0
#define PCM_CHANNEL_RIGHT             1
#define SUBWOOFER_OUTPUT_CHANNEL      PCM_CHANNEL_LEFT
#define WIDEBAND_OUTPUT_CHANNEL       PCM_CHANNEL_RIGHT


esp_err_t audio_i2s_start(void);

/*
 * Liest Stereo vom TinySine und bildet daraus Mono.
 * Das ungefilterte Mono wird dem Snapserver im Puffer "mono" bereitgestellt.
 * Lokal wird dasselbe Mono durch eine Linkwitz-Riley-Weiche 4. Ordnung
 * verarbeitet und als Tiefpass/Hochpass an den PCM5102A ausgegeben.
 *
 * mono_samples muss zwischen 1 und MAX_FRAME_SAMPLES (960) liegen.
 * timestamp_us erhaelt den Aufnahmezeitpunkt des Frames in Mikrosekunden
 * (monotone esp_timer-Basis).
 */
esp_err_t audio_i2s_read_frame(int16_t *mono,
                               size_t mono_samples,
                               int64_t *timestamp_us);
