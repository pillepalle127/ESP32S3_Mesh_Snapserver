/**
 * @file wled_analysis.c
 * @brief Spectrum, level and beat of one audio window for WLED.
 *
 * The output ranges follow what WLED's effects expect (level and bands
 * 0-255, magnitude up to about 1000); how they are derived is our own:
 * a Hann-windowed real FFT, band energies in dB, and one slow AGC that
 * anchors everything to the loud parts of the music, so the lights move
 * the same whether the source plays loud or quiet.
 */
#include "wled_analysis.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "dsps_fft2r.h"
#define HAVE_ESP_DSP 1
#else
#define HAVE_ESP_DSP 0
#endif

#define N            WLED_ANALYSIS_FFT_SIZE
#define M            (WLED_ANALYSIS_FFT_SIZE / 2U)   /* complex FFT size */
#define PI_F         3.14159265358979f

/* Input below this RMS counts as silence: no AGC update, outputs decay. */
#define GATE_DBFS           (-60.0f)
/* The AGC reference: starts here, stays within these limits. */
#define REF_START_DBFS      (-20.0f)
#define REF_MIN_DBFS        (-40.0f)
#define REF_MAX_DBFS        (-3.0f)
/* Rises within a few frames on loud parts, falls 1 dB/s (20 ms frames). */
#define REF_ATTACK          0.3f
#define REF_RELEASE_DB      0.02f
/* dB below the reference that map to 0; the reference itself maps to
 * LEVEL_AT_REF (level) or 255 (bands, magnitude). */
#define LEVEL_RANGE_DB      30.0f
#define LEVEL_AT_REF        230.0f
#define BAND_RANGE_DB       30.0f
/* Music falls off towards the treble; this lifts band i by i * TILT. */
#define BAND_TILT_DB        0.5f
/* Bands fall by this factor per frame, rise at once. */
#define BAND_FALL           0.80f
#define LEVEL_RISE          0.6f
#define LEVEL_FALL          0.12f
/*
 * Beat: the bass jumps above its recent average by more than it usually
 * wanders -- BEAT_DEV_FACTOR times its mean deviation, at least
 * BEAT_MIN_RISE_DB -- and rises from the frame before by BEAT_EDGE_DB.
 * At most every 160 ms. Average and deviation follow over about 0.5 s.
 * On a test signal with a bass line this finds 56 of 60 kicks 6 dB above
 * it and 38 of 60 at 3 dB; pink noise fires about once in 6 s.
 */
#ifndef BEAT_DEV_FACTOR
#define BEAT_DEV_FACTOR     3.0f
#endif
#ifndef BEAT_MIN_RISE_DB
#define BEAT_MIN_RISE_DB    3.0f
#endif
#ifndef BEAT_EDGE_DB
#define BEAT_EDGE_DB        2.0f
#endif
#define BEAT_MIN_REL_DB     (-30.0f)
#define BEAT_MIN_FRAMES     8U
#define BEAT_AVG            0.04f
/* Bands 0-2 (up to about 215 Hz) make up the bass for the beat. */
#define BEAT_BANDS          3U
#define BASS_AVG_UNSET_DB   (-200.0f)
#define MAJOR_PEAK_MAX_HZ   11025.0f

/*
 * Band edges in Hz: the frequency ranges WLED's own analysis uses for its
 * 16 channels (43 Hz sub-bass to 9.3 kHz), so its effects see the bands
 * they were made for.
 */
static const float BAND_EDGES_HZ[WLED_ANALYSIS_BANDS + 1U] = {
    43, 86, 129, 216, 301, 430, 560, 818, 1120, 1421, 1895, 2412, 3015, 3704, 4479, 7106, 9259,
};

/*
 * A broadband signal at RMS r spreads 3 r^2 over the one-sided spectrum
 * (Hann window, POWER_NORM below). Subtracting this puts band energies on
 * the same scale as the RMS level.
 */
#define SPECTRUM_OVER_RMS_DB 4.771f

/* Full-scale sine (amplitude 1.0) through the Hann window: |X| = N/4. */
#define POWER_NORM (16.0f / ((float)N * (float)N))

static float clampf(float x, float lo, float hi)
{
    return (x < lo) ? lo : (x > hi) ? hi : x;
}

static float to_db(float power)
{
    return 10.0f * log10f(power + 1e-12f);
}

static uint8_t to_byte(float value)
{
    return (uint8_t)lrintf(clampf(value, 0.0f, 255.0f));
}

/* Twiddle exp(-2 pi i k / N) = (tw_cos, tw_sin) for k = 0..N/2, from the
 * one cosine table: sin(x) = cos(pi/2 - x), and cos is even. */
static inline float tw_cos(const wled_analysis_t *a, uint32_t k)
{
    return a->cos_tab[k];
}

static inline float tw_sin(const wled_analysis_t *a, uint32_t k)
{
    return -a->cos_tab[(k <= N / 4U) ? N / 4U - k : k - N / 4U];
}

float wled_analysis_window(const wled_analysis_t *a, uint32_t n)
{
    return 0.5f - 0.5f * a->cos_tab[(n <= N / 2U) ? n : N - n];
}

/* In-place radix-2 FFT of M complex values, interleaved re/im in buf. */
static void own_complex_fft(wled_analysis_t *a)
{
    float *x = a->buf;

    for (uint32_t i = 1, j = 0; i < M; ++i) {
        uint32_t bit = M >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float t = x[2 * i];
            x[2 * i] = x[2 * j];
            x[2 * j] = t;
            t = x[2 * i + 1];
            x[2 * i + 1] = x[2 * j + 1];
            x[2 * j + 1] = t;
        }
    }

    for (uint32_t size = 2; size <= M; size <<= 1) {
        const uint32_t half = size >> 1;
        /* exp(-2 pi i j / size) = twiddle j * (N / size). */
        const uint32_t step = N / size;
        for (uint32_t start = 0; start < M; start += size) {
            for (uint32_t j = 0; j < half; ++j) {
                const float wr = tw_cos(a, j * step);
                const float wi = tw_sin(a, j * step);
                float *p = &x[2 * (start + j)];
                float *q = &x[2 * (start + j + half)];
                const float tr = wr * q[0] - wi * q[1];
                const float ti = wr * q[1] + wi * q[0];
                q[0] = p[0] - tr;
                q[1] = p[1] - ti;
                p[0] += tr;
                p[1] += ti;
            }
        }
    }
}

/* esp-dsp's FFT where it passed the check (several times faster on the
 * S3), our own otherwise. */
static void complex_fft(wled_analysis_t *a)
{
#if HAVE_ESP_DSP
    if (a->use_dsp) {
        dsps_fft2r_fc32(a->buf, (int)M);
        dsps_bit_rev2r_fc32(a->buf, (int)M);
        return;
    }
#endif
    own_complex_fft(a);
}

/*
 * Power of bin k of the N-point real spectrum, from the M-point complex
 * one: X[k] = E[k] + W^k O[k], with E and O the spectra of the even and
 * odd samples, recovered from Z[k] and Z[M-k].
 */
static float split_power(const wled_analysis_t *a, uint32_t k)
{
    const uint32_t mirror = (M - k) & (M - 1U);
    const float ar = a->buf[2 * k];
    const float ai = a->buf[2 * k + 1];
    const float br = a->buf[2 * mirror];
    const float bi = a->buf[2 * mirror + 1];

    const float er = 0.5f * (ar + br);
    const float ei = 0.5f * (ai - bi);
    const float or_ = 0.5f * (ai + bi);
    const float oi = -0.5f * (ar - br);

    const float wr = tw_cos(a, k);
    const float wi = tw_sin(a, k);
    const float xr = er + wr * or_ - wi * oi;
    const float xi = ei + wr * oi + wi * or_;
    return (xr * xr + xi * xi) * POWER_NORM;
}

void wled_analysis_power_spectrum(wled_analysis_t *a, float *power)
{
    complex_fft(a);
    power[0] = 0.0f; /* DC, of no use here */
    for (uint32_t k = 1; k < M; ++k) {
        power[k] = split_power(a, k);
    }
}

#if HAVE_ESP_DSP
/* Windowed noise, the same every time. */
static void fill_test_signal(wled_analysis_t *a)
{
    uint32_t seed = 12345U;
    for (uint32_t n = 0; n < N; ++n) {
        seed = seed * 1664525U + 1013904223U;
        a->buf[n] = ((float)(seed >> 8) / 8388608.0f - 1.0f) * wled_analysis_window(a, n);
    }
}

/*
 * The same signal through both FFTs. esp-dsp is used only if every bin
 * agrees with ours to within 0.1 % of the strongest one -- a wrong table
 * or layout would show as far more than that.
 */
static void check_dsp_fft(wled_analysis_t *a)
{
    float *own = malloc(M * sizeof(float));
    if (own == NULL) {
        return;
    }
    a->use_dsp = false;
    fill_test_signal(a);
    wled_analysis_power_spectrum(a, own);

    a->use_dsp = true;
    fill_test_signal(a);
    complex_fft(a);

    float peak = 0.0f;
    for (uint32_t k = 1; k < M; ++k) {
        if (own[k] > peak) {
            peak = own[k];
        }
    }
    float worst = 0.0f;
    for (uint32_t k = 1; k < M; ++k) {
        const float diff = fabsf(split_power(a, k) - own[k]) / peak;
        if (diff > worst) {
            worst = diff;
        }
    }
    free(own);
    a->dsp_error = worst;
    a->use_dsp = worst < 1e-3f;
}
#endif

void wled_analysis_init(wled_analysis_t *a, uint32_t sample_rate)
{
    for (uint32_t k = 0; k <= N / 2U; ++k) {
        a->cos_tab[k] = cosf(2.0f * PI_F * (float)k / (float)N);
    }

    a->bin_hz = (float)sample_rate / (float)N;
    for (uint32_t i = 0; i <= WLED_ANALYSIS_BANDS; ++i) {
        long bin = lrintf(BAND_EDGES_HZ[i] / a->bin_hz);
        if (bin < 1) {
            bin = 1;
        }
        /* At least one bin per band. */
        if (i > 0 && bin <= (long)a->band_lo[i - 1U]) {
            bin = (long)a->band_lo[i - 1U] + 1;
        }
        if (bin > (long)M - 1) {
            bin = (long)M - 1;
        }
        a->band_lo[i] = (uint16_t)bin;
    }
    uint32_t last = (uint32_t)(MAJOR_PEAK_MAX_HZ / a->bin_hz);
    if (last > M - 1U) {
        last = M - 1U;
    }
    if (last < a->band_lo[WLED_ANALYSIS_BANDS]) {
        last = a->band_lo[WLED_ANALYSIS_BANDS];
    }
    a->last_bin = (uint16_t)last;

    a->ref_db = REF_START_DBFS;
    a->level_smth = 0.0f;
    memset(a->bands_out, 0, sizeof(a->bands_out));
    a->bass_avg_db = BASS_AVG_UNSET_DB;
    a->frames_since_peak = BEAT_MIN_FRAMES;
    a->frame_counter = 0;

    a->use_dsp = false;
    a->dsp_error = -1.0f;
#if HAVE_ESP_DSP
    if (dsps_fft2r_init_fc32(NULL, (int)M) == ESP_OK) {
        check_dsp_fft(a);
    }
#endif
}

bool wled_analysis_run(wled_analysis_t *a, const int16_t *samples, size_t new_samples,
                       wled_sync_packet_t *out)
{
    if (new_samples == 0U || new_samples > N) {
        new_samples = N;
    }

    /* Integer: exact, and the S3's FPU has no double precision. */
    int64_t sum_sq = 0;
    for (size_t n = N - new_samples; n < N; ++n) {
        sum_sq += (int32_t)samples[n] * samples[n];
    }
    const float rms_db = to_db((float)sum_sq / ((float)new_samples * 32768.0f * 32768.0f));
    const bool silent = rms_db < GATE_DBFS;

    memset(out, 0, sizeof(*out));
    memcpy(out->header, "00002", 6);

    /* Silent with everything decayed: nothing to show, skip the FFT. */
    if (silent && a->level_smth == 0.0f) {
        bool any = false;
        for (uint32_t i = 0; i < WLED_ANALYSIS_BANDS && !any; ++i) {
            any = a->bands_out[i] > 0.0f;
        }
        if (!any) {
            a->bass_avg_db = BASS_AVG_UNSET_DB;
            out->frame_counter = a->frame_counter++;
            out->fft_major_peak = 1.0f;
            return false;
        }
    }

    /* The Hann window from the cosine table, in two halves so neither
     * loop needs a branch: w(n) = (1 - cos(2 pi n / N)) / 2, and the second
     * half mirrors the first. */
    const float scale = 0.5f / 32768.0f;
    for (uint32_t n = 0; n <= N / 2U; ++n) {
        a->buf[n] = (float)samples[n] * scale * (1.0f - a->cos_tab[n]);
    }
    for (uint32_t n = N / 2U + 1U; n < N; ++n) {
        a->buf[n] = (float)samples[n] * scale * (1.0f - a->cos_tab[N - n]);
    }
    complex_fft(a);

    /* Only the bins up to 11 kHz: band energies and the strongest bin. */
    float band_power[WLED_ANALYSIS_BANDS] = {0};
    uint32_t band = 0;
    uint32_t top = 1;
    float top_power = 0.0f;
    for (uint32_t k = 1; k <= a->last_bin; ++k) {
        const float p = split_power(a, k);
        if (p > top_power) {
            top_power = p;
            top = k;
        }
        while (band < WLED_ANALYSIS_BANDS && k >= a->band_lo[band + 1U]) {
            ++band;
        }
        if (band < WLED_ANALYSIS_BANDS && k >= a->band_lo[0]) {
            band_power[band] += p;
        }
    }

    if (!silent) {
        if (rms_db > a->ref_db) {
            a->ref_db += REF_ATTACK * (rms_db - a->ref_db);
        } else {
            a->ref_db -= REF_RELEASE_DB;
        }
        a->ref_db = clampf(a->ref_db, REF_MIN_DBFS, REF_MAX_DBFS);
    }
    const float spectrum_ref_db = a->ref_db + SPECTRUM_OVER_RMS_DB;

    /* Level */
    const float level = silent ? 0.0f
                               : clampf((rms_db - a->ref_db + LEVEL_RANGE_DB) / LEVEL_RANGE_DB *
                                            LEVEL_AT_REF,
                                        0.0f, 255.0f);
    a->level_smth += (level - a->level_smth) * ((level > a->level_smth) ? LEVEL_RISE : LEVEL_FALL);
    if (a->level_smth < 0.5f) {
        a->level_smth = 0.0f;
    }

    /* Bands */
    float bass_power = 0.0f;
    bool any_band = false;
    for (uint32_t i = 0; i < WLED_ANALYSIS_BANDS; ++i) {
        if (i < BEAT_BANDS) {
            bass_power += band_power[i];
        }
        float target = 0.0f;
        if (!silent) {
            const float rel_db = to_db(band_power[i]) - spectrum_ref_db + (float)i * BAND_TILT_DB;
            target = clampf((rel_db + BAND_RANGE_DB) / BAND_RANGE_DB * 255.0f, 0.0f, 255.0f);
        }
        float shown = a->bands_out[i] * BAND_FALL;
        if (target > shown) {
            shown = target;
        }
        if (shown < 0.5f) {
            shown = 0.0f;
        }
        a->bands_out[i] = shown;
        any_band = any_band || shown > 0.0f;
    }

    /* Beat. The average starts over after silence, from the first frame
     * that is not: rising from nothing, it would call every frame of the
     * first half second a beat. */
    const float bass_db = to_db(bass_power);
    bool peak = false;
    if (a->frames_since_peak < BEAT_MIN_FRAMES) {
        ++a->frames_since_peak;
    }
    if (silent) {
        a->bass_avg_db = BASS_AVG_UNSET_DB;
    } else if (a->bass_avg_db <= BASS_AVG_UNSET_DB) {
        a->bass_avg_db = bass_db;
        a->bass_dev_db = 3.0f;
        a->bass_prev_db = bass_db;
    } else {
        float threshold = BEAT_DEV_FACTOR * a->bass_dev_db;
        if (threshold < BEAT_MIN_RISE_DB) {
            threshold = BEAT_MIN_RISE_DB;
        }
        if (a->frames_since_peak >= BEAT_MIN_FRAMES &&
            bass_db > a->bass_avg_db + threshold &&
            bass_db > a->bass_prev_db + BEAT_EDGE_DB &&
            bass_db - spectrum_ref_db > BEAT_MIN_REL_DB) {
            peak = true;
            a->frames_since_peak = 0;
        }
        const float diff = bass_db - a->bass_avg_db;
        a->bass_avg_db += BEAT_AVG * diff;
        a->bass_dev_db += BEAT_AVG * (fabsf(diff) - a->bass_dev_db);
        a->bass_prev_db = bass_db;
    }

    /* Strongest bin */
    float magnitude = 0.0f;
    float major_peak = 1.0f;
    if (!silent) {
        const float rel_db = to_db(top_power) - spectrum_ref_db;
        magnitude = 4.0f * clampf((rel_db + BAND_RANGE_DB) / BAND_RANGE_DB * 255.0f, 0.0f, 255.0f);
        major_peak = clampf((float)top * a->bin_hz, 1.0f, MAJOR_PEAK_MAX_HZ);
    }

    out->sample_raw = level;
    out->sample_smth = a->level_smth;
    out->sample_peak = peak ? 1U : 0U;
    out->frame_counter = a->frame_counter++;
    for (uint32_t i = 0; i < WLED_ANALYSIS_BANDS; ++i) {
        out->fft_result[i] = to_byte(a->bands_out[i]);
    }
    out->fft_magnitude = magnitude;
    out->fft_major_peak = major_peak;

    return !silent || a->level_smth > 0.0f || any_band;
}
