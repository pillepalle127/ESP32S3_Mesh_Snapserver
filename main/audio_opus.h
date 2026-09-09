/**
 * @file audio_opus.h
 * @brief Public interface and stream format constants for the mono Opus encoder.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_SAMPLE_RATE       48000
#define AUDIO_CHANNELS              1
#define AUDIO_BITS                 16
#define AUDIO_FRAME_MS             20
#define AUDIO_FRAME_SAMPLES       960
#define AUDIO_MAX_OPUS_PACKET    1500

typedef struct {
    const uint8_t *data;
    size_t size;
    int64_t timestamp_us;
} audio_opus_packet_t;

esp_err_t audio_opus_start(void);
esp_err_t audio_opus_get_packet(audio_opus_packet_t *packet);
