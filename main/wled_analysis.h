/**
 * @file wled_analysis.h
 * @brief Spectrum, level and beat of one audio window, packed as a WLED
 *        AudioReactive sync packet.
 *
 * Builds on the host as well (with its own FFT); on the ESP the FFT comes
 * from esp-dsp, checked against our own at init. Only the packet layout is
 * WLED's (interoperability); the analysis behind it is our own.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Samples per analysis window: 21.3 ms at 48 kHz, about what WLED's own
 * 512 samples at 22.05 kHz span. */
#define WLED_ANALYSIS_FFT_SIZE 1024U
#define WLED_ANALYSIS_BANDS    16U

/*
 * WLED AudioReactive sync packet, format "00002" (WLED 0.14 and later),
 * 44 bytes, little endian. Layout as in WLED's audio_reactive.cpp.
 */
typedef struct __attribute__((packed)) {
    char     header[6];          /* "00002\0" */
    uint8_t  reserved1[2];
    float    sample_raw;         /* level, 0-255, unsmoothed */
    float    sample_smth;        /* level, 0-255, smoothed */
    uint8_t  sample_peak;        /* 1: beat in this packet */
    uint8_t  frame_counter;      /* unused by WLED, counted by WLED-MM */
    uint8_t  fft_result[WLED_ANALYSIS_BANDS]; /* 0-255 per band */
    uint16_t reserved3;
    float    fft_magnitude;      /* strongest bin, 0-1020 */
    float    fft_major_peak;     /* its frequency in Hz, 1-11025 */
} wled_sync_packet_t;

_Static_assert(sizeof(wled_sync_packet_t) == 44, "WLED sync packet must be 44 bytes");

/*
 * State of one analysis, about 6 KB. Allocate it 16-byte aligned (esp-dsp's
 * SIMD FFT works on buf), then wled_analysis_init().
 */
typedef struct {
    float    buf[WLED_ANALYSIS_FFT_SIZE] __attribute__((aligned(16))); /* windowed samples, then the FFT */
    /* cos(2 pi k / N) for k = 0..N/2; window and twiddles derive from it. */
    float    cos_tab[WLED_ANALYSIS_FFT_SIZE / 2U + 1U];
    uint16_t band_lo[WLED_ANALYSIS_BANDS + 1U];   /* first bin of each band, then the end */
    uint16_t last_bin;                            /* highest bin looked at (11 kHz) */
    float    bin_hz;
    bool     use_dsp;          /* esp-dsp FFT passed the check at init */
    float    dsp_error;        /* its largest deviation there, -1: not checked */

    float    ref_db;           /* loudness the mapping is anchored to (AGC) */
    float    level_smth;
    float    bands_out[WLED_ANALYSIS_BANDS];
    float    bass_avg_db;
    float    bass_dev_db;      /* mean absolute deviation around bass_avg_db */
    float    bass_prev_db;
    uint32_t frames_since_peak;
    uint8_t  frame_counter;
} wled_analysis_t;

/* Fills the tables and, on the ESP, checks esp-dsp's FFT against our own
 * (use_dsp, dsp_error). sample_rate in Hz. */
void wled_analysis_init(wled_analysis_t *a, uint32_t sample_rate);

/* The Hann window value for sample n. */
float wled_analysis_window(const wled_analysis_t *a, uint32_t n);

/*
 * Analyses the last WLED_ANALYSIS_FFT_SIZE samples (oldest first), of which
 * the last new_samples are new since the previous call, and fills *out.
 * Returns false while the input is silent and the output has decayed to
 * zero -- the FFT is skipped then, and the caller can send less often.
 */
bool wled_analysis_run(wled_analysis_t *a, const int16_t *samples, size_t new_samples,
                       wled_sync_packet_t *out);

/*
 * The full one-sided power spectrum of a->buf into power[0 .. N/2-1],
 * normalised so a full-scale sine reads 1.0 in its bin. Overwrites buf.
 * For the init check and the host test; the analysis itself only looks at
 * the bins up to last_bin.
 */
void wled_analysis_power_spectrum(wled_analysis_t *a, float *power);

#ifdef __cplusplus
}
#endif
