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
/* AUDIO_FRAME_MS * AUDIO_SAMPLE_RATE / 1000 */
#define AUDIO_FRAME_SAMPLES       960
/* Upper bound for one encoded Opus packet. */
#define AUDIO_MAX_OPUS_PACKET    1500

/*
 * One encoded Opus packet. data points into a static buffer owned by
 * audio_opus.c and is valid until the next audio_opus_get_packet() call.
 */
typedef struct {
    const uint8_t *data;
    size_t size;
    int64_t timestamp_us;
} audio_opus_packet_t;

esp_err_t audio_opus_start(void);
esp_err_t audio_opus_get_packet(audio_opus_packet_t *packet);
