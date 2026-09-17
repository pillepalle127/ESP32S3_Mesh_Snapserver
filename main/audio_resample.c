/**
 * @file audio_resample.c
 * @brief Linear resampler driven by a 32.32 fixed-point phase accumulator.
 */
#include "audio_resample.h"

#define PHASE_ONE       (1ULL << 32)
#define PHASE_FRAC_MASK (PHASE_ONE - 1ULL)

void audio_resample_init(audio_resample_t *state)
{
    state->phase = 0;
    state->step = PHASE_ONE;
}

void audio_resample_set_ppm(audio_resample_t *state, int32_t ppm)
{
    if (ppm > AUDIO_RESAMPLE_MAX_PPM) {
        ppm = AUDIO_RESAMPLE_MAX_PPM;
    } else if (ppm < -AUDIO_RESAMPLE_MAX_PPM) {
        ppm = -AUDIO_RESAMPLE_MAX_PPM;
    }

    state->step = (uint64_t)((int64_t)PHASE_ONE +
                             ((int64_t)ppm * (int64_t)PHASE_ONE) / 1000000LL);
}

int32_t audio_resample_get_ppm(const audio_resample_t *state)
{
    const int64_t delta = (int64_t)state->step - (int64_t)PHASE_ONE;
    return (int32_t)((delta * 1000000LL) / (int64_t)PHASE_ONE);
}

size_t audio_resample_input_needed(const audio_resample_t *state, size_t out_samples)
{
    if (out_samples == 0U) {
        return 0U;
    }

    /*
     * Highest index read is the integer part of the last output position
     * plus one for the interpolation partner, hence the +2.
     */
    const uint64_t last_position = state->phase + (uint64_t)(out_samples - 1U) * state->step;
    return (size_t)(last_position >> 32) + 2U;
}

size_t audio_resample_process(audio_resample_t *state,
                              const int16_t *input,
                              int16_t *output,
                              size_t out_samples)
{
    uint64_t phase = state->phase;
    const uint64_t step = state->step;

    for (size_t i = 0; i < out_samples; ++i) {
        const size_t index = (size_t)(phase >> 32);
        const uint64_t fraction = phase & PHASE_FRAC_MASK;

        const int32_t first = input[index];
        const int32_t second = input[index + 1U];
        const int64_t interpolated =
            (int64_t)first + (((int64_t)(second - first) * (int64_t)fraction) >> 32);

        output[i] = (int16_t)interpolated;
        phase += step;
    }

    const size_t consumed = (size_t)(phase >> 32);
    state->phase = phase & PHASE_FRAC_MASK;
    return consumed;
}
