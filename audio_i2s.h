#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Gemeinsamer I2S-Bus:
 * ESP32-S3 = Master, TinySine und PCM5102A = Slaves.
 * MCLK wird nicht auf einen GPIO gefuehrt.
 *
 * Diese vier GPIOs sind die zentrale Pin-Konfiguration.
 */
#define AUDIO_I2S_GPIO_BCLK       7
#define AUDIO_I2S_GPIO_LRCLK      8
#define AUDIO_I2S_GPIO_DIN        9   /* TinySine SD OUT -> ESP32-S3 */
#define AUDIO_I2S_GPIO_DOUT      10   /* ESP32-S3 -> PCM5102A DIN */

#define AUDIO_I2S_SAMPLE_RATE 48000
#define AUDIO_I2S_CHANNELS       2
#define AUDIO_I2S_BITS          16

esp_err_t audio_i2s_start(void);

/*
 * Liest Stereo vom TinySine, gibt denselben Frame lokal am PCM5102A aus
 * und erzeugt fuer den Snapserver Mono durch Mittelwertbildung L/R.
 */
esp_err_t audio_i2s_read_frame(int16_t *mono,
                               size_t mono_samples,
                               int64_t *timestamp_us);
