/**
 * @file voice_playout.h
 * @brief Plays an announcement from its Opus packets at a fixed delay:
 *        missing packets are concealed, late ones dropped.
 *
 * The receive task pushes packets with their sequence numbers; the audio
 * path renders one frame at a time and decodes only then. Playback starts
 * once one frame's worth is in (the same 20 ms the PCM mailbox used to
 * wait for) and from then on runs on its own clock: each frame takes the
 * packet that is due. If that packet is not there, Opus' packet loss
 * concealment fills the slot instead of silence, and the packet is dropped
 * when it comes after all. So late packets cost a concealed frame, never
 * extra delay -- unlike the mailbox, which went silent, waited for a new
 * prefill after every gap, and threw away audio when the late packets then
 * arrived in a burst (measured 2026-10-10: gaps of 40-150 ms about ten
 * times in nine seconds, almost no packets lost).
 *
 * Used by the server's own speaker (audio_i2s.c) and by every client
 * (audio_sink.c), one instance each.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct voice_playout voice_playout_t;

/* Counters since the last voice_playout_take_stats(), frames are packets. */
typedef struct {
    uint32_t played;     /* packets decoded and played */
    uint32_t concealed;  /* frames filled by packet loss concealment */
    uint32_t silent;     /* frames with nothing to play or conceal */
    uint32_t late;       /* packets that arrived after their slot had played */
    uint32_t trimmed;    /* packets skipped so the delay cannot grow */
    uint32_t fill_avg_ms;/* audio buffered ahead when a frame was taken */
    uint32_t fill_max_ms;
    uint32_t decode_avg_us;
    uint32_t decode_max_us;
} voice_playout_stats_t;

/* Allocates one instance (PSRAM) with its own Opus decoder; NULL if out of
 * memory. */
voice_playout_t *voice_playout_create(void);

/* Forgets everything queued and starts the next announcement afresh. Any
 * task; the decoder is reset on the next render. */
void voice_playout_reset(voice_playout_t *vp);

/*
 * Queues one packet. seq is the stream's sequence number; len 0 is the
 * server's silence primer (10 ms). From the receive task; never blocks.
 */
void voice_playout_push(voice_playout_t *vp, uint32_t seq, const uint8_t *payload, size_t len);

/* Fills out with `samples` 48 kHz mono samples: decoded voice, concealment
 * or silence. From the audio path, once per frame. */
void voice_playout_render(voice_playout_t *vp, int16_t *out, size_t samples);

/* Returns and clears the counters. */
void voice_playout_take_stats(voice_playout_t *vp, voice_playout_stats_t *out);

#ifdef __cplusplus
}
#endif
