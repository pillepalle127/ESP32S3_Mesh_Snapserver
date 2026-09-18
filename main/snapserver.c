/*
 * ESP32-S3 Mini Snapserver
 *
 * Snapcast binary protocol implementation for one mono Opus stream.
 * Socket writes and client teardown are synchronized to prevent message
 * interleaving and descriptor reuse races.
 *
 * The Hello message of every client is parsed and kept, so the JSON-RPC
 * control server on port 1705 can report the connected clients in
 * Server.GetStatus. Without this the controller UI shows an empty group.
 */
#include "snapserver.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "cJSON.h"
#include "device_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "audio_i2s.h"
#include "audio_opus.h"

static const char *TAG = "SNAPSERVER";

#define MAX_CLIENTS             SNAPSERVER_MAX_CLIENTS
#define RX_MAX                  4096
#define SNAP_PROTOCOL_VER       2
/*
 * Task stacks live in PSRAM, which is 8 MB here and barely used, while the
 * Wi-Fi driver competes for internal DRAM. Only the TCB stays internal, at
 * about a hundred bytes, so a client slot costs internal memory only for its
 * lwIP socket. The TCBs of tasks created this way must be released with
 * vTaskDeleteWithCaps(), and the API warns against self-deletion -- which is
 * why the connection tasks below are permanent and park between sessions
 * instead of being created per connection.
 */
#define TASK_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/*
 * Measured, not guessed: "stack headroom" reported conn=5256 B free of 8192
 * and sender=1796 B free of 3072, steady across a full run, so the peaks are
 * 2936 B and 1276 B -- the connection figure includes parsing Hello, since a
 * high-water mark covers everything since the task started. Internal DRAM is
 * what the Wi-Fi driver takes its TX buffers from, and at ten clients these
 * two stacks alone were 112 kB of a ~101 kB heap.
 */
#define CLIENT_TASK_STACK       5120
#define SERVER_TASK_STACK       8192
#define AUDIO_TASK_STACK        8192

/*
 * Chunk fan-out.
 *
 * The encode/capture loop used to send to every client itself, so its 20 ms
 * budget grew with the client count. Measured with four clients: average
 * frame interval 20449 us instead of 20000, single iterations up to 61 ms,
 * because a socket reported writable still blocks inside send() until the
 * whole chunk is out (~10 ms per client on a busy channel). The resulting
 * drift made the server re-anchor its capture timeline every few seconds,
 * which every client then saw as a timestamp jump.
 *
 * Now audio_task only drops each chunk into a pool slot and posts a
 * reference to each client's queue; one sender task per client does the
 * blocking send. Sends of different clients overlap instead of adding up,
 * which is the whole point -- in a shared task ten clients would serialise
 * into ~100 ms per round against a 20 ms chunk interval.
 *
 * Slots are recycled after CHUNK_POOL_SIZE chunks. A sender that has fallen
 * further behind than that finds its slot's sequence changed and drops the
 * chunk, which is the right outcome: the audio is long overdue anyway.
 */
#define CHUNK_POOL_SIZE         16
#define CLIENT_TX_QUEUE_DEPTH    8
#define SENDER_TASK_STACK     2048
/* Below audio_task (6) so encoding never waits behind a blocked send. */
#define SENDER_TASK_PRIORITY     5

/*
 * Statistics are printed from their own low-priority task, never from
 * audio_task. Four log lines per second at 115200 baud are ~42 ms of
 * blocking UART time; against a budget of 1000 ms per second for 50 frames
 * that is far more than the ~0.5 % by which the capture loop was running
 * late, and the DMA ring (40 ms) cannot absorb it indefinitely -- the
 * result was audible dropouts.
 */
#define STATS_TASK_STACK      3072
#define STATS_TASK_PRIORITY      2

/* Single socket write timeout. */
#define CLIENT_SEND_TIMEOUT_US   300000

/*
 * How long a half-written message may keep retrying before the connection is
 * declared dead. Only reached once send() has already accepted part of it.
 */
#define PARTIAL_SEND_LIMIT_US  10000000

/*
 * Wake-up interval for the blocking recv() in client_task(), not a
 * disconnect criterion: hitting it merely lets the loop re-check whether
 * the client is still active before waiting again. A client that never
 * sends anything is perfectly legal -- our own ported ESP32 client does
 * exactly that, since it ignores SNAP_MSG_TIME -- and used to be dropped
 * every ~31 s here, reconnecting endlessly. Detecting a peer that vanished
 * without a FIN/RST is the TCP keepalive's job below (~25 s), which
 * surfaces as a real recv() error rather than a timeout.
 */
#define CLIENT_RECV_TIMEOUT_US  30000000

/* TCP keepalive: idle 10s, then 3 probes 5s apart -> dead peer detected
 * after ~25s even while client_task is blocked waiting for the next
 * message. */
#define CLIENT_KEEPALIVE_IDLE_SEC   10
#define CLIENT_KEEPALIVE_INTVL_SEC  5
#define CLIENT_KEEPALIVE_COUNT      3

/* The Opus stream on the wire is mono, independent of the local I2S link. */
#define SNAPSTREAM_CHANNELS     1

/*
 * Snapcast writes this constant as a little-endian uint32_t into the
 * codec header.
 */
#define SNAP_OPUS_ID            0x4F505553U

enum {
    SNAP_TYPE_BASE = 0,
    SNAP_TYPE_CODEC_HEADER = 1,
    SNAP_TYPE_WIRE_CHUNK = 2,
    SNAP_TYPE_SERVER = 3,
    SNAP_TYPE_TIME = 4,
    SNAP_TYPE_HELLO = 5,
    SNAP_TYPE_CLIENT_INFO = 7,
    SNAP_TYPE_ERROR = 8
};

typedef struct __attribute__((packed)) {
    uint16_t type;
    uint16_t id;
    uint16_t refers_to;
    int32_t sent_sec;
    int32_t sent_usec;
    int32_t received_sec;
    int32_t received_usec;
    uint32_t size;
} snap_base_t;

typedef struct {
    uint32_t sequence; /* 0 marks a slot that was never filled */
    uint32_t size;
    int64_t timestamp_us;
    uint8_t data[AUDIO_MAX_OPUS_PACKET];
} chunk_slot_t;

/* What a client's queue carries: which slot, and which generation of it. */
typedef struct {
    uint16_t slot;
    uint32_t sequence;
} tx_item_t;

typedef struct {
    int fd;
    bool active;
    bool ready;
    uint16_t next_id;
    TaskHandle_t task;
    SemaphoreHandle_t send_mutex;
    char peer[16];

    /* Identity from the Hello message, guarded by s_clients_lock. */
    char id[64];
    char name[64];
    char hostname[64];
    char mac[24];
    char arch[24];
    char os[32];
    char version[24];
    int32_t instance;
    int32_t protocol_ver;

    /* Mutable state controlled via the JSON-RPC interface. */
    int32_t volume_percent;
    bool muted;
    int32_t latency_ms;

    int32_t last_seen_sec;
    int32_t last_seen_usec;

    /* Accounting, only touched under s_clients_lock. */
    uint32_t chunks_sent;
    uint32_t chunk_bytes;
    uint32_t chunk_errors;
    uint32_t chunks_skipped;
    uint32_t time_msgs;

    /* Fan-out, see the CHUNK_POOL_SIZE comment above. */
    QueueHandle_t tx_queue;
    TaskHandle_t tx_task;

    /*
     * Unused stack words, each task measuring its own. Read from outside via
     * the handle instead and a task that has just deleted itself takes the
     * caller down with it. This is here to answer one question with numbers:
     * whether 8 kB per connection and 3 kB per sender can come down far
     * enough for SNAPSERVER_MAX_CLIENTS speakers to fit in internal DRAM.
     */
    uint16_t conn_stack_free;
    uint16_t tx_stack_free;
    uint8_t *tx_payload; /* 12 B header + chunk, PSRAM */
} client_t;

static client_t s_clients[MAX_CLIENTS];

static struct {
    int64_t avg_us;
    int64_t min_us;
    int64_t max_us;
    uint32_t samples;
    bool fresh;
} s_frame_stats;

static chunk_slot_t *s_chunk_pool;
static uint32_t s_chunk_write;     /* next slot to fill, wraps at CHUNK_POOL_SIZE */
static uint32_t s_chunk_sequence;  /* never 0 once running, see chunk_slot_t */
static portMUX_TYPE s_clients_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_server_task;
static TaskHandle_t s_audio_task;
static bool s_started;

/*
 * The ESP has no RTC or SNTP, so its wall clock starts at an arbitrary
 * anchor. Snapcast clients report their real wall clock in every Time
 * message and compute "diff to server" in 32-bit milliseconds. An offset of
 * months overflows that arithmetic, the chunk age becomes nonsense and the
 * client discards all audio ("No chunks available"). Adopt the clock of the
 * first client that talks to us; afterwards the offset is near zero.
 */
static bool s_clock_synced;

/*
 * Lower bound for a timestamp to be considered a real wall clock
 * (2024-01-01). Uptime based clients report far smaller values and are
 * ignored for clock adoption.
 */
#define SNAP_CLOCK_PLAUSIBLE_MIN_SEC 1704067200L

/*
 * Snapcast clients interpret all timestamps as wall clock time. Using the
 * raw esp_timer uptime makes every wire chunk appear decades old to a client
 * whose clock runs on epoch time, so the client drops all of them and reports
 * "No chunks available". Without an RTC or SNTP we cannot know the real time,
 * but placing the clock in a plausible epoch range is sufficient: the client
 * only needs a consistent, monotonic base to compute its own offset from.
 */
#define SNAP_EPOCH_BASE_SEC 1767225600LL /* 2026-01-01T00:00:00Z */

/*
 * Offset from the monotonic esp_timer to wall clock, in microseconds.
 *
 * Do NOT use gettimeofday()/settimeofday() as the Snapcast timebase on this
 * target: the POSIX clock is driven by the RTC slow clock (150 kHz RC
 * oscillator) which can be off by several percent, while esp_timer runs off
 * the crystal. Mixing both makes the server clock drift against the client
 * by tens of milliseconds per second, so the client's time offset is stale
 * the moment it is computed and every chunk falls outside the buffer window.
 * Keep one monotonic source and shift it by a settable offset instead.
 */
static int64_t s_wall_offset_us = SNAP_EPOCH_BASE_SEC * 1000000LL;

static int64_t now_us(void)
{
    /*
     * s_wall_offset_us is a 64-bit value re-anchored from handle_time() on
     * another task; on this 32-bit target that write/read is not atomic, so
     * both sides must go through the same critical section to avoid a torn
     * value.
     */
    portENTER_CRITICAL(&s_clients_lock);
    const int64_t offset = s_wall_offset_us;
    portEXIT_CRITICAL(&s_clients_lock);

    return esp_timer_get_time() + offset;
}

static void now_ts(int32_t *sec, int32_t *usec)
{
    const int64_t t = now_us();
    *sec = (int32_t)(t / 1000000LL);
    *usec = (int32_t)(t % 1000000LL);
}

/*
 * 0 = sent, -1 = the connection is gone, 1 = the send buffer stayed full and
 * nothing at all was written (only ever returned when droppable is set).
 *
 * A full socket buffer means the peer is behind, not that it died: SO_SNDTIMEO
 * makes send() report that as EWOULDBLOCK. Treating it as fatal used to tear
 * the client down, and since a reconnect costs a fresh handshake plus a refill
 * of the whole bufferMs, a stall that the client could have ridden out turned
 * into a guaranteed audible gap.
 *
 * Once part of a message has gone out we are committed -- abandoning it would
 * leave the peer's framing desynchronised -- so from that point on we keep
 * retrying until PARTIAL_SEND_LIMIT_US, and only then give up for real.
 */
static int send_all(int fd, const void *data, size_t len, bool droppable)
{
    const uint8_t *p = (const uint8_t *)data;
    bool wrote_any = false;
    int64_t deadline_us = 0;

    while (len > 0U) {
        const int n = send(fd, p, len, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wrote_any && droppable) {
                return 1;
            }
            const int64_t now = esp_timer_get_time();
            if (deadline_us == 0) {
                deadline_us = now + PARTIAL_SEND_LIMIT_US;
                continue;
            }
            if (now < deadline_us) {
                continue;
            }
            ESP_LOGW(TAG,
                     "send stalled: fd=%d, giving up with %u B left",
                     fd,
                     (unsigned)len);
            return -1;
        }
        if (n <= 0) {
            ESP_LOGW(TAG,
                     "send failed: fd=%d rc=%d errno=%d (%s), remaining=%u",
                     fd,
                     n,
                     errno,
                     strerror(errno),
                     (unsigned)len);
            return -1;
        }
        p += n;
        len -= (size_t)n;
        wrote_any = true;
    }
    return 0;
}

/*
 * 0 = complete, -1 = error/EOF, RECV_IDLE = the receive timeout expired
 * before the first byte arrived. Only the latter is harmless: once part of
 * a message has been read there is no way to resync, so a timeout mid-way
 * is treated as an error like any other.
 */
#define RECV_IDLE 1

static int recv_all(int fd, void *data, size_t len)
{
    uint8_t *p = (uint8_t *)data;
    bool got_any = false;

    while (len > 0U) {
        const int n = recv(fd, p, len, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && !got_any && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return RECV_IDLE;
        }
        if (n <= 0) {
            return -1;
        }
        got_any = true;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_msg_unlocked(int fd,
                             uint16_t type,
                             uint16_t id,
                             uint16_t refers_to,
                             const void *payload,
                             uint32_t payload_size,
                             int32_t received_sec,
                             int32_t received_usec)
{
    snap_base_t base;
    memset(&base, 0, sizeof(base));
    now_ts(&base.sent_sec, &base.sent_usec);
    base.type = type;
    base.id = id;
    base.refers_to = refers_to;
    base.received_sec = received_sec;
    base.received_usec = received_usec;
    base.size = payload_size;

    /*
     * A dropped wire chunk is a click at worst; a dropped ServerSettings,
     * CodecHeader or Time reply breaks the session outright, so those wait.
     */
    const bool droppable = (type == SNAP_TYPE_WIRE_CHUNK);

    const int header_rc = send_all(fd, &base, sizeof(base), droppable);
    if (header_rc != 0) {
        return header_rc;
    }
    if (payload_size > 0U && send_all(fd, payload, payload_size, false) < 0) {
        return -1;
    }
    return 0;
}

static int client_send_msg(client_t *client,
                           uint16_t type,
                           uint16_t id,
                           uint16_t refers_to,
                           const void *payload,
                           uint32_t payload_size,
                           int32_t received_sec,
                           int32_t received_usec,
                           bool require_ready)
{
    if (client == NULL || client->send_mutex == NULL) {
        return -1;
    }

    if (xSemaphoreTake(client->send_mutex, portMAX_DELAY) != pdTRUE) {
        return -1;
    }

    int fd = -1;
    bool allowed = false;
    portENTER_CRITICAL(&s_clients_lock);
    allowed = client->active && client->fd >= 0 &&
              (!require_ready || client->ready);
    if (allowed) {
        fd = client->fd;
    }
    portEXIT_CRITICAL(&s_clients_lock);

    int result = -1;
    if (allowed) {
        result = send_msg_unlocked(fd,
                                   type,
                                   id,
                                   refers_to,
                                   payload,
                                   payload_size,
                                   received_sec,
                                   received_usec);
    }

    xSemaphoreGive(client->send_mutex);
    return result;
}

static int send_server_settings(client_t *client, uint16_t refers_to)
{
    char json[128];
    int32_t volume = 100;
    bool muted = false;
    int32_t latency = 0;

    portENTER_CRITICAL(&s_clients_lock);
    volume = client->volume_percent;
    muted = client->muted;
    latency = client->latency_ms;
    portEXIT_CRITICAL(&s_clients_lock);

    device_config_t cfg;
    device_config_get(&cfg);
    const uint16_t buffer_ms = cfg.buffer_ms;

    const int json_len = snprintf(
        json,
        sizeof(json),
        "{\"bufferMs\":%u,\"latency\":%ld,\"muted\":%s,\"volume\":%ld}",
        (unsigned)buffer_ms,
        (long)latency,
        muted ? "true" : "false",
        (long)volume);

    if (json_len <= 0 || (size_t)json_len >= sizeof(json)) {
        return -1;
    }

    const uint32_t len = (uint32_t)json_len;
    uint8_t payload[sizeof(uint32_t) + sizeof(json)];

    memcpy(payload, &len, sizeof(len));
    memcpy(payload + sizeof(len), json, len);

    ESP_LOGI(TAG,
             "Sending ServerSettings: bufferMs=%u latency=%ld muted=%s volume=%ld",
             (unsigned)buffer_ms,
             (long)latency,
             muted ? "true" : "false",
             (long)volume);

    return client_send_msg(client,
                           SNAP_TYPE_SERVER,
                           1,
                           refers_to,
                           payload,
                           (uint32_t)(sizeof(len) + len),
                           0,
                           0,
                           false);
}

static int send_codec_header(client_t *client, uint16_t refers_to)
{
    static const char codec[] = "opus";
    const uint32_t codec_size = 4;
    const uint32_t header_size = 12;
    uint8_t payload[4 + 4 + 4 + 12];
    size_t off = 0;

    memcpy(payload + off, &codec_size, 4); off += 4;
    memcpy(payload + off, codec, codec_size); off += codec_size;
    memcpy(payload + off, &header_size, 4); off += 4;

    const uint32_t opus_id = SNAP_OPUS_ID;
    const uint32_t rate = AUDIO_SAMPLE_RATE;
    const uint16_t bits = AUDIO_BITS;
    const uint16_t channels = SNAPSTREAM_CHANNELS;

    memcpy(payload + off, &opus_id, 4); off += 4;
    memcpy(payload + off, &rate, 4); off += 4;
    memcpy(payload + off, &bits, 2); off += 2;
    memcpy(payload + off, &channels, 2); off += 2;

    ESP_LOGI(TAG,
             "Sending CodecHeader: codec=opus rate=%lu bits=%u channels=%u "
             "header_size=%lu opus_id=0x%08lX",
             (unsigned long)rate,
             (unsigned)bits,
             (unsigned)channels,
             (unsigned long)header_size,
             (unsigned long)opus_id);

    return client_send_msg(client,
                           SNAP_TYPE_CODEC_HEADER,
                           2,
                           refers_to,
                           payload,
                           (uint32_t)off,
                           0,
                           0,
                           false);
}

/*
 * Builds and sends one wire chunk from a pool slot. Returns 1 when the slot
 * had already been recycled under us, which is not an error: the sender was
 * simply too far behind and the chunk is stale.
 */
static int send_wire_chunk(client_t *client, const tx_item_t *item)
{
    const chunk_slot_t *slot = &s_chunk_pool[item->slot];
    uint8_t *payload = client->tx_payload;

    /*
     * The timestamp originates from esp_timer (uptime) and must be expressed
     * in the same wall clock domain that now_ts() reports.
     *
     * s_wall_offset_us is re-anchored when a plausible wall clock is learned.
     * Adding the same offset to the monotonic capture timestamp keeps message
     * headers and audio chunks in one consistent time domain.
     */
    portENTER_CRITICAL(&s_clients_lock);
    const int64_t wall_offset_us = s_wall_offset_us;
    portEXIT_CRITICAL(&s_clients_lock);
    const int64_t chunk_us = slot->timestamp_us + wall_offset_us;

    const int32_t sec = (int32_t)(chunk_us / 1000000LL);
    const int32_t usec = (int32_t)(chunk_us % 1000000LL);
    const uint32_t size = slot->size;

    if (size > AUDIO_MAX_OPUS_PACKET) {
        return 1;
    }

    memcpy(payload + 0, &sec, 4);
    memcpy(payload + 4, &usec, 4);
    memcpy(payload + 8, &size, 4);
    memcpy(payload + 12, slot->data, size);

    /*
     * Re-read the sequence only after copying: audio_task may have started
     * refilling this slot while we were reading it, in which case the copy is
     * a mix of two chunks and must not go out.
     */
    if (slot->sequence != item->sequence) {
        return 1;
    }

    const int rc = client_send_msg(client,
                                   SNAP_TYPE_WIRE_CHUNK,
                                   0,
                                   0,
                                   payload,
                                   12U + size,
                                   0,
                                   0,
                                   true);

    portENTER_CRITICAL(&s_clients_lock);
    if (rc == 0) {
        client->chunks_sent++;
        client->chunk_bytes += (12U + size);
    } else if (rc < 0) {
        client->chunk_errors++;
    }
    portEXIT_CRITICAL(&s_clients_lock);

    return rc;
}

static int handle_time(client_t *client,
                       const snap_base_t *base,
                       const uint8_t *payload,
                       uint32_t size)
{
    if (client == NULL || base == NULL || payload == NULL || size != 8U) {
        ESP_LOGW(TAG,
                 "Time message rejected: size=%lu (expected 8)",
                 (unsigned long)size);
        return -1;
    }

    int32_t server_recv_sec = 0;
    int32_t server_recv_usec = 0;
    now_ts(&server_recv_sec, &server_recv_usec);

    int64_t client_send_us =
        (int64_t)base->sent_sec * 1000000LL + base->sent_usec;
    int64_t server_recv_us =
        (int64_t)server_recv_sec * 1000000LL + server_recv_usec;

    /*
     * Clock adoption with plausibility filter.
     *
     * Not every client owns a real clock: our own ESP snapclient has neither
     * RTC nor SNTP and reports plain uptime (a few 100000 s at most). Adopting
     * that would move the server clock back to 1970 and make things worse.
     * Only accept a timestamp that can actually be a wall clock, i.e. one that
     * lies after SNAP_CLOCK_PLAUSIBLE_MIN_SEC. PC and Android clients qualify,
     * uptime-based ones do not.
     */
    portENTER_CRITICAL(&s_clients_lock);
    const bool already_synced = s_clock_synced;
    if (!already_synced && base->sent_sec > SNAP_CLOCK_PLAUSIBLE_MIN_SEC) {
        s_clock_synced = true;
    }
    portEXIT_CRITICAL(&s_clients_lock);

    if (!already_synced && base->sent_sec > SNAP_CLOCK_PLAUSIBLE_MIN_SEC) {

        const int64_t skew_us = client_send_us - server_recv_us;

        if (skew_us > 60000000LL || skew_us < -60000000LL) {
            /*
             * Re-anchor the monotonic timebase; no POSIX clock involved.
             * s_clock_synced was already claimed above (still inside this
             * function's single-threaded-per-client path but guarded
             * against a second client racing the same adoption window).
             */
            portENTER_CRITICAL(&s_clients_lock);
            s_wall_offset_us = client_send_us - esp_timer_get_time();
            portEXIT_CRITICAL(&s_clients_lock);

            ESP_LOGW(TAG,
                     "Adopted wall clock from %s: %ld s (skew was %lld s)",
                     client->peer,
                     (long)base->sent_sec,
                     (long long)(skew_us / 1000000LL));

            now_ts(&server_recv_sec, &server_recv_usec);
            server_recv_us =
                (int64_t)server_recv_sec * 1000000LL + server_recv_usec;
        }
    }

    const int64_t delta = server_recv_us - client_send_us;

    /*
     * Normalise into a valid timeval: the microsecond field must always be
     * in [0, 999999], with the borrow carried into the seconds field.
     * Plain C division/modulo truncate towards zero, which produced a
     * negative usec for negative deltas. Snapcast clients then compute a
     * bogus server time offset and discard every wire chunk as being
     * outside the buffer window ("No chunks available").
     */
    int32_t delta_sec = (int32_t)(delta / 1000000LL);
    int32_t delta_usec = (int32_t)(delta % 1000000LL);

    if (delta_usec < 0) {
        delta_usec += 1000000;
        delta_sec -= 1;
    }

    uint8_t response[8];

    memcpy(response + 0, &delta_sec, 4);
    memcpy(response + 4, &delta_usec, 4);

    uint32_t seen = 0;
    portENTER_CRITICAL(&s_clients_lock);
    client->time_msgs++;
    seen = client->time_msgs;
    portEXIT_CRITICAL(&s_clients_lock);

    if (seen <= 3U) {
        ESP_LOGI(TAG,
                 "Time #%lu from %s: client_sent=%ld.%06ld "
                 "server_recv=%ld.%06ld delta=%ld.%06ld",
                 (unsigned long)seen,
                 client->peer,
                 (long)base->sent_sec,
                 (long)base->sent_usec,
                 (long)server_recv_sec,
                 (long)server_recv_usec,
                 (long)delta_sec,
                 (long)delta_usec);
    }

    return client_send_msg(client,
                           SNAP_TYPE_TIME,
                           0,
                           base->id,
                           response,
                           sizeof(response),
                           server_recv_sec,
                           server_recv_usec,
                           false);
}

/* ------------------------------------------------------------------ */
/* Hello parsing                                                      */
/* ------------------------------------------------------------------ */

static void copy_json_string(char *dst,
                             size_t dst_size,
                             const cJSON *root,
                             const char *key,
                             const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    if (cJSON_IsString(item) && item->valuestring != NULL &&
        item->valuestring[0] != '\0') {
        strlcpy(dst, item->valuestring, dst_size);
    } else {
        strlcpy(dst, fallback != NULL ? fallback : "", dst_size);
    }
}

static int32_t json_int_or(const cJSON *root,
                           const char *key,
                           int32_t fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? (int32_t)item->valuedouble : fallback;
}

/*
 * Parses the Hello payload and stores the client identity. Returns false if
 * the payload is malformed or the client speaks a newer stream protocol.
 */
static bool handle_hello(client_t *client,
                         const uint8_t *payload,
                         uint32_t size)
{
    if (payload == NULL || size < sizeof(uint32_t)) {
        return false;
    }

    uint32_t json_size = 0;
    memcpy(&json_size, payload, sizeof(json_size));
    if (json_size == 0U ||
        json_size > size - sizeof(uint32_t) ||
        json_size >= RX_MAX) {
        return false;
    }

    char *json = malloc((size_t)json_size + 1U);
    if (json == NULL) {
        return false;
    }
    memcpy(json, payload + sizeof(uint32_t), json_size);
    json[json_size] = '\0';

    cJSON *root = cJSON_Parse(json);
    free(json);

    if (root == NULL) {
        ESP_LOGW(TAG, "Hello payload is not valid JSON");
        return false;
    }

    const int32_t proto =
        json_int_or(root, "SnapStreamProtocolVersion", SNAP_PROTOCOL_VER);

    if (proto > SNAP_PROTOCOL_VER) {
        ESP_LOGW(TAG,
                 "Rejecting client with stream protocol %ld (supported: %d)",
                 (long)proto,
                 SNAP_PROTOCOL_VER);
        cJSON_Delete(root);
        return false;
    }

    /*
     * Fill a scratch copy first, then publish it under the lock. cJSON
     * allocations must not happen inside a critical section.
     */
    char id[64];
    char name[64];
    char hostname[64];
    char mac[24];
    char arch[24];
    char os[32];
    char version[24];

    copy_json_string(mac, sizeof(mac), root, "MAC", client->peer);
    copy_json_string(id, sizeof(id), root, "ID", mac);
    copy_json_string(hostname, sizeof(hostname), root, "HostName", id);
    copy_json_string(name, sizeof(name), root, "ClientName", hostname);
    copy_json_string(arch, sizeof(arch), root, "Arch", "unknown");
    copy_json_string(os, sizeof(os), root, "OS", "unknown");
    copy_json_string(version, sizeof(version), root, "Version", "0.0.0");

    const int32_t instance = json_int_or(root, "Instance", 1);

    cJSON_Delete(root);

    int32_t seen_sec = 0;
    int32_t seen_usec = 0;
    now_ts(&seen_sec, &seen_usec);

    portENTER_CRITICAL(&s_clients_lock);
    strlcpy(client->id, id, sizeof(client->id));
    strlcpy(client->name, name, sizeof(client->name));
    strlcpy(client->hostname, hostname, sizeof(client->hostname));
    strlcpy(client->mac, mac, sizeof(client->mac));
    strlcpy(client->arch, arch, sizeof(client->arch));
    strlcpy(client->os, os, sizeof(client->os));
    strlcpy(client->version, version, sizeof(client->version));
    client->instance = instance;
    client->protocol_ver = proto;
    client->last_seen_sec = seen_sec;
    client->last_seen_usec = seen_usec;
    portEXIT_CRITICAL(&s_clients_lock);

    ESP_LOGI(TAG,
             "Hello from %s: id=%s name=%s host=%s os=%s arch=%s version=%s "
             "instance=%ld protocol=%ld",
             client->peer,
             id,
             name,
             hostname,
             os,
             arch,
             version,
             (long)instance,
             (long)proto);

    return true;
}

static int recv_message(int fd, snap_base_t *base, uint8_t **payload)
{
    const int header_result = recv_all(fd, base, sizeof(*base));
    if (header_result == RECV_IDLE) {
        return RECV_IDLE;
    }
    if (header_result < 0) {
        return -1;
    }
    if (base->size > RX_MAX) {
        return -2;
    }

    *payload = NULL;
    if (base->size > 0U) {
        *payload = malloc(base->size);
        if (*payload == NULL) {
            return -3;
        }
        if (recv_all(fd, *payload, base->size) < 0) {
            free(*payload);
            *payload = NULL;
            return -1;
        }
    }
    return 0;
}

static void close_client(client_t *client)
{
    if (client == NULL || client->send_mutex == NULL) {
        return;
    }

    xSemaphoreTake(client->send_mutex, portMAX_DELAY);

    int fd = -1;
    uint32_t chunks = 0;
    uint32_t bytes = 0;
    uint32_t errors = 0;
    uint32_t skipped = 0;
    uint32_t times = 0;
    char peer[sizeof(client->peer)];

    portENTER_CRITICAL(&s_clients_lock);
    fd = client->fd;
    chunks = client->chunks_sent;
    bytes = client->chunk_bytes;
    errors = client->chunk_errors;
    skipped = client->chunks_skipped;
    times = client->time_msgs;
    strlcpy(peer, client->peer, sizeof(peer));
    client->ready = false;
    client->active = false;
    client->fd = -1;
    /* `task` is the slot's permanent connection task and outlives the
     * session -- clearing it here would strand the slot for good. */
    portEXIT_CRITICAL(&s_clients_lock);

    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }

    xSemaphoreGive(client->send_mutex);

    ESP_LOGI(TAG,
             "Session summary %s: chunks_sent=%lu bytes=%lu send_errors=%lu "
             "skipped=%lu time_msgs=%lu",
             peer,
             (unsigned long)chunks,
             (unsigned long)bytes,
             (unsigned long)errors,
             (unsigned long)skipped,
             (unsigned long)times);
}

static void client_session(client_t *client)
{
    int fd = -1;

    portENTER_CRITICAL(&s_clients_lock);
    fd = client->fd;
    portEXIT_CRITICAL(&s_clients_lock);

    snap_base_t base;
    uint8_t *payload = NULL;

    /* Anything other than a complete message is fatal here, including the
     * idle timeout: a peer that connects without saying Hello is of no use. */
    if (recv_message(fd, &base, &payload) != 0 ||
        base.type != SNAP_TYPE_HELLO ||
        !handle_hello(client, payload, base.size)) {
        free(payload);
        payload = NULL;
        ESP_LOGW(TAG, "Invalid Hello");
        goto done;
    }

    /*
     * Snapcast clients send Hello as a request and wait for a reply whose
     * refersTo field carries the Hello message id. Answering with 0 leaves
     * the pending request unresolved, the client times out after 2 s,
     * reconnects and never starts its time sync.
     */
    const uint16_t hello_id = base.id;

    free(payload);
    payload = NULL;

    if (send_server_settings(client, hello_id) < 0 ||
        send_codec_header(client, hello_id) < 0) {
        goto done;
    }

    portENTER_CRITICAL(&s_clients_lock);
    client->ready = true;
    portEXIT_CRITICAL(&s_clients_lock);
    ESP_LOGI(TAG, "Client handshake complete (%s, fd=%d)", client->peer, fd);

    for (;;) {
        portENTER_CRITICAL(&s_clients_lock);
        const bool active = client->active;
        fd = client->fd;
        portEXIT_CRITICAL(&s_clients_lock);

        if (!active || fd < 0) {
            break;
        }

        const int rc = recv_message(fd, &base, &payload);
        if (rc == RECV_IDLE) {
            /* Nothing sent for a while, which is legal -- loop back and
             * wait again, re-checking client->active on the way. */
            continue;
        }
        if (rc < 0) {
            ESP_LOGW(TAG,
                     "Client receive ended: %s rc=%d errno=%d (%s)",
                     client->peer,
                     rc,
                     errno,
                     strerror(errno));
            break;
        }

        client->conn_stack_free = (uint16_t)uxTaskGetStackHighWaterMark(NULL);

        int32_t seen_sec = 0;
        int32_t seen_usec = 0;
        now_ts(&seen_sec, &seen_usec);

        portENTER_CRITICAL(&s_clients_lock);
        client->last_seen_sec = seen_sec;
        client->last_seen_usec = seen_usec;
        portEXIT_CRITICAL(&s_clients_lock);

        switch (base.type) {
        case SNAP_TYPE_TIME:
            if (handle_time(client, &base, payload, base.size) < 0) {
                ESP_LOGW(TAG, "Time reply failed (%s)", client->peer);
                free(payload);
                payload = NULL;
                goto done;
            }
            break;
        case SNAP_TYPE_CLIENT_INFO:
            break;
        default:
            ESP_LOGD(TAG, "Ignoring client message type=%u", base.type);
            break;
        }

        free(payload);
        payload = NULL;
    }

done:
    free(payload);
    close_client(client);
    ESP_LOGI(TAG, "Client disconnected");
}

/*
 * One per client slot, created once at startup and parked on a notification
 * for the lifetime of the server -- the same reasoning as sender_task, plus
 * the stack is in PSRAM and freeing it from the task running on it is
 * exactly what vTaskDeleteWithCaps() warns about.
 */
static void client_task(void *arg)
{
    client_t *client = (client_t *)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        client_session(client);
    }
}

static void mark_client_failed(client_t *client)
{
    if (client == NULL) {
        return;
    }

    int fd = -1;
    portENTER_CRITICAL(&s_clients_lock);
    if (client->active) {
        client->ready = false;
        fd = client->fd;
    }
    portEXIT_CRITICAL(&s_clients_lock);

    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }
}

/*
 * One per client slot, created once at startup and parked on its queue for
 * the lifetime of the server. Created statically rather than per connection
 * on purpose: a queue that audio_task may post to while the owning task is
 * being torn down is exactly the kind of race this file has already been
 * bitten by, and MAX_CLIENTS idle tasks cost only their stacks.
 */
static void sender_task(void *arg)
{
    client_t *client = (client_t *)arg;
    uint32_t stack_check = 0;

    for (;;) {
        tx_item_t item;
        if (xQueueReceive(client->tx_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        bool send_it = false;
        portENTER_CRITICAL(&s_clients_lock);
        send_it = client->active && client->ready && client->fd >= 0;
        portEXIT_CRITICAL(&s_clients_lock);

        if (!send_it) {
            continue;
        }

        if ((++stack_check & 0xFFU) == 0U) {
            client->tx_stack_free =
                (uint16_t)uxTaskGetStackHighWaterMark(NULL);
        }

        const int rc = send_wire_chunk(client, &item);
        if (rc > 0) {
            portENTER_CRITICAL(&s_clients_lock);
            client->chunks_skipped++;
            portEXIT_CRITICAL(&s_clients_lock);
        } else if (rc < 0) {
            ESP_LOGW(TAG,
                     "Wire chunk send failed (%s); closing client",
                     client->peer);
            mark_client_failed(client);
        }
    }
}

/*
 * Prints everything the audio path measures, at low priority and off the
 * real-time path. See STATS_TASK_STACK for why this matters.
 */
static void stats_task(void *arg)
{
    (void)arg;
    uint32_t last_chunks[MAX_CLIENTS] = {0};
    int64_t last_peak_us = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        int64_t avg_us = 0;
        int64_t min_us = 0;
        int64_t max_us = 0;
        uint32_t samples = 0;
        bool fresh = false;

        portENTER_CRITICAL(&s_clients_lock);
        avg_us = s_frame_stats.avg_us;
        min_us = s_frame_stats.min_us;
        max_us = s_frame_stats.max_us;
        samples = s_frame_stats.samples;
        fresh = s_frame_stats.fresh;
        s_frame_stats.fresh = false;
        portEXIT_CRITICAL(&s_clients_lock);

        if (fresh) {
            ESP_LOGI(TAG,
                     "frame delta: avg=%lld us min=%lld us max=%lld us samples=%lu",
                     (long long)avg_us,
                     (long long)min_us,
                     (long long)max_us,
                     (unsigned long)samples);
        }

        for (int i = 0; i < MAX_CLIENTS; ++i) {
            bool active = false;
            bool ready = false;
            uint32_t chunks = 0;
            uint32_t bytes = 0;
            uint32_t errs = 0;
            uint32_t skipped = 0;
            uint32_t times = 0;
            char peer[16];

            portENTER_CRITICAL(&s_clients_lock);
            active = s_clients[i].active;
            ready = s_clients[i].ready;
            chunks = s_clients[i].chunks_sent;
            bytes = s_clients[i].chunk_bytes;
            errs = s_clients[i].chunk_errors;
            skipped = s_clients[i].chunks_skipped;
            times = s_clients[i].time_msgs;
            strlcpy(peer, s_clients[i].peer, sizeof(peer));
            portEXIT_CRITICAL(&s_clients_lock);

            /* A reconnect resets the counter; avoid an unsigned wrap. */
            const uint32_t chunk_rate =
                (chunks >= last_chunks[i]) ? (chunks - last_chunks[i]) : chunks;

            if (active) {
                ESP_LOGI(TAG,
                         "client[%d] %s ready=%d chunks/s=%lu total=%lu "
                         "bytes=%lu send_errors=%lu skipped=%lu time_msgs=%lu",
                         i, peer, (int)ready,
                         (unsigned long)chunk_rate,
                         (unsigned long)chunks,
                         (unsigned long)bytes,
                         (unsigned long)errs,
                         (unsigned long)skipped,
                         (unsigned long)times);
            }
            last_chunks[i] = chunks;
        }

        /*
         * Internal DRAM, every 5 s. This is the resource the Wi-Fi driver
         * takes its TX buffers from, and running it down showed up on
         * device as every client -- including a phone running stock
         * Snapcast -- going to 0 chunks/s at the same instant while the
         * capture loop kept perfect time, followed by "Could not create
         * client task". largest is what an allocation actually has to fit
         * into; min_ever is the low-water mark since boot.
         */
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_peak_us >= 5000000LL) {
            ESP_LOGI(TAG,
                     "heap: internal free=%u B largest=%u B min_ever=%u B",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));

            uint16_t conn_min = UINT16_MAX;
            uint16_t send_min = UINT16_MAX;
            portENTER_CRITICAL(&s_clients_lock);
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (s_clients[i].conn_stack_free != 0U &&
                    s_clients[i].conn_stack_free < conn_min) {
                    conn_min = s_clients[i].conn_stack_free;
                }
                if (s_clients[i].tx_stack_free != 0U &&
                    s_clients[i].tx_stack_free < send_min) {
                    send_min = s_clients[i].tx_stack_free;
                }
            }
            portEXIT_CRITICAL(&s_clients_lock);

            if (conn_min != UINT16_MAX || send_min != UINT16_MAX) {
                ESP_LOGI(TAG,
                         "stack headroom: conn=%d B sender=%d B (of %d / %d)",
                         (conn_min == UINT16_MAX) ? -1 : (int)(conn_min * sizeof(StackType_t)),
                         (send_min == UINT16_MAX) ? -1 : (int)(send_min * sizeof(StackType_t)),
                         CLIENT_TASK_STACK, SENDER_TASK_STACK);
            }

            int16_t peak_left = 0;
            int16_t peak_right = 0;
            audio_i2s_take_output_peak(&peak_left, &peak_right);
            ESP_LOGI(TAG, "DSP output peak: left=%d right=%d (of 32767)",
                     (int)peak_left, (int)peak_right);
            last_peak_us = now_us;
        }
    }
}

static void audio_task(void *arg)
{
    (void)arg;

    int64_t last_frame_start_us = 0;
    int64_t stats_start_us = esp_timer_get_time();
    int64_t delta_sum_us = 0;
    int64_t delta_min_us = INT64_MAX;
    int64_t delta_max_us = 0;
    uint32_t delta_count = 0;

    for (;;) {
        const int64_t frame_start_us = esp_timer_get_time();
        if (last_frame_start_us != 0) {
            const int64_t delta_us = frame_start_us - last_frame_start_us;
            delta_sum_us += delta_us;
            if (delta_us < delta_min_us) delta_min_us = delta_us;
            if (delta_us > delta_max_us) delta_max_us = delta_us;
            ++delta_count;
        }
        last_frame_start_us = frame_start_us;

        audio_opus_packet_t packet;
        const esp_err_t audio_result = audio_opus_get_packet(&packet);
        if (audio_result == ESP_OK) {
            client_t *clients[MAX_CLIENTS];
            int count = 0;

            portENTER_CRITICAL(&s_clients_lock);
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (s_clients[i].active && s_clients[i].ready) {
                    clients[count++] = &s_clients[i];
                }
            }
            portEXIT_CRITICAL(&s_clients_lock);

            /*
             * Fill one pool slot and hand out references. Everything here is
             * bounded work -- a memcpy plus one queue post per client -- so
             * the capture cadence no longer depends on how many clients are
             * connected, nor on how fast any of them drains its socket.
             */
            if (packet.size <= AUDIO_MAX_OPUS_PACKET) {
                const uint16_t slot_index = (uint16_t)s_chunk_write;
                chunk_slot_t *slot = &s_chunk_pool[slot_index];

                slot->size = (uint32_t)packet.size;
                slot->timestamp_us = packet.timestamp_us;
                memcpy(slot->data, packet.data, packet.size);
                /* Published last: a sender comparing sequences must never
                 * see the new number in front of the new payload. */
                slot->sequence = ++s_chunk_sequence;
                s_chunk_write = (s_chunk_write + 1U) % CHUNK_POOL_SIZE;

                const tx_item_t item = {
                    .slot = slot_index,
                    .sequence = slot->sequence,
                };

                for (int i = 0; i < count; ++i) {
                    if (clients[i]->tx_queue == NULL) {
                        continue;
                    }
                    if (xQueueSend(clients[i]->tx_queue, &item, 0) != pdTRUE) {
                        /* Queue full: this client is behind. Drop its oldest
                         * pending chunk rather than the newest -- stale audio
                         * is the least useful thing to keep. */
                        tx_item_t dropped;
                        if (xQueueReceive(clients[i]->tx_queue, &dropped, 0) == pdTRUE) {
                            portENTER_CRITICAL(&s_clients_lock);
                            clients[i]->chunks_skipped++;
                            portEXIT_CRITICAL(&s_clients_lock);
                        }
                        (void)xQueueSend(clients[i]->tx_queue, &item, 0);
                    }
                }
            }
        } else {
            ESP_LOGE(TAG, "Audio frame failed: %s", esp_err_to_name(audio_result));
        }

        const int64_t stats_now_us = esp_timer_get_time();
        if (delta_count > 0U && stats_now_us - stats_start_us >= 1000000LL) {
            /* Publish only -- printing happens in stats_task. */
            portENTER_CRITICAL(&s_clients_lock);
            s_frame_stats.avg_us = delta_sum_us / (int64_t)delta_count;
            s_frame_stats.min_us = delta_min_us;
            s_frame_stats.max_us = delta_max_us;
            s_frame_stats.samples = delta_count;
            s_frame_stats.fresh = true;
            portEXIT_CRITICAL(&s_clients_lock);

            stats_start_us = stats_now_us;
            delta_sum_us = 0;
            delta_min_us = INT64_MAX;
            delta_max_us = 0;
            delta_count = 0;
        }
    }
}

static void server_task(void *arg)
{
    (void)arg;

    const int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket failed errno=%d", errno);
        s_server_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    if (setsockopt(listen_fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &reuse,
                   sizeof(reuse)) < 0) {
        ESP_LOGW(TAG, "setsockopt failed errno=%d", errno);
    }

    const struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(SNAPSERVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(listen_fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed errno=%d", errno);
        close(listen_fd);
        s_server_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_fd, MAX_CLIENTS) < 0) {
        ESP_LOGE(TAG, "listen failed errno=%d", errno);
        close(listen_fd);
        s_server_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Listening on TCP port %d", SNAPSERVER_PORT);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        const int fd = accept(listen_fd,
                              (struct sockaddr *)&peer,
                              &peer_len);
        if (fd < 0) {
            if (errno != EINTR) {
                ESP_LOGW(TAG, "accept failed errno=%d", errno);
            }
            continue;
        }

        char peer_ip[16] = {0};
        inet_ntoa_r(peer.sin_addr, peer_ip, sizeof(peer_ip) - 1);

        const struct timeval send_timeout = {
            .tv_sec = (time_t)(CLIENT_SEND_TIMEOUT_US / 1000000),
            .tv_usec = (suseconds_t)(CLIENT_SEND_TIMEOUT_US % 1000000),
        };
        if (setsockopt(fd,
                       SOL_SOCKET,
                       SO_SNDTIMEO,
                       &send_timeout,
                       sizeof(send_timeout)) < 0) {
            ESP_LOGW(TAG,
                     "Could not set client send timeout, errno=%d",
                     errno);
        }

        const struct timeval recv_timeout = {
            .tv_sec = (time_t)(CLIENT_RECV_TIMEOUT_US / 1000000),
            .tv_usec = (suseconds_t)(CLIENT_RECV_TIMEOUT_US % 1000000),
        };
        if (setsockopt(fd,
                       SOL_SOCKET,
                       SO_RCVTIMEO,
                       &recv_timeout,
                       sizeof(recv_timeout)) < 0) {
            ESP_LOGW(TAG,
                     "Could not set client receive timeout, errno=%d",
                     errno);
        }

        int keepalive = 1;
        if (setsockopt(fd,
                       SOL_SOCKET,
                       SO_KEEPALIVE,
                       &keepalive,
                       sizeof(keepalive)) < 0) {
            ESP_LOGW(TAG, "Could not enable TCP keepalive, errno=%d", errno);
        }
        int keepalive_idle = CLIENT_KEEPALIVE_IDLE_SEC;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,
                  &keepalive_idle, sizeof(keepalive_idle));
        int keepalive_intvl = CLIENT_KEEPALIVE_INTVL_SEC;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL,
                  &keepalive_intvl, sizeof(keepalive_intvl));
        int keepalive_cnt = CLIENT_KEEPALIVE_COUNT;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,
                  &keepalive_cnt, sizeof(keepalive_cnt));

        int nodelay = 1;
        if (setsockopt(fd,
                       IPPROTO_TCP,
                       TCP_NODELAY,
                       &nodelay,
                       sizeof(nodelay)) < 0) {
            ESP_LOGW(TAG, "Could not set TCP_NODELAY, errno=%d", errno);
        }

        int slot = -1;
        portENTER_CRITICAL(&s_clients_lock);
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (!s_clients[i].active) {
                s_clients[i].fd = fd;
                s_clients[i].active = true;
                s_clients[i].ready = false;
                s_clients[i].next_id = 1;
                s_clients[i].chunks_sent = 0;
                s_clients[i].chunk_bytes = 0;
                s_clients[i].chunk_errors = 0;
                s_clients[i].chunks_skipped = 0;
                s_clients[i].time_msgs = 0;
                s_clients[i].instance = 1;
                s_clients[i].protocol_ver = SNAP_PROTOCOL_VER;
                s_clients[i].volume_percent = 100;
                s_clients[i].muted = false;
                s_clients[i].latency_ms = 0;
                s_clients[i].id[0] = '\0';
                s_clients[i].name[0] = '\0';
                s_clients[i].hostname[0] = '\0';
                s_clients[i].mac[0] = '\0';
                s_clients[i].arch[0] = '\0';
                s_clients[i].os[0] = '\0';
                s_clients[i].version[0] = '\0';
                strlcpy(s_clients[i].peer,
                       peer_ip,
                       sizeof(s_clients[i].peer));
                slot = i;
                break;
            }
        }
        portEXIT_CRITICAL(&s_clients_lock);

        if (slot >= 0) {
            /*
             * Drop references left over from whoever held this slot before,
             * so they cannot be sent to the new occupant. Deliberately
             * outside the critical section: FreeRTOS queue calls take their
             * own locks and must never run with interrupts disabled. Safe
             * here because the sender only transmits once `ready` is set,
             * which happens after the handshake well below.
             */
            xQueueReset(s_clients[slot].tx_queue);
        }

        if (slot < 0) {
            ESP_LOGW(TAG, "Rejecting client: all slots occupied");
            shutdown(fd, SHUT_RDWR);
            close(fd);
            continue;
        }

        ESP_LOGI(TAG,
                 "Client connected slot=%d ip=%s fd=%d",
                 slot,
                 s_clients[slot].peer,
                 fd);

        /* The slot's task already exists and is parked; wake it. */
        xTaskNotifyGive(s_clients[slot].task);
    }
}

/* ------------------------------------------------------------------ */
/* Public API for the control server                                  */
/* ------------------------------------------------------------------ */

size_t snapserver_get_clients(snapserver_client_info_t *out,
                              size_t max_clients)
{
    if (out == NULL || max_clients == 0U) {
        return 0;
    }

    size_t count = 0;

    portENTER_CRITICAL(&s_clients_lock);
    for (int i = 0; i < MAX_CLIENTS && count < max_clients; ++i) {
        if (!s_clients[i].active || !s_clients[i].ready) {
            continue;
        }

        snapserver_client_info_t *dst = &out[count];
        memset(dst, 0, sizeof(*dst));

        strlcpy(dst->id, s_clients[i].id, sizeof(dst->id));
        strlcpy(dst->name, s_clients[i].name, sizeof(dst->name));
        strlcpy(dst->hostname, s_clients[i].hostname, sizeof(dst->hostname));
        strlcpy(dst->mac, s_clients[i].mac, sizeof(dst->mac));
        strlcpy(dst->arch, s_clients[i].arch, sizeof(dst->arch));
        strlcpy(dst->os, s_clients[i].os, sizeof(dst->os));
        strlcpy(dst->version, s_clients[i].version, sizeof(dst->version));
        strlcpy(dst->ip, s_clients[i].peer, sizeof(dst->ip));

        dst->instance = s_clients[i].instance;
        dst->protocol_ver = s_clients[i].protocol_ver;
        dst->connected = true;
        dst->volume_percent = s_clients[i].volume_percent;
        dst->muted = s_clients[i].muted;
        dst->latency_ms = s_clients[i].latency_ms;
        dst->last_seen_sec = s_clients[i].last_seen_sec;
        dst->last_seen_usec = s_clients[i].last_seen_usec;

        ++count;
    }
    portEXIT_CRITICAL(&s_clients_lock);

    /* An id is mandatory for the controller; fall back to the peer address. */
    for (size_t i = 0; i < count; ++i) {
        if (out[i].id[0] == '\0') {
            strlcpy(out[i].id, out[i].ip, sizeof(out[i].id));
        }
        if (out[i].name[0] == '\0') {
            strlcpy(out[i].name, out[i].id, sizeof(out[i].name));
        }
        if (out[i].hostname[0] == '\0') {
            strlcpy(out[i].hostname, out[i].ip, sizeof(out[i].hostname));
        }
    }

    return count;
}

/*
 * Finds a client by id and applies a mutation. Volume and latency changes
 * only update the bookkeeping; they take effect for the client on its next
 * ServerSettings message.
 */
static client_t *find_client_by_id_unsafe(const char *id)
{
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (s_clients[i].active &&
            strncmp(s_clients[i].id, id, sizeof(s_clients[i].id)) == 0) {
            return &s_clients[i];
        }
    }
    return NULL;
}

bool snapserver_set_client_volume(const char *id,
                                  int32_t percent,
                                  bool muted)
{
    if (id == NULL || id[0] == '\0') {
        return false;
    }

    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }

    bool found = false;
    client_t *client = NULL;

    portENTER_CRITICAL(&s_clients_lock);
    client = find_client_by_id_unsafe(id);
    if (client != NULL) {
        client->volume_percent = percent;
        client->muted = muted;
        found = true;
    }
    portEXIT_CRITICAL(&s_clients_lock);

    if (found) {
        ESP_LOGI(TAG,
                 "Volume for %s set to %ld%% (muted=%s)",
                 id,
                 (long)percent,
                 muted ? "true" : "false");
        /* Push the new setting immediately. */
        (void)send_server_settings(client, 0);
    }

    return found;
}

bool snapserver_set_client_latency(const char *id, int32_t latency_ms)
{
    if (id == NULL || id[0] == '\0') {
        return false;
    }

    bool found = false;
    client_t *client = NULL;

    portENTER_CRITICAL(&s_clients_lock);
    client = find_client_by_id_unsafe(id);
    if (client != NULL) {
        client->latency_ms = latency_ms;
        found = true;
    }
    portEXIT_CRITICAL(&s_clients_lock);

    if (found) {
        ESP_LOGI(TAG, "Latency for %s set to %ld ms", id, (long)latency_ms);
        (void)send_server_settings(client, 0);
    }

    return found;
}

bool snapserver_set_client_name(const char *id, const char *name)
{
    if (id == NULL || id[0] == '\0' || name == NULL) {
        return false;
    }

    bool found = false;

    portENTER_CRITICAL(&s_clients_lock);
    client_t *client = find_client_by_id_unsafe(id);
    if (client != NULL) {
        strlcpy(client->name, name, sizeof(client->name));
        found = true;
    }
    portEXIT_CRITICAL(&s_clients_lock);

    if (found) {
        ESP_LOGI(TAG, "Name for %s set to '%s'", id, name);
    }

    return found;
}

esp_err_t snapserver_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    {
        int32_t sec = 0;
        int32_t usec = 0;
        now_ts(&sec, &usec);
        ESP_LOGI(TAG,
                 "Server time base: %ld.%06ld (wall clock)",
                 (long)sec,
                 (long)usec);
    }

    s_chunk_pool = heap_caps_malloc(sizeof(chunk_slot_t) * CHUNK_POOL_SIZE,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_chunk_pool == NULL) {
        s_chunk_pool = heap_caps_malloc(sizeof(chunk_slot_t) * CHUNK_POOL_SIZE,
                                        MALLOC_CAP_8BIT);
    }
    if (s_chunk_pool == NULL) {
        ESP_LOGE(TAG, "No memory for the %d-slot chunk pool", CHUNK_POOL_SIZE);
        return ESP_ERR_NO_MEM;
    }
    memset(s_chunk_pool, 0, sizeof(chunk_slot_t) * CHUNK_POOL_SIZE);
    s_chunk_write = 0;
    s_chunk_sequence = 0;

    memset(s_clients, 0, sizeof(s_clients));
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        s_clients[i].fd = -1;
        s_clients[i].volume_percent = 100;
        s_clients[i].instance = 1;
        s_clients[i].protocol_ver = SNAP_PROTOCOL_VER;
        s_clients[i].send_mutex = xSemaphoreCreateMutex();
        s_clients[i].tx_queue = xQueueCreate(CLIENT_TX_QUEUE_DEPTH, sizeof(tx_item_t));
        s_clients[i].tx_payload = heap_caps_malloc(12U + AUDIO_MAX_OPUS_PACKET,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_clients[i].tx_payload == NULL) {
            s_clients[i].tx_payload = heap_caps_malloc(12U + AUDIO_MAX_OPUS_PACKET,
                                                       MALLOC_CAP_8BIT);
        }

        char task_name[16];
        snprintf(task_name, sizeof(task_name), "snapsend%d", i);
        bool sender_ok =
            s_clients[i].send_mutex != NULL && s_clients[i].tx_queue != NULL &&
            s_clients[i].tx_payload != NULL &&
            xTaskCreatePinnedToCoreWithCaps(sender_task,
                                            task_name,
                                            SENDER_TASK_STACK,
                                            &s_clients[i],
                                            SENDER_TASK_PRIORITY,
                                            &s_clients[i].tx_task,
                                            0,
                                            TASK_STACK_CAPS) == pdPASS;

        if (sender_ok) {
            snprintf(task_name, sizeof(task_name), "snapconn%d", i);
            sender_ok = xTaskCreatePinnedToCoreWithCaps(client_task,
                                                        task_name,
                                                        CLIENT_TASK_STACK,
                                                        &s_clients[i],
                                                        5,
                                                        &s_clients[i].task,
                                                        0,
                                                        TASK_STACK_CAPS) == pdPASS;
        }

        if (!sender_ok) {
            ESP_LOGE(TAG, "Could not set up sender for client slot %d", i);
            for (int j = 0; j <= i; ++j) {
                if (s_clients[j].task != NULL) {
                    vTaskDeleteWithCaps(s_clients[j].task);
                    s_clients[j].task = NULL;
                }
                if (s_clients[j].tx_task != NULL) {
                    vTaskDeleteWithCaps(s_clients[j].tx_task);
                    s_clients[j].tx_task = NULL;
                }
                if (s_clients[j].tx_queue != NULL) {
                    vQueueDelete(s_clients[j].tx_queue);
                    s_clients[j].tx_queue = NULL;
                }
                free(s_clients[j].tx_payload);
                s_clients[j].tx_payload = NULL;
                if (s_clients[j].send_mutex != NULL) {
                    vSemaphoreDelete(s_clients[j].send_mutex);
                    s_clients[j].send_mutex = NULL;
                }
            }
            return ESP_ERR_NO_MEM;
        }
    }

    if (xTaskCreatePinnedToCore(server_task,
                                "snapserver",
                                SERVER_TASK_STACK,
                                NULL,
                                5,
                                &s_server_task,
                                0) != pdPASS) {
        goto fail;
    }

    if (xTaskCreatePinnedToCore(stats_task,
                                "snapstats",
                                STATS_TASK_STACK,
                                NULL,
                                STATS_TASK_PRIORITY,
                                NULL,
                                0) != pdPASS) {
        vTaskDelete(s_server_task);
        s_server_task = NULL;
        goto fail;
    }

    if (xTaskCreatePinnedToCore(audio_task,
                                "opus_audio",
                                AUDIO_TASK_STACK,
                                NULL,
                                6,
                                &s_audio_task,
                                1) != pdPASS) {
        vTaskDelete(s_server_task);
        s_server_task = NULL;
        goto fail;
    }

    s_started = true;

    /*
     * The fan-out added MAX_CLIENTS sender tasks; if internal RAM ran short
     * here, the next task creation to fail would be the JSON-RPC control
     * connection, which looks like "clients no longer listed" rather than
     * like an out-of-memory error. Worth seeing in the log.
     */
    ESP_LOGI(TAG,
             "Free heap after start: %u B internal, %u B largest block",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    return ESP_OK;

fail:
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (s_clients[i].send_mutex != NULL) {
            vSemaphoreDelete(s_clients[i].send_mutex);
            s_clients[i].send_mutex = NULL;
        }
    }
    return ESP_ERR_NO_MEM;
}
