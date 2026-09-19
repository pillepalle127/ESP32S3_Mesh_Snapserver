/**
 * @file voice_announce.c
 * @brief Low-latency voice announcements over UDP:1706. See the header for
 *        the design rationale (why UDP/Opus/level-1 only).
 */
#include "voice_announce.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "audio_i2s.h"
#include "audio_sink.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "opus.h"
#include "snapserver.h"
#include "status_led.h"

static const char *TAG = "VOICE";

/* See TASK_STACK_CAPS in snapserver.c -- same reasoning, duplicated here
 * rather than shared: task stacks in PSRAM, only the TCB stays internal. */
#define TASK_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/*
 * Both tasks decode Opus, and the server task also pushes the announcement
 * mute (snapserver_refresh_announcement() runs the whole ServerSettings
 * send path, log lines included). Opus keeps its big temporaries on its own
 * pseudostack (PSRAM, see the micro-opus Kconfig), but still wants a few kB
 * of real stack. Both stacks are PSRAM too, so being generous is free.
 */
#define VOICE_SERVER_TASK_STACK 10240
#define VOICE_CLIENT_TASK_STACK 10240
/* Same tier as server_task/CLIENT_TASK/SENDER_TASK (5), strictly below
 * audio_task's 6 -- existing priorities are deliberately left untouched,
 * see TODO.md on the reverted priority experiment. */
#define VOICE_TASK_PRIORITY        5

/*
 * The server splits the announcement into two tasks, because relaying and
 * decoding compete for very different things:
 *
 * - voice_server_task only receives, filters and relays. Cheap, and it must
 *   never wait: when it shared a loop with the decoder, every slow decode
 *   held back the next relay, and the level-1 clients -- with idle CPUs of
 *   their own -- ran dry 15 % of the time (2026-09-19). Core 0, next to the
 *   Wi-Fi and lwIP it feeds.
 *
 * - voice_speaker_task decodes for this device's own speaker. Core 1, below
 *   the music encoder: decoding on core 0 once starved IDLE0 for 15 s (task
 *   watchdog), and on core 1 the encoder must keep its deadline. If it falls
 *   behind, the queue between the two fills and this speaker drops packets;
 *   nobody else notices.
 */
#define VOICE_SERVER_TASK_CORE     0
#define VOICE_SPEAKER_TASK_CORE    1
#define VOICE_SPEAKER_TASK_PRIORITY 4
#define VOICE_SPEAKER_TASK_STACK 10240
/* 6 x 20 ms. Deeper would only turn a CPU shortage into latency. */
#define VOICE_SPEAKER_QUEUE_DEPTH    6

/*
 * Wire format on both hops (phone -> server -> level-1 clients): a 4-byte
 * little-endian sequence number, then one Opus packet, 48 kHz mono, as the
 * phone's encoder cut it (normally 20 ms). An empty payload is the server's
 * silence primer, see voice_server_task.
 *
 * Opus rather than raw PCM, which this first used: at 768 kbit/s per
 * level-1 client an announcement needed about nine times the music's
 * airtime, and on device that congested the shared channel -- the music
 * streams collapsed and recovered in bursts, internal heap fell to 14 kB,
 * and the queues it built were the latency one could hear. Opus at voice
 * bitrates is some 25 times smaller for ~10 ms more delay.
 */
#define VOICE_HEADER_BYTES           4U
#define VOICE_MAX_PAYLOAD         1275U   /* largest legal Opus packet */
#define VOICE_MAX_PACKET (VOICE_HEADER_BYTES + VOICE_MAX_PAYLOAD)
#define VOICE_SAMPLE_RATE        48000
#define VOICE_MAX_DECODE_SAMPLES  5760U   /* 120 ms, Opus' longest frame */
#define VOICE_PRIMER_SAMPLES       480U   /* 10 ms of silence per primer */
/* Up to this many lost packets in a row are concealed by Opus' own packet
 * loss concealment instead of leaving a gap; beyond that it is a real
 * dropout, and inventing more audio would only add latency. */
#define VOICE_PLC_MAX_FRAMES         3

#define VOICE_FRAME_US           10000LL

/*
 * A struggling fan-out target must not delay the others: UDP sendto()
 * normally doesn't block, but bound it anyway rather than trust that.
 */
#define VOICE_SEND_TIMEOUT_US    10000

/*
 * The server task wakes at least this often even when nothing arrives, to
 * run the watchdogs and to prime the clients with silence -- neither may
 * wait for the next packet.
 */
#define VOICE_POLL_MS               10

#define VOICE_SILENCE_LIMIT_US   (1000LL * 1000LL)
#define VOICE_MAX_DURATION_US    (180LL * 1000LL * 1000LL)

/*
 * Grace for the *first* packet only. The app arms first and opens the
 * microphone afterwards (capturing before arming would only build a
 * backlog that the clients' small mailbox then carries as permanent extra
 * latency), and Android's VOICE_COMMUNICATION capture can take several
 * hundred ms to start on some phones. Once packets flow, a gap of
 * VOICE_SILENCE_LIMIT_US ends the announcement.
 */
#define VOICE_START_GRACE_US     (3000LL * 1000LL)

/* How often the level-1 targets and the announcement mute are re-checked
 * while an announcement runs -- a client may change level mid-way. */
#define VOICE_REFRESH_US         (1000LL * 1000LL)

/*
 * Client side: a gap at least this long between packets starts a new
 * session, so the sequence number and the decoder are reset. The server
 * numbers its stream continuously, but restarts from 0 when it reboots;
 * without this a client that remembers a high number would throw away
 * everything after the reboot as "stale". Same value as audio_sink.c's
 * VOICE_INACTIVITY_TIMEOUT_US, which releases the overlay after the same gap.
 */
#define VOICE_SESSION_GAP_US   (400LL * 1000LL)

/* ------------------------------------------------------------------ */
/* Opus decoding, shared by both roles                                */
/* ------------------------------------------------------------------ */

typedef struct {
    OpusDecoder *decoder;
    int16_t *pcm;              /* VOICE_MAX_DECODE_SAMPLES, PSRAM      */
    int last_frame_samples;    /* frame size to conceal a lost packet  */
    /* Decode cost of the current announcement, logged at its end. */
    uint32_t decode_count;
    int64_t decode_total_us;
    int64_t decode_max_us;
} voice_decoder_t;

static esp_err_t voice_decoder_init(voice_decoder_t *d)
{
    int err = OPUS_OK;
    d->decoder = opus_decoder_create(VOICE_SAMPLE_RATE, 1, &err);
    if (d->decoder == NULL || err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_decoder_create failed: %s (%d)", opus_strerror(err), err);
        d->decoder = NULL;
        return ESP_FAIL;
    }

    d->pcm = heap_caps_malloc(VOICE_MAX_DECODE_SAMPLES * sizeof(int16_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (d->pcm == NULL) {
        d->pcm = heap_caps_malloc(VOICE_MAX_DECODE_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT);
    }
    if (d->pcm == NULL) {
        opus_decoder_destroy(d->decoder);
        d->decoder = NULL;
        return ESP_ERR_NO_MEM;
    }

    d->last_frame_samples = 960;
    return ESP_OK;
}

/* Logs and clears the decode cost since the last reset. */
static void voice_decoder_log_stats(voice_decoder_t *d, const char *who)
{
    if (d->decode_count == 0) {
        return;
    }
    ESP_LOGI(TAG, "%s decode: packets=%lu avg=%lld us max=%lld us",
             who, (unsigned long)d->decode_count,
             d->decode_total_us / d->decode_count, d->decode_max_us);
    d->decode_count = 0;
    d->decode_total_us = 0;
    d->decode_max_us = 0;
}

static void voice_decoder_reset(voice_decoder_t *d)
{
    (void)opus_decoder_ctl(d->decoder, OPUS_RESET_STATE);
    d->last_frame_samples = 960;
}

static int voice_decode_raw(voice_decoder_t *d, const uint8_t *payload, size_t len)
{
    int n;
    if (payload == NULL) {
        n = opus_decode(d->decoder, NULL, 0, d->pcm, d->last_frame_samples, 0);
    } else {
        n = opus_decode(d->decoder, payload, (opus_int32)len,
                        d->pcm, (int)VOICE_MAX_DECODE_SAMPLES, 0);
        if (n > 0) {
            d->last_frame_samples = n;
        }
    }
    return (n > 0) ? n : 0;
}

/* Decodes one payload into d->pcm, or conceals one lost packet when payload
 * is NULL. Returns the number of samples, 0 if the packet was unusable. */
static int voice_decode(voice_decoder_t *d, const uint8_t *payload, size_t len)
{
    const int64_t t0 = esp_timer_get_time();
    const int n = voice_decode_raw(d, payload, len);
    const int64_t dt = esp_timer_get_time() - t0;
    ++d->decode_count;
    d->decode_total_us += dt;
    if (dt > d->decode_max_us) {
        d->decode_max_us = dt;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Server: control state, relay, watchdogs                            */
/* ------------------------------------------------------------------ */

static int s_server_fd = -1;
static TaskHandle_t s_server_task;
static voice_decoder_t s_server_decoder;

/*
 * Announcement state. Written only with s_state_mutex held, so a stop can
 * never interleave with a start. A FreeRTOS mutex rather than a spinlock:
 * the holders log, and nothing here is time-critical enough to justify
 * disabling interrupts. Start and stop themselves only flip state -- every
 * send they cause (mute pushes, silence priming) happens in
 * voice_server_task, never in the JSON-RPC handler, whose 6 kB stack
 * overflowed once already.
 */
static SemaphoreHandle_t s_state_mutex;
static bool s_active;
static int s_owner_fd = -1;
static uint32_t s_owner_ip;   /* network order; 0 = accept any sender */
static uint32_t s_session;    /* bumped by every start */
static int64_t s_started_us;

/*
 * Fan-out targets. Written and read by voice_server_task only now, but kept
 * behind its own lock so the sendto() loop works on a copy -- sendto() must
 * never run inside a cross-core spinlock.
 */
static portMUX_TYPE s_level1_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_level1_ips[SNAPSERVER_MAX_CLIENTS][16];
static size_t s_level1_count;

static size_t refresh_level1_ips(void)
{
    char local_ips[SNAPSERVER_MAX_CLIENTS][16];
    const size_t count =
        snapserver_get_level1_client_ips(local_ips, SNAPSERVER_MAX_CLIENTS);

    portENTER_CRITICAL(&s_level1_lock);
    memcpy(s_level1_ips, local_ips, sizeof(local_ips));
    s_level1_count = count;
    portEXIT_CRITICAL(&s_level1_lock);

    return count;
}

/* Call with s_state_mutex held. Unmuting the others is voice_server_task's
 * job: it sees the state change within VOICE_POLL_MS and pushes it. */
static void stop_locked(const char *reason)
{
    if (!s_active) {
        return;
    }
    s_active = false;
    s_owner_fd = -1;
    audio_i2s_set_voice_active(false);
    snapserver_set_announcement(false);
    status_led_set_state(STATUS_LED_PLAYING);
    ESP_LOGI(TAG, "Announcement stopped: %s", reason);
}

bool voice_announce_rpc_start(int fd)
{
    if (s_state_mutex == NULL) {
        return false;
    }

    /*
     * Only the phone that armed the announcement may feed it. Behind NAPT
     * that is its relay's address, the same for its TCP and its UDP, so the
     * comparison still holds there.
     */
    uint32_t owner_ip = 0;
    struct sockaddr_in peer = {0};
    socklen_t peer_len = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &peer_len) == 0) {
        owner_ip = peer.sin_addr.s_addr;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_active) {
        xSemaphoreGive(s_state_mutex);
        return false;
    }
    s_active = true;
    s_owner_fd = fd;
    s_owner_ip = owner_ip;
    ++s_session;
    s_started_us = esp_timer_get_time();
    audio_i2s_set_voice_active(true);
    snapserver_set_announcement(true);
    status_led_set_state(STATUS_LED_VOICE_ANNOUNCEMENT);
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "Announcement started by fd=%d", fd);
    return true;
}

bool voice_announce_rpc_stop(int fd)
{
    (void)fd; /* any connection may stop, see the header */
    if (s_state_mutex != NULL) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        stop_locked("Voice.Stop");
        xSemaphoreGive(s_state_mutex);
    }
    return true;
}

void voice_announce_on_control_disconnect(int fd)
{
    if (s_state_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_active && s_owner_fd == fd) {
        stop_locked("owning control connection closed");
    }
    xSemaphoreGive(s_state_mutex);
}

/*
 * Sends one packet to every level-1 client, stamped with the server's own
 * running sequence number. Rewriting it makes the clients see one
 * continuous stream -- silence primer and phone audio alike, and across a
 * phone app that restarts its numbering -- so their "drop anything not
 * newer" check stays simple. Reordering on the phone's side is filtered
 * here, before renumbering.
 */
static void relay_to_level1(uint8_t *packet, size_t len, uint32_t *out_sequence)
{
    const uint32_t sequence = (*out_sequence)++;
    memcpy(packet, &sequence, sizeof(sequence));

    char ips[SNAPSERVER_MAX_CLIENTS][16];
    size_t count;
    portENTER_CRITICAL(&s_level1_lock);
    count = s_level1_count;
    memcpy(ips, s_level1_ips, sizeof(ips));
    portEXIT_CRITICAL(&s_level1_lock);

    for (size_t i = 0; i < count; ++i) {
        struct sockaddr_in dst = {0};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(VOICE_ANNOUNCE_PORT);
        if (inet_pton(AF_INET, ips[i], &dst.sin_addr) != 1) {
            continue;
        }
        (void)sendto(s_server_fd, packet, len, 0,
                     (struct sockaddr *)&dst, sizeof(dst));
    }
}

/*
 * One message from the relay to the speaker task: an Opus payload, how many
 * packets were lost right before it, or a control marker.
 */
#define SPEAKER_MSG_AUDIO   0
#define SPEAKER_MSG_RESET   1   /* new announcement: fresh decoder state */
#define SPEAKER_MSG_END     2   /* announcement over: log decode stats   */

typedef struct {
    uint8_t kind;
    uint8_t lost;
    uint16_t len;
    uint8_t payload[VOICE_MAX_PAYLOAD];
} speaker_msg_t;

static QueueHandle_t s_speaker_queue;
static StaticQueue_t s_speaker_queue_ctrl;      /* internal RAM, small   */
static volatile uint32_t s_speaker_dropped;     /* queue full: not played */

/* Relay side: never blocks, a full queue just loses this packet for the
 * own speaker. Markers are worth a short wait, they are rare. */
static void speaker_post(const speaker_msg_t *msg)
{
    const TickType_t wait = (msg->kind == SPEAKER_MSG_AUDIO) ? 0 : pdMS_TO_TICKS(20);
    if (xQueueSend(s_speaker_queue, msg, wait) != pdTRUE && msg->kind == SPEAKER_MSG_AUDIO) {
        ++s_speaker_dropped;
    }
}

static void voice_speaker_task(void *arg)
{
    (void)arg;
    /* PSRAM: 1.3 kB is too much for internal RAM and too much for a stack
     * that is itself in PSRAM anyway. */
    speaker_msg_t *msg = heap_caps_malloc(sizeof(*msg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (msg == NULL) {
        ESP_LOGE(TAG, "voice_speaker_task: out of memory");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        if (xQueueReceive(s_speaker_queue, msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg->kind == SPEAKER_MSG_RESET) {
            voice_decoder_reset(&s_server_decoder);
            s_speaker_dropped = 0;
            continue;
        }
        if (msg->kind == SPEAKER_MSG_END) {
            voice_decoder_log_stats(&s_server_decoder, "Server");
            if (s_speaker_dropped != 0U) {
                ESP_LOGW(TAG, "Server speaker could not keep up: %lu packet(s) not played",
                         (unsigned long)s_speaker_dropped);
            }
            continue;
        }

        if (msg->lost > 0 && msg->lost <= VOICE_PLC_MAX_FRAMES) {
            for (uint8_t i = 0; i < msg->lost; ++i) {
                const int samples = voice_decode(&s_server_decoder, NULL, 0);
                audio_i2s_feed_voice(s_server_decoder.pcm, (size_t)samples);
            }
        }
        const int samples = voice_decode(&s_server_decoder, msg->payload, msg->len);
        if (samples > 0) {
            audio_i2s_feed_voice(s_server_decoder.pcm, (size_t)samples);
        }
    }
}

static void voice_server_task(void *arg)
{
    (void)arg;
    /* PSRAM, not static: a static would land in scarce internal RAM. */
    speaker_msg_t *msg = heap_caps_malloc(sizeof(*msg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (msg == NULL) {
        ESP_LOGE(TAG, "voice_server_task: out of memory");
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[VOICE_MAX_PACKET + 32];
    uint8_t primer[VOICE_HEADER_BYTES];

    uint32_t seen_session = 0;
    bool was_active = false;
    bool have_audio = false;
    bool have_phone_sequence = false;
    uint32_t phone_sequence = 0;
    uint32_t out_sequence = 0;
    int64_t last_audio_us = 0;
    int64_t last_primer_us = 0;
    int64_t last_refresh_us = 0;

    for (;;) {
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        /* Times out after VOICE_POLL_MS, see there. */
        const int n = recvfrom(s_server_fd, buf, sizeof(buf), 0,
                               (struct sockaddr *)&from, &from_len);
        const int64_t now = esp_timer_get_time();

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        const bool active = s_active;
        const uint32_t session = s_session;
        const uint32_t owner_ip = s_owner_ip;
        const int64_t started_us = s_started_us;
        xSemaphoreGive(s_state_mutex);

        /*
         * An announcement started or ended since the last round: find the
         * level-1 targets and push the mute change to the other clients
         * right away, rather than on the next periodic refresh.
         */
        if (active != was_active || session != seen_session) {
            if (session != seen_session) {
                seen_session = session;
                have_audio = false;
                have_phone_sequence = false;
                msg->kind = SPEAKER_MSG_RESET;
                speaker_post(msg);
            }
            was_active = active;
            const size_t targets = refresh_level1_ips();
            snapserver_refresh_announcement();
            last_refresh_us = now;
            if (active) {
                ESP_LOGI(TAG, "Announcement: %u level-1 target(s)", (unsigned)targets);
            } else {
                msg->kind = SPEAKER_MSG_END;
                speaker_post(msg);
            }
        }

        if (!active) {
            continue;
        }

        /* Phone audio: a non-empty Opus payload from the owner. */
        bool got_audio = false;
        uint32_t lost = 0;
        if (n > (int)VOICE_HEADER_BYTES && n <= (int)VOICE_MAX_PACKET &&
            (owner_ip == 0 || from.sin_addr.s_addr == owner_ip)) {
            uint32_t sequence;
            memcpy(&sequence, buf, sizeof(sequence));
            /* Wraparound-safe: only newer packets, reordered ones are dropped. */
            const int32_t ahead = (int32_t)(sequence - phone_sequence);
            if (!have_phone_sequence || ahead > 0) {
                lost = (have_phone_sequence && ahead > 1) ? (uint32_t)(ahead - 1) : 0;
                phone_sequence = sequence;
                have_phone_sequence = true;
                got_audio = true;
                have_audio = true;
                last_audio_us = now;
            }
        }

        const char *stop_reason = NULL;
        if (now - started_us > VOICE_MAX_DURATION_US) {
            stop_reason = "3 min limit reached";
        } else if (have_audio && now - last_audio_us > VOICE_SILENCE_LIMIT_US) {
            stop_reason = "no audio for 1 s";
        } else if (!have_audio && now - started_us > VOICE_START_GRACE_US) {
            stop_reason = "no audio within 3 s of the start";
        }
        if (stop_reason != NULL) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            if (s_active && s_session == session) {
                stop_locked(stop_reason);
            }
            xSemaphoreGive(s_state_mutex);
            continue; /* the next round sees !active and unmutes */
        }

        if (now - last_refresh_us > VOICE_REFRESH_US) {
            (void)refresh_level1_ips();
            snapserver_refresh_announcement();
            last_refresh_us = now;
        }

        if (got_audio) {
            /* The clients decode for themselves -- relayed untouched but
             * for the sequence number. */
            relay_to_level1(buf, (size_t)n, &out_sequence);

            /* This device's own speaker, decoded by voice_speaker_task. */
            msg->kind = SPEAKER_MSG_AUDIO;
            msg->lost = (lost > 255U) ? 255U : (uint8_t)lost;
            msg->len = (uint16_t)((size_t)n - VOICE_HEADER_BYTES);
            memcpy(msg->payload, buf + VOICE_HEADER_BYTES, msg->len);
            speaker_post(msg);
        } else if (!have_audio && now - last_primer_us >= VOICE_FRAME_US) {
            /*
             * Until the phone's microphone is up, level-1 clients get an
             * empty packet -- 10 ms of silence -- at the normal rate: it
             * switches them to the announcement immediately, so their music
             * stops together with everyone else's instead of running on for
             * however long the phone takes.
             */
            relay_to_level1(primer, sizeof(primer), &out_sequence);
            last_primer_us = now;
        }
    }
}

esp_err_t voice_announce_start(void)
{
    if (s_server_fd >= 0) {
        return ESP_OK; /* idempotent */
    }

    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
        if (s_state_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_server_decoder.decoder == NULL) {
        const esp_err_t err = voice_decoder_init(&s_server_decoder);
        if (err != ESP_OK) {
            return err;
        }
    }

    s_server_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_server_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        return ESP_FAIL;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(VOICE_ANNOUNCE_PORT);
    if (bind(s_server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() on UDP:%d failed: errno=%d", VOICE_ANNOUNCE_PORT, errno);
        close(s_server_fd);
        s_server_fd = -1;
        return ESP_FAIL;
    }

    const struct timeval snd_timeout = { .tv_sec = 0, .tv_usec = VOICE_SEND_TIMEOUT_US };
    (void)setsockopt(s_server_fd, SOL_SOCKET, SO_SNDTIMEO, &snd_timeout, sizeof(snd_timeout));
    const struct timeval rcv_timeout = { .tv_sec = 0, .tv_usec = VOICE_POLL_MS * 1000 };
    (void)setsockopt(s_server_fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

    /* Queue storage (~8 kB) in PSRAM, only the control block internal --
     * same split as the per-client TX queues in snapserver.c. */
    uint8_t *queue_storage = heap_caps_malloc(VOICE_SPEAKER_QUEUE_DEPTH * sizeof(speaker_msg_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (queue_storage != NULL) {
        s_speaker_queue = xQueueCreateStatic(VOICE_SPEAKER_QUEUE_DEPTH, sizeof(speaker_msg_t),
                                             queue_storage, &s_speaker_queue_ctrl);
    }
    if (s_speaker_queue == NULL ||
        xTaskCreatePinnedToCoreWithCaps(voice_speaker_task,
                                        "voice_speaker",
                                        VOICE_SPEAKER_TASK_STACK,
                                        NULL,
                                        VOICE_SPEAKER_TASK_PRIORITY,
                                        NULL,
                                        VOICE_SPEAKER_TASK_CORE,
                                        TASK_STACK_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "Could not create voice_speaker_task");
        close(s_server_fd);
        s_server_fd = -1;
        return ESP_FAIL;
    }

    if (xTaskCreatePinnedToCoreWithCaps(voice_server_task,
                                        "voice_server",
                                        VOICE_SERVER_TASK_STACK,
                                        NULL,
                                        VOICE_TASK_PRIORITY,
                                        &s_server_task,
                                        VOICE_SERVER_TASK_CORE,
                                        TASK_STACK_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "Could not create voice_server_task");
        close(s_server_fd);
        s_server_fd = -1;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Voice announcement server listening on UDP:%d (Opus)", VOICE_ANNOUNCE_PORT);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Client: UDP receive, decode, into audio_sink.c's voice mailbox     */
/* ------------------------------------------------------------------ */

static int s_client_fd = -1;
static TaskHandle_t s_client_task;
static voice_decoder_t s_client_decoder;

static void voice_client_task(void *arg)
{
    (void)arg;

    uint8_t buf[VOICE_MAX_PACKET + 32];
    uint32_t last_sequence = 0;
    bool have_sequence = false;
    int64_t last_packet_us = 0;

    for (;;) {
        const int n = recvfrom(s_client_fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n < (int)VOICE_HEADER_BYTES || n > (int)VOICE_MAX_PACKET) {
            continue;
        }

        uint32_t sequence;
        memcpy(&sequence, buf, sizeof(sequence));

        const int64_t now = esp_timer_get_time();
        if (now - last_packet_us > VOICE_SESSION_GAP_US) {
            /* New session, see VOICE_SESSION_GAP_US. */
            have_sequence = false;
            voice_decoder_log_stats(&s_client_decoder, "Previous announcement");
            voice_decoder_reset(&s_client_decoder);
        }
        last_packet_us = now;

        /*
         * Standard wraparound-safe sequence comparison: drop anything not
         * newer than what was already played, matching "verwerfen statt
         * warten" for reordered/duplicate/stale packets on the mesh.
         */
        const int32_t ahead = (int32_t)(sequence - last_sequence);
        if (have_sequence && ahead <= 0) {
            continue;
        }
        const uint32_t lost = (have_sequence && ahead > 1) ? (uint32_t)(ahead - 1) : 0;
        last_sequence = sequence;
        have_sequence = true;

        const size_t payload_len = (size_t)n - VOICE_HEADER_BYTES;
        if (payload_len == 0) {
            /* The server's silence primer. */
            memset(s_client_decoder.pcm, 0, VOICE_PRIMER_SAMPLES * sizeof(int16_t));
            audio_sink_feed_voice(s_client_decoder.pcm, VOICE_PRIMER_SAMPLES);
            continue;
        }

        if (lost > 0 && lost <= VOICE_PLC_MAX_FRAMES) {
            for (uint32_t i = 0; i < lost; ++i) {
                const int samples = voice_decode(&s_client_decoder, NULL, 0);
                audio_sink_feed_voice(s_client_decoder.pcm, (size_t)samples);
            }
        }
        const int samples = voice_decode(&s_client_decoder, buf + VOICE_HEADER_BYTES, payload_len);
        if (samples > 0) {
            audio_sink_feed_voice(s_client_decoder.pcm, (size_t)samples);
        }
    }
}

esp_err_t voice_receive_start(void)
{
    if (s_client_fd >= 0) {
        return ESP_OK; /* idempotent */
    }

    if (s_client_decoder.decoder == NULL) {
        const esp_err_t err = voice_decoder_init(&s_client_decoder);
        if (err != ESP_OK) {
            return err;
        }
    }

    s_client_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_client_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        return ESP_FAIL;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(VOICE_ANNOUNCE_PORT);
    if (bind(s_client_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() on UDP:%d failed: errno=%d", VOICE_ANNOUNCE_PORT, errno);
        close(s_client_fd);
        s_client_fd = -1;
        return ESP_FAIL;
    }

    if (xTaskCreatePinnedToCoreWithCaps(voice_client_task,
                                        "voice_client",
                                        VOICE_CLIENT_TASK_STACK,
                                        NULL,
                                        VOICE_TASK_PRIORITY,
                                        &s_client_task,
                                        1,
                                        TASK_STACK_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "Could not create voice_client_task");
        close(s_client_fd);
        s_client_fd = -1;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Voice announcement receiver listening on UDP:%d (Opus)", VOICE_ANNOUNCE_PORT);
    return ESP_OK;
}
