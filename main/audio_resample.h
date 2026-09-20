/**
 * @file audio_resample.h
 * @brief Drift-correcting linear resampler with a 32.32 phase accumulator.
 *
 * Used by the client playback path to stretch or compress the incoming
 * stream by a few ppm so the local I2S clock stays locked to the server's
 * timeline. A 16.16 accumulator (as in the ESP32_Mesh_Snapclient reference)
 * quantises the ratio to ~15 ppm steps, which is coarser than the drift
 * being corrected; 32.32 puts the step far below 1 ppm.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Control range the caller may request.
 *
 * 200 ppm was chosen for a crystal pair (2 x +-20 ppm) plus headroom. On
 * device the controller was seen sitting at that limit while the playback
 * error grew past 70 ms, until a hard resync jumped it back -- audible as
 * the speakers drifting apart and then snapping into place. In steady
 * state, though, the clients settle at well under 50 ppm, so the limit is
 * about catching up, not about a standing rate difference.
 *
 * 500 ppm gives that headroom without inventing a rate difference nobody
 * has measured: 0.05 % is under a cent of pitch, inaudible, and the
 * scheduler only ever asks for what the error actually needs.
 */
#define AUDIO_RESAMPLE_MAX_PPM 500

typedef struct {
    uint64_t phase; /* 32.32 position of the next output sample */
    uint64_t step;  /* 32.32 input samples consumed per output sample */
} audio_resample_t;

void audio_resample_init(audio_resample_t *state);

/* ppm > 0 consumes input faster than it emits output (playback speeds up). */
void audio_resample_set_ppm(audio_resample_t *state, int32_t ppm);

int32_t audio_resample_get_ppm(const audio_resample_t *state);

/* Input samples that must be available for one audio_resample_process()
 * call producing out_samples. */
size_t audio_resample_input_needed(const audio_resample_t *state, size_t out_samples);

/*
 * Emits exactly out_samples and returns how many input samples were fully
 * consumed. The caller must drop that many from the front of its input and
 * keep the remainder for the next call -- the lookahead sample needed for
 * interpolation is deliberately left unconsumed.
 */
size_t audio_resample_process(audio_resample_t *state,
                              const int16_t *input,
                              int16_t *output,
                              size_t out_samples);
