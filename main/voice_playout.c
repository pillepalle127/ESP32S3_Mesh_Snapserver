/**
 * @file voice_playout.c
 * @brief Plays an announcement from its Opus packets at a fixed delay, see
 *        voice_playout.h.
 */
#include "voice_playout.h"

#include <stdlib.h>
#include <string.h>

#ifdef VOICE_PLAYOUT_HOST
/* Host test (scratchpad): no FreeRTOS, a stand-in decoder. */
typedef int lock_t;
#define LOCK_INIT(l) ((void)(l))
#define LOCK(l)      ((void)(l))
#define UNLOCK(l)    ((void)(l))
int host_decode(void *dec, const uint8_t *payload, size_t len, int16_t *pcm, int max_samples);
int host_conceal(void *dec, int16_t *pcm, int samples);
int host_duration(const uint8_t *payload, size_t len);
void *host_decoder_create(void);
void host_decoder_reset(void *dec);
int64_t host_time_us(void);
#define NOW_US()     host_time_us()
#define ALLOC(size)  calloc(1, (size))
#else
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "opus.h"
typedef portMUX_TYPE lock_t;
#define LOCK_INIT(l) portMUX_INITIALIZE(l)
#define LOCK(l)      portENTER_CRITICAL(l)
#define UNLOCK(l)    portEXIT_CRITICAL(l)
#define NOW_US()     esp_timer_get_time()
#define ALLOC(size)  heap_caps_calloc(1, (size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif

#define SAMPLE_RATE        48000
#define MAX_PAYLOAD        1275U    /* largest legal Opus packet */
#define MAX_DECODE_SAMPLES 5760     /* 120 ms, Opus' longest frame */
#define PRIMER_SAMPLES     480      /* the server's empty packet: 10 ms */

/* Room for a burst behind a gap: 8 packets, 160 ms of 20 ms speech. */
#define SLOTS              8U

/*
 * Playback starts once this much is queued: one 20 ms frame, the same
 * prefill as the PCM mailbox it replaces, so the delay does not change.
 */
#define PREFILL_SAMPLES    960

/*
 * Up to this many frames in a row are concealed, 100 ms; beyond that it is
 * silence until packets come again. Opus fades its concealment out on its
 * own, so a longer run only ends quieter, never louder.
 */
#define PLC_MAX_FRAMES     5U

/*
 * Catching up after a stall. The queue's target is the prefill, one frame.
 * Once it has stayed above that for TRIM_AFTER_FRAMES, one packet is
 * skipped -- the smallest of the next TRIM_LOOKAHEAD, which in speech is the
 * one most likely to be a pause. Spread out instead of one audible cut as
 * before; but the further above target, the faster: from TRIM_FAST_SAMPLES
 * one per frame, and from TRIM_RUSH_SAMPLES two. Without that the queue
 * stayed at 130 ms through a burst of radio trouble (2026-10-10).
 *
 * Host simulation against the old mailbox under the same arrivals: mean
 * delay equal or lower (e.g. 120 ms stall per second: 45 against 46 ms,
 * peaks 130 against 150 ms), no silence inside the speech, about as many
 * packets lost, preferably pauses.
 */
#define TARGET_SAMPLES     960
#define TRIM_AFTER_FRAMES  2U
#define TRIM_FAST_SAMPLES  (3 * 960)
#define TRIM_RUSH_SAMPLES  (5 * 960)
#define TRIM_LOOKAHEAD     4U

/* A packet this far ahead of the one that is due belongs to a new stream:
 * start over instead of concealing the gap. */
#define RESYNC_AHEAD       64

typedef struct {
    bool     used;
    bool     skip;          /* chosen for the catch-up: drop when due */
    uint32_t seq;
    uint16_t len;
    uint16_t samples;
    uint8_t *data;          /* MAX_PAYLOAD bytes in s->payloads */
} slot_t;

struct voice_playout {
    lock_t   lock;
    slot_t   slots[SLOTS];
    uint8_t *payloads;

    /* Shared with push() under the lock. */
    bool     started;
    uint32_t next_seq;       /* the packet that plays next */
    bool     reset_decoder;  /* reset() asked for a fresh decoder */

    /* Render side only. */
    void    *decoder;
    uint32_t plc_run;
    uint32_t above_target;   /* frames the queue stayed above TARGET_SAMPLES */
    int      last_samples;   /* duration of the last frame, for concealment */
    int16_t *pcm;            /* MAX_DECODE_SAMPLES */
    int      pcm_len;
    int      pcm_pos;
    uint8_t  packet[MAX_PAYLOAD];

    /* Counters, under the lock. */
    voice_playout_stats_t stats;
    uint64_t fill_sum_ms;
    uint32_t fill_count;
    uint64_t decode_sum_us;
    uint32_t decode_count;
};

#ifndef VOICE_PLAYOUT_HOST
static void *dec_create(void)
{
    int err = OPUS_OK;
    OpusDecoder *dec = opus_decoder_create(SAMPLE_RATE, 1, &err);
    return (err == OPUS_OK) ? dec : NULL;
}

static void dec_reset(void *dec)
{
    (void)opus_decoder_ctl((OpusDecoder *)dec, OPUS_RESET_STATE);
}

static int dec_decode(void *dec, const uint8_t *payload, size_t len, int16_t *pcm, int max_samples)
{
    const int n = opus_decode((OpusDecoder *)dec, payload, (opus_int32)len, pcm, max_samples, 0);
    return (n > 0) ? n : 0;
}

static int dec_conceal(void *dec, int16_t *pcm, int samples)
{
    const int n = opus_decode((OpusDecoder *)dec, NULL, 0, pcm, samples, 0);
    return (n > 0) ? n : 0;
}

static int pkt_duration(const uint8_t *payload, size_t len)
{
    const int n = opus_packet_get_nb_samples(payload, (opus_int32)len, SAMPLE_RATE);
    return (n > 0) ? n : 960;
}
#else
#define dec_create host_decoder_create
#define dec_reset  host_decoder_reset
#define dec_decode     host_decode
#define dec_conceal    host_conceal
#define pkt_duration   host_duration
#endif

voice_playout_t *voice_playout_create(void)
{
    voice_playout_t *vp = ALLOC(sizeof(*vp));
    if (vp == NULL) {
        return NULL;
    }
    vp->payloads = ALLOC(SLOTS * MAX_PAYLOAD);
    vp->pcm = ALLOC(MAX_DECODE_SAMPLES * sizeof(int16_t));
    vp->decoder = (vp->payloads != NULL && vp->pcm != NULL) ? dec_create() : NULL;
    if (vp->decoder == NULL) {
        free(vp->payloads);
        free(vp->pcm);
        free(vp);
        return NULL;
    }
    LOCK_INIT(&vp->lock);
    for (size_t i = 0; i < SLOTS; ++i) {
        vp->slots[i].data = vp->payloads + i * MAX_PAYLOAD;
    }
    vp->last_samples = 960;
    return vp;
}

void voice_playout_reset(voice_playout_t *vp)
{
    if (vp == NULL) {
        return;
    }
    LOCK(&vp->lock);
    for (size_t i = 0; i < SLOTS; ++i) {
        vp->slots[i].used = false;
    }
    vp->started = false;
    vp->reset_decoder = true;
    UNLOCK(&vp->lock);
}

/* Queued audio beyond the packet that is due, in samples. Under the lock. */
static int queued_ahead(const voice_playout_t *vp)
{
    int samples = 0;
    for (size_t i = 0; i < SLOTS; ++i) {
        if (vp->slots[i].used && !vp->slots[i].skip &&
            (!vp->started || (int32_t)(vp->slots[i].seq - vp->next_seq) >= 0)) {
            samples += vp->slots[i].samples;
        }
    }
    return samples;
}

void voice_playout_push(voice_playout_t *vp, uint32_t seq, const uint8_t *payload, size_t len)
{
    if (vp == NULL || len > MAX_PAYLOAD) {
        return;
    }
    const uint16_t samples = (len == 0U) ? PRIMER_SAMPLES : (uint16_t)pkt_duration(payload, len);

    LOCK(&vp->lock);
    if (vp->started) {
        const int32_t ahead = (int32_t)(seq - vp->next_seq);
        if (ahead < 0) {
            /* Its slot has played, concealed or not: too late. */
            ++vp->stats.late;
            UNLOCK(&vp->lock);
            return;
        }
        if (ahead >= RESYNC_AHEAD) {
            for (size_t i = 0; i < SLOTS; ++i) {
                vp->slots[i].used = false;
            }
            vp->started = false;
        }
    }

    slot_t *free_slot = NULL;
    slot_t *oldest = NULL;
    for (size_t i = 0; i < SLOTS; ++i) {
        slot_t *s = &vp->slots[i];
        if (s->used && s->seq == seq) {
            UNLOCK(&vp->lock); /* duplicate */
            return;
        }
        if (!s->used) {
            if (free_slot == NULL) {
                free_slot = s;
            }
        } else if (oldest == NULL || (int32_t)(s->seq - oldest->seq) < 0) {
            oldest = s;
        }
    }
    if (free_slot == NULL) {
        /* Full: more than 160 ms queued, the oldest has waited longest. */
        free_slot = oldest;
        ++vp->stats.trimmed;
    }
    free_slot->used = true;
    free_slot->skip = false;
    free_slot->seq = seq;
    free_slot->len = (uint16_t)len;
    free_slot->samples = samples;
    if (len > 0U) {
        memcpy(free_slot->data, payload, len);
    }
    UNLOCK(&vp->lock);
}

/* Decides and decodes the next frame into vp->pcm. */
static void next_frame(voice_playout_t *vp)
{
    bool have = false;
    size_t len = 0;

    LOCK(&vp->lock);
    if (!vp->started && queued_ahead(vp) >= PREFILL_SAMPLES) {
        uint32_t first = 0;
        bool any = false;
        for (size_t i = 0; i < SLOTS; ++i) {
            if (vp->slots[i].used && (!any || (int32_t)(vp->slots[i].seq - first) < 0)) {
                first = vp->slots[i].seq;
                any = true;
            }
        }
        vp->started = any;
        vp->next_seq = first;
        vp->plc_run = 0;
    }
    if (!vp->started) {
        UNLOCK(&vp->lock);
        vp->pcm_len = 0;
        return;
    }

    /*
     * The packet that is due. Packets picked for the catch-up are dropped
     * on the way. If it is missing but later ones are here, it is lost (or
     * hopelessly late): conceal its slot and move on. If nothing is here at
     * all, the stream has stalled: conceal, but wait for it -- it usually
     * comes, late but complete. Only as long as concealment lasts, though:
     * after that the clock runs on and whatever comes late is dropped, so a
     * long stall cannot pile up delay.
     */
    for (;;) {
        slot_t *due = NULL;
        bool later = false;
        for (size_t i = 0; i < SLOTS; ++i) {
            slot_t *s = &vp->slots[i];
            if (!s->used) {
                continue;
            }
            if (s->seq == vp->next_seq) {
                due = s;
            } else if ((int32_t)(s->seq - vp->next_seq) > 0) {
                later = true;
            }
        }
        if (due != NULL && due->skip) {
            due->used = false;
            ++vp->next_seq;
            continue;
        }
        if (due != NULL) {
            len = due->len;
            if (len > 0U) {
                memcpy(vp->packet, due->data, len);
            }
            due->used = false;
            have = true;
            ++vp->next_seq;
        } else if (later || vp->plc_run >= PLC_MAX_FRAMES) {
            ++vp->next_seq;
        }
        break;
    }

    const int ahead = queued_ahead(vp);
    vp->above_target = (ahead > TARGET_SAMPLES) ? vp->above_target + 1U : 0U;
    if (vp->above_target >= TRIM_AFTER_FRAMES || ahead > TRIM_FAST_SAMPLES) {
        const unsigned picks = (ahead > TRIM_RUSH_SAMPLES) ? 2U : 1U;
        for (unsigned k = 0; k < picks; ++k) {
            slot_t *victim = NULL;
            for (size_t i = 0; i < SLOTS; ++i) {
                slot_t *s = &vp->slots[i];
                const int32_t pos = (int32_t)(s->seq - vp->next_seq);
                if (s->used && !s->skip && pos >= 0 && pos < (int32_t)TRIM_LOOKAHEAD &&
                    (victim == NULL || s->len < victim->len)) {
                    victim = s;
                }
            }
            if (victim != NULL) {
                victim->skip = true;
                ++vp->stats.trimmed;
            }
        }
        vp->above_target = 0;
    }
    const uint32_t fill_ms = (uint32_t)(ahead * 1000 / SAMPLE_RATE);
    vp->fill_sum_ms += fill_ms;
    ++vp->fill_count;
    if (fill_ms > vp->stats.fill_max_ms) {
        vp->stats.fill_max_ms = fill_ms;
    }
    UNLOCK(&vp->lock);

    enum { PLAYED, CONCEALED, SILENT } kind;
    if (have) {
        kind = PLAYED;
    } else if (vp->plc_run < PLC_MAX_FRAMES) {
        kind = CONCEALED;
    } else {
        kind = SILENT;
    }

    const int64_t t0 = NOW_US();
    int n = 0;
    if (kind == PLAYED) {
        if (len == 0U) {
            n = PRIMER_SAMPLES;
            memset(vp->pcm, 0, (size_t)n * sizeof(int16_t));
        } else {
            n = dec_decode(vp->decoder, vp->packet, len, vp->pcm, MAX_DECODE_SAMPLES);
        }
        vp->plc_run = 0;
        if (n > 0) {
            vp->last_samples = n;
        }
    } else if (kind == CONCEALED) {
        n = dec_conceal(vp->decoder, vp->pcm, vp->last_samples);
        ++vp->plc_run;
    }
    const int64_t took = NOW_US() - t0;

    LOCK(&vp->lock);
    if (kind == PLAYED) {
        ++vp->stats.played;
    } else if (kind == CONCEALED) {
        ++vp->stats.concealed;
    } else {
        ++vp->stats.silent;
    }
    if (kind != SILENT) {
        vp->decode_sum_us += (uint64_t)took;
        ++vp->decode_count;
        if ((uint32_t)took > vp->stats.decode_max_us) {
            vp->stats.decode_max_us = (uint32_t)took;
        }
    }
    UNLOCK(&vp->lock);

    if (n <= 0) {
        /* Undecodable packet: a frame of silence keeps the clock. */
        n = vp->last_samples;
        memset(vp->pcm, 0, (size_t)n * sizeof(int16_t));
    }
    vp->pcm_len = n;
    vp->pcm_pos = 0;
}

void voice_playout_render(voice_playout_t *vp, int16_t *out, size_t samples)
{
    if (vp == NULL) {
        memset(out, 0, samples * sizeof(int16_t));
        return;
    }

    LOCK(&vp->lock);
    const bool fresh = vp->reset_decoder;
    vp->reset_decoder = false;
    UNLOCK(&vp->lock);
    if (fresh) {
        dec_reset(vp->decoder);
        vp->pcm_len = vp->pcm_pos = 0;
        vp->plc_run = 0;
        vp->above_target = 0;
        vp->last_samples = 960;
    }

    size_t done = 0;
    while (done < samples) {
        if (vp->pcm_pos < vp->pcm_len) {
            size_t n = (size_t)(vp->pcm_len - vp->pcm_pos);
            if (n > samples - done) {
                n = samples - done;
            }
            memcpy(out + done, vp->pcm + vp->pcm_pos, n * sizeof(int16_t));
            vp->pcm_pos += (int)n;
            done += n;
            continue;
        }
        next_frame(vp);
        if (vp->pcm_len == 0) {
            /* Not started yet: silence until the prefill is in. */
            memset(out + done, 0, (samples - done) * sizeof(int16_t));
            return;
        }
    }
}

void voice_playout_take_stats(voice_playout_t *vp, voice_playout_stats_t *out)
{
    if (vp == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    LOCK(&vp->lock);
    *out = vp->stats;
    out->fill_avg_ms = vp->fill_count ? (uint32_t)(vp->fill_sum_ms / vp->fill_count) : 0U;
    out->decode_avg_us = vp->decode_count ? (uint32_t)(vp->decode_sum_us / vp->decode_count) : 0U;
    memset(&vp->stats, 0, sizeof(vp->stats));
    vp->fill_sum_ms = 0;
    vp->fill_count = 0;
    vp->decode_sum_us = 0;
    vp->decode_count = 0;
    UNLOCK(&vp->lock);
}
