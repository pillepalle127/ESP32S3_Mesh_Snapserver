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
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "audio_opus.h"

static const char *TAG = "SNAPSERVER";

#define MAX_CLIENTS             SNAPSERVER_MAX_CLIENTS
#define RX_MAX                  4096
#define SNAP_PROTOCOL_VER       2
#define CLIENT_TASK_STACK       8192
#define SERVER_TASK_STACK       8192
#define AUDIO_TASK_STACK        8192

/* Single socket write timeout. */
#define CLIENT_SEND_TIMEOUT_US  2000000

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
    uint32_t time_msgs;
} client_t;

static client_t s_clients[MAX_CLIENTS];
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
    return esp_timer_get_time() + s_wall_offset_us;
}

static void now_ts(int32_t *sec, int32_t *usec)
{
    const int64_t t = now_us();
    *sec = (int32_t)(t / 1000000LL);
    *usec = (int32_t)(t % 1000000LL);
}

static int send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    while (len > 0U) {
        const int n = send(fd, p, len, 0);
        if (n < 0 && errno == EINTR) {
            continue;
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
    }
    return 0;
}

static int recv_all(int fd, void *data, size_t len)
{
    uint8_t *p = (uint8_t *)data;

    while (len > 0U) {
        const int n = recv(fd, p, len, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
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

    if (send_all(fd, &base, sizeof(base)) < 0) {
        return -1;
    }
    if (payload_size > 0U && send_all(fd, payload, payload_size) < 0) {
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

    const int json_len = snprintf(
        json,
        sizeof(json),
        "{\"bufferMs\":1000,\"latency\":%ld,\"muted\":%s,\"volume\":%ld}",
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
             "Sending ServerSettings: bufferMs=1000 latency=%ld muted=%s volume=%ld",
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

static int send_wire_chunk(client_t *client,
                           const audio_opus_packet_t *packet)
{
    if (packet == NULL || packet->data == NULL ||
        packet->size > AUDIO_MAX_OPUS_PACKET) {
        return -1;
    }

    uint8_t payload[12U + AUDIO_MAX_OPUS_PACKET];

    /*
     * packet->timestamp_us originates from esp_timer (uptime) and must be
     * expressed in the same wall clock domain that now_ts() reports.
     *
     * s_wall_offset_us is re-anchored when a plausible wall clock is learned.
     * Adding the same offset to the monotonic capture timestamp keeps message
     * headers and audio chunks in one consistent time domain.
     */
    const int64_t chunk_us = packet->timestamp_us + s_wall_offset_us;

    const int32_t sec = (int32_t)(chunk_us / 1000000LL);
    const int32_t usec = (int32_t)(chunk_us % 1000000LL);
    const uint32_t size = (uint32_t)packet->size;

    memcpy(payload + 0, &sec, 4);
    memcpy(payload + 4, &usec, 4);
    memcpy(payload + 8, &size, 4);
    memcpy(payload + 12, packet->data, packet->size);

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
    } else {
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
    if (!s_clock_synced &&
        base->sent_sec > SNAP_CLOCK_PLAUSIBLE_MIN_SEC) {

        const int64_t skew_us = client_send_us - server_recv_us;

        if (skew_us > 60000000LL || skew_us < -60000000LL) {
            /* Re-anchor the monotonic timebase; no POSIX clock involved. */
            s_wall_offset_us = client_send_us - esp_timer_get_time();

            ESP_LOGW(TAG,
                     "Adopted wall clock from %s: %ld s (skew was %lld s)",
                     client->peer,
                     (long)base->sent_sec,
                     (long long)(skew_us / 1000000LL));

            now_ts(&server_recv_sec, &server_recv_usec);
            server_recv_us =
                (int64_t)server_recv_sec * 1000000LL + server_recv_usec;
        }

        s_clock_synced = true;
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
    if (recv_all(fd, base, sizeof(*base)) < 0) {
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
    uint32_t times = 0;

    portENTER_CRITICAL(&s_clients_lock);
    fd = client->fd;
    chunks = client->chunks_sent;
    bytes = client->chunk_bytes;
    errors = client->chunk_errors;
    times = client->time_msgs;
    client->ready = false;
    client->active = false;
    client->fd = -1;
    client->task = NULL;
    portEXIT_CRITICAL(&s_clients_lock);

    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }

    xSemaphoreGive(client->send_mutex);

    ESP_LOGI(TAG,
             "Session summary %s: chunks_sent=%lu bytes=%lu send_errors=%lu "
             "time_msgs=%lu",
             client->peer,
             (unsigned long)chunks,
             (unsigned long)bytes,
             (unsigned long)errors,
             (unsigned long)times);
}

static void client_task(void *arg)
{
    client_t *client = (client_t *)arg;
    int fd = -1;

    portENTER_CRITICAL(&s_clients_lock);
    fd = client->fd;
    portEXIT_CRITICAL(&s_clients_lock);

    snap_base_t base;
    uint8_t *payload = NULL;

    if (recv_message(fd, &base, &payload) < 0 ||
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
        if (rc < 0) {
            ESP_LOGW(TAG,
                     "Client receive ended: %s rc=%d errno=%d (%s)",
                     client->peer,
                     rc,
                     errno,
                     strerror(errno));
            break;
        }

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
    vTaskDelete(NULL);
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

static void audio_task(void *arg)
{
    (void)arg;

    int64_t last_frame_start_us = 0;
    int64_t stats_start_us = esp_timer_get_time();
    int64_t delta_sum_us = 0;
    int64_t delta_min_us = INT64_MAX;
    int64_t delta_max_us = 0;
    uint32_t delta_count = 0;
    uint32_t last_chunks[MAX_CLIENTS] = {0};

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

            for (int i = 0; i < count; ++i) {
                if (send_wire_chunk(clients[i], &packet) < 0) {
                    ESP_LOGW(TAG,
                             "Wire chunk send failed (%s); closing client",
                             clients[i]->peer);
                    mark_client_failed(clients[i]);
                }
            }
        } else {
            ESP_LOGE(TAG, "Audio frame failed: %s", esp_err_to_name(audio_result));
        }

        const int64_t stats_now_us = esp_timer_get_time();
        if (delta_count > 0U && stats_now_us - stats_start_us >= 1000000LL) {
            ESP_LOGI(TAG,
                     "frame delta: avg=%lld us min=%lld us max=%lld us samples=%lu",
                     (long long)(delta_sum_us / (int64_t)delta_count),
                     (long long)delta_min_us,
                     (long long)delta_max_us,
                     (unsigned long)delta_count);

            for (int i = 0; i < MAX_CLIENTS; ++i) {
                bool active = false;
                bool ready = false;
                uint32_t chunks = 0;
                uint32_t bytes = 0;
                uint32_t errs = 0;
                uint32_t times = 0;

                portENTER_CRITICAL(&s_clients_lock);
                active = s_clients[i].active;
                ready = s_clients[i].ready;
                chunks = s_clients[i].chunks_sent;
                bytes = s_clients[i].chunk_bytes;
                errs = s_clients[i].chunk_errors;
                times = s_clients[i].time_msgs;
                portEXIT_CRITICAL(&s_clients_lock);

                /* A reconnect resets the counter; avoid an unsigned wrap. */
                const uint32_t chunk_rate =
                    (chunks >= last_chunks[i])
                        ? (chunks - last_chunks[i])
                        : chunks;

                if (active) {
                    ESP_LOGI(TAG,
                             "client[%d] %s ready=%d chunks/s=%lu total=%lu "
                             "bytes=%lu send_errors=%lu time_msgs=%lu",
                             i,
                             s_clients[i].peer,
                             (int)ready,
                             (unsigned long)chunk_rate,
                             (unsigned long)chunks,
                             (unsigned long)bytes,
                             (unsigned long)errs,
                             (unsigned long)times);
                }
                last_chunks[i] = chunks;
            }

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
                s_clients[i].task = NULL;
                s_clients[i].chunks_sent = 0;
                s_clients[i].chunk_bytes = 0;
                s_clients[i].chunk_errors = 0;
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
                slot = i;
                break;
            }
        }
        portEXIT_CRITICAL(&s_clients_lock);

        if (slot < 0) {
            ESP_LOGW(TAG, "Rejecting client: all slots occupied");
            shutdown(fd, SHUT_RDWR);
            close(fd);
            continue;
        }

        strlcpy(s_clients[slot].peer, peer_ip, sizeof(s_clients[slot].peer));

        ESP_LOGI(TAG,
                 "Client connected slot=%d ip=%s fd=%d",
                 slot,
                 s_clients[slot].peer,
                 fd);

        if (xTaskCreatePinnedToCore(client_task,
                                    "snap_client",
                                    CLIENT_TASK_STACK,
                                    &s_clients[slot],
                                    5,
                                    &s_clients[slot].task,
                                    0) != pdPASS) {
            ESP_LOGE(TAG, "Could not create client task");
            close_client(&s_clients[slot]);
        }
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

    memset(s_clients, 0, sizeof(s_clients));
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        s_clients[i].fd = -1;
        s_clients[i].volume_percent = 100;
        s_clients[i].instance = 1;
        s_clients[i].protocol_ver = SNAP_PROTOCOL_VER;
        s_clients[i].send_mutex = xSemaphoreCreateMutex();
        if (s_clients[i].send_mutex == NULL) {
            for (int j = 0; j < i; ++j) {
                vSemaphoreDelete(s_clients[j].send_mutex);
                s_clients[j].send_mutex = NULL;
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
