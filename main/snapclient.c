/**
 * @file snapclient.c
 * @brief Snapcast protocol client -- TCP connection, Hello, CodecHeader,
 *        WireChunk/Opus decode, feeding audio_sink.c.
 *
 * See snapclient.h for provenance. Our own server never sends the ready
 * flag conditional on a client Time message (see snapserver.c's
 * client_task(): ready is set right after CodecHeader, before any Time
 * exchange), so this client does not send SNAP_MSG_TIME at all yet --
 * that is Stufe 2's job (drift compensation needs it, plain playback
 * doesn't).
 */
#include "snapclient.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "audio_sink.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "opus.h"

static const char *TAG = "SNAPCLIENT";

typedef enum {
    SNAP_MSG_BASE            = 0,
    SNAP_MSG_CODEC_HEADER    = 1,
    SNAP_MSG_WIRE_CHUNK      = 2,
    SNAP_MSG_SERVER_SETTINGS = 3,
    SNAP_MSG_TIME            = 4,
    SNAP_MSG_HELLO           = 5,
} snap_msg_type_t;

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

#define EVT_NETWORK_AVAILABLE BIT0
#define EVT_STATE_CHANGED     BIT1

#define SNAP_SAMPLE_RATE_DEFAULT 48000
#define SNAP_CHANNELS_DEFAULT    1

/* Opus allows up to 120 ms per packet at 48 kHz. */
#define OPUS_MAX_FRAME_SAMPLES 5760
#define OPUS_PCM_MAX_CHANNELS  2
#define OPUS_PCM_BUFFER_SAMPLES (OPUS_MAX_FRAME_SAMPLES * OPUS_PCM_MAX_CHANNELS)
#define OPUS_PCM_BUFFER_BYTES (OPUS_PCM_BUFFER_SAMPLES * sizeof(opus_int16))

#define SNAP_MESSAGE_BUFFER_SIZE 8192
#define SNAP_DISCARD_BUFFER_SIZE 2048

#define SNAP_TASK_STACK_SIZE 12288
#define SNAP_TASK_PRIORITY   6
#define SNAP_TASK_CORE       0

#define SNAP_CONNECT_TIMEOUT_MS  1500
#define SNAP_CONNECT_RETRY_MS     500
#define SNAP_RECONNECT_DELAY_MS  1000

static int s_sock = -1;
static volatile bool s_run = true;
static volatile bool s_network_available;
static bool s_task_started;
static EventGroupHandle_t s_evt;

static char s_host[64];
static uint16_t s_port = 1704;

static char s_codec[16];
static uint32_t s_codec_sample_rate = SNAP_SAMPLE_RATE_DEFAULT;
static uint16_t s_codec_channels = SNAP_CHANNELS_DEFAULT;

static OpusDecoder *s_opus_decoder;
static opus_int16 *s_opus_pcm;
static int16_t *s_mono_pcm; /* opus_decode() output down-mixed to mono */

static int send_full(int socket_fd, const void *buffer, size_t length)
{
    const uint8_t *data = (const uint8_t *)buffer;
    size_t sent_total = 0;

    while (sent_total < length) {
        const int sent = send(socket_fd, data + sent_total, length - sent_total, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (sent == 0) {
            return -1;
        }
        sent_total += (size_t)sent;
    }
    return 0;
}

static int read_full(int socket_fd, void *buffer, size_t length)
{
    uint8_t *data = (uint8_t *)buffer;
    size_t received_total = 0;

    while (received_total < length) {
        const int received = recv(socket_fd, data + received_total, length - received_total, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (received == 0) {
            return -1;
        }
        received_total += (size_t)received;
    }
    return 0;
}

static int tcp_connect(void)
{
    if (!s_network_available || !s_run) {
        errno = ENETDOWN;
        return -1;
    }

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;
    char port_text[8];

    if (snprintf(port_text, sizeof(port_text), "%u", (unsigned)s_port) >= (int)sizeof(port_text)) {
        return -1;
    }

    if (getaddrinfo(s_host, port_text, &hints, &result) != 0 || result == NULL) {
        ESP_LOGW(TAG, "getaddrinfo failed for %s", s_host);
        return -1;
    }

    const int socket_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (socket_fd < 0) {
        ESP_LOGW(TAG, "socket() failed, errno=%d", errno);
        freeaddrinfo(result);
        return -1;
    }

    const int original_flags = fcntl(socket_fd, F_GETFL, 0);
    if (original_flags < 0 || fcntl(socket_fd, F_SETFL, original_flags | O_NONBLOCK) < 0) {
        ESP_LOGW(TAG, "Setting socket nonblocking failed, errno=%d", errno);
        close(socket_fd);
        freeaddrinfo(result);
        return -1;
    }

    const int connect_result = connect(socket_fd, result->ai_addr, result->ai_addrlen);
    if (connect_result != 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "connect() to %s:%u failed immediately (errno=%d)",
                 s_host, (unsigned)s_port, errno);
        close(socket_fd);
        freeaddrinfo(result);
        return -1;
    }

    if (connect_result != 0) {
        fd_set write_set;
        fd_set error_set;
        FD_ZERO(&write_set);
        FD_ZERO(&error_set);
        FD_SET(socket_fd, &write_set);
        FD_SET(socket_fd, &error_set);

        struct timeval timeout = {
            .tv_sec = SNAP_CONNECT_TIMEOUT_MS / 1000,
            .tv_usec = (SNAP_CONNECT_TIMEOUT_MS % 1000) * 1000,
        };

        const int select_result = select(socket_fd + 1, NULL, &write_set, &error_set, &timeout);
        if (select_result <= 0) {
            ESP_LOGW(TAG, "TCP connect to %s:%u aborted (%s)",
                     s_host, (unsigned)s_port, select_result == 0 ? "timeout" : "select error");
            close(socket_fd);
            freeaddrinfo(result);
            return -1;
        }

        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0 ||
            socket_error != 0) {
            ESP_LOGW(TAG, "TCP connect to %s:%u failed (errno=%d)",
                     s_host, (unsigned)s_port, socket_error);
            close(socket_fd);
            freeaddrinfo(result);
            return -1;
        }
    }

    freeaddrinfo(result);

    if (!s_network_available || !s_run) {
        shutdown(socket_fd, SHUT_RDWR);
        close(socket_fd);
        errno = ENETDOWN;
        return -1;
    }

    if (fcntl(socket_fd, F_SETFL, original_flags) < 0) {
        ESP_LOGW(TAG, "Resetting socket to blocking failed, errno=%d", errno);
        close(socket_fd);
        return -1;
    }

    ESP_LOGI(TAG, "Connected to Snapserver %s:%u", s_host, (unsigned)s_port);
    return socket_fd;
}

static int send_hello(int socket_fd)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGE(TAG, "Could not read STA MAC");
        return -1;
    }

    char mac_text[18];
    snprintf(mac_text, sizeof(mac_text), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char client_name[32];
    snprintf(client_name, sizeof(client_name), "ESP32S3-SnapMesh-%02X%02X", mac[4], mac[5]);

    char hello_json[384];
    const int hello_length = snprintf(hello_json, sizeof(hello_json),
        "{"
        "\"MAC\":\"%s\","
        "\"HostName\":\"%s\","
        "\"Version\":\"0.27.0\","
        "\"ClientName\":\"%s\","
        "\"OS\":\"esp-idf\","
        "\"Arch\":\"xtensa\","
        "\"Instance\":1,"
        "\"SnapStreamProtocolVersion\":2"
        "}",
        mac_text, client_name, client_name);

    if (hello_length <= 0 || hello_length >= (int)sizeof(hello_json)) {
        ESP_LOGE(TAG, "Hello JSON invalid or too long");
        return -1;
    }

    const uint32_t payload_length = (uint32_t)hello_length;
    snap_base_t header = {0};
    header.type = SNAP_MSG_HELLO;
    header.size = (uint32_t)sizeof(payload_length) + payload_length;

    if (send_full(socket_fd, &header, sizeof(header)) != 0 ||
        send_full(socket_fd, &payload_length, sizeof(payload_length)) != 0 ||
        send_full(socket_fd, hello_json, payload_length) != 0) {
        ESP_LOGE(TAG, "Sending Hello failed");
        return -1;
    }

    ESP_LOGI(TAG, "Sent Hello: Client=%s, ID=%s", client_name, mac_text);
    return 0;
}

static esp_err_t opus_decoder_prepare(void)
{
    if (s_opus_decoder != NULL && s_opus_pcm != NULL) {
        return ESP_OK;
    }

    if (s_opus_pcm == NULL) {
        s_opus_pcm = heap_caps_malloc(OPUS_PCM_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_opus_pcm == NULL) {
            s_opus_pcm = heap_caps_malloc(OPUS_PCM_BUFFER_BYTES, MALLOC_CAP_8BIT);
        }
        if (s_opus_pcm == NULL) {
            ESP_LOGE(TAG, "No memory for %u B Opus PCM buffer", (unsigned)OPUS_PCM_BUFFER_BYTES);
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_mono_pcm == NULL) {
        s_mono_pcm = heap_caps_malloc(OPUS_MAX_FRAME_SAMPLES * sizeof(int16_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_mono_pcm == NULL) {
            s_mono_pcm = heap_caps_malloc(OPUS_MAX_FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT);
        }
        if (s_mono_pcm == NULL) {
            ESP_LOGE(TAG, "No memory for mono downmix buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    int opus_error = OPUS_OK;
    s_opus_decoder = opus_decoder_create((opus_int32)s_codec_sample_rate,
                                         (int)s_codec_channels, &opus_error);
    if (s_opus_decoder == NULL || opus_error != OPUS_OK) {
        ESP_LOGE(TAG, "opus_decoder_create failed: %s (%d)",
                 opus_strerror(opus_error), opus_error);
        s_opus_decoder = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Opus decoder ready: %u Hz, %u channel(s)",
             (unsigned)s_codec_sample_rate, (unsigned)s_codec_channels);
    return ESP_OK;
}

static void opus_decoder_reset(void)
{
    if (s_opus_decoder != NULL) {
        opus_decoder_destroy(s_opus_decoder);
        s_opus_decoder = NULL;
    }
}

/*
 * Snapcast CodecHeader:
 *   uint32_t codec_name_length
 *   char     codec_name[codec_name_length]
 *   uint32_t codec_specific_length
 *   uint8_t  codec_specific[codec_specific_length]
 *
 * Our own server's codec_specific payload for "opus" (see snapserver.c's
 * send_codec_header()) is 12 bytes: uint32 opus_id, uint32 rate, uint16
 * bits, uint16 channels. Only "opus" is handled -- our server never sends
 * "pcm".
 */
static void handle_codec_header(const uint8_t *payload, uint32_t size)
{
    if (payload == NULL || size < sizeof(uint32_t)) {
        ESP_LOGW(TAG, "CodecHeader too small: %lu B", (unsigned long)size);
        return;
    }

    uint32_t name_length = 0;
    memcpy(&name_length, payload, sizeof(name_length));

    if (name_length == 0 || name_length >= sizeof(s_codec) ||
        sizeof(uint32_t) + name_length + sizeof(uint32_t) > size) {
        ESP_LOGE(TAG, "CodecHeader invalid: size=%lu name_len=%lu",
                 (unsigned long)size, (unsigned long)name_length);
        return;
    }

    memset(s_codec, 0, sizeof(s_codec));
    memcpy(s_codec, payload + sizeof(uint32_t), name_length);

    const uint8_t *specific = payload + sizeof(uint32_t) + name_length;
    uint32_t specific_length = 0;
    memcpy(&specific_length, specific, sizeof(specific_length));
    specific += sizeof(specific_length);

    const uint32_t specific_available =
        size - (uint32_t)(sizeof(uint32_t) + name_length + sizeof(uint32_t));
    if (specific_length > specific_available) {
        ESP_LOGE(TAG, "CodecHeader specific data truncated");
        return;
    }

    opus_decoder_reset();

    if (strcmp(s_codec, "opus") != 0) {
        ESP_LOGW(TAG, "Unsupported codec: %s", s_codec);
        return;
    }

    if (specific_length != 12U) {
        ESP_LOGW(TAG,
                 "Unexpected opus codec-specific length %lu, falling back to "
                 "%u Hz / %u channel(s)",
                 (unsigned long)specific_length, SNAP_SAMPLE_RATE_DEFAULT, SNAP_CHANNELS_DEFAULT);
        s_codec_sample_rate = SNAP_SAMPLE_RATE_DEFAULT;
        s_codec_channels = SNAP_CHANNELS_DEFAULT;
    } else {
        uint32_t rate = SNAP_SAMPLE_RATE_DEFAULT;
        uint16_t bits = 16;
        uint16_t channels = SNAP_CHANNELS_DEFAULT;
        memcpy(&rate, specific + 4, sizeof(rate));
        memcpy(&bits, specific + 8, sizeof(bits));
        memcpy(&channels, specific + 10, sizeof(channels));
        (void)bits; /* S16LE is the only format micro-opus decodes to here */

        s_codec_sample_rate = rate;
        s_codec_channels = (channels == 0U || channels > OPUS_PCM_MAX_CHANNELS) ? 1U : channels;
    }

    ESP_LOGI(TAG, "CodecHeader: codec=%s, rate=%u, channels=%u",
             s_codec, (unsigned)s_codec_sample_rate, (unsigned)s_codec_channels);

    if (opus_decoder_prepare() != ESP_OK) {
        ESP_LOGE(TAG, "Opus decoder init failed after CodecHeader");
    }
}

/*
 * Snapcast WireChunk: int32 sec, int32 usec, uint32 audio_length, uint8_t
 * audio[audio_length]. The timestamp is unused in Stufe 1 (no time sync
 * yet, see the file header comment); Stufe 2 will read it here.
 */
static void handle_wire_chunk(const uint8_t *payload, uint32_t size)
{
    if (payload == NULL || size < 12U) {
        return;
    }

    uint32_t audio_length = 0;
    memcpy(&audio_length, payload + 8, sizeof(audio_length));
    if (audio_length == 0U || audio_length > size - 12U) {
        return;
    }

    if (s_codec[0] == '\0' || s_opus_decoder == NULL || s_opus_pcm == NULL) {
        return;
    }

    const uint8_t *audio = payload + 12;
    const int samples_per_channel = opus_decode(s_opus_decoder, audio, (opus_int32)audio_length,
                                                s_opus_pcm, OPUS_MAX_FRAME_SAMPLES, 0);
    if (samples_per_channel < 0) {
        ESP_LOGW(TAG, "opus_decode failed: %s (%d)",
                 opus_strerror(samples_per_channel), samples_per_channel);
        return;
    }

    if (s_codec_channels == 1U) {
        audio_sink_feed_network(s_opus_pcm, (size_t)samples_per_channel);
        return;
    }

    /* Down-mix stereo to mono to match the DSP/output stage, which always
     * runs on a single channel (see audio_i2s.c). */
    for (int i = 0; i < samples_per_channel; ++i) {
        const int32_t left = s_opus_pcm[2 * i];
        const int32_t right = s_opus_pcm[2 * i + 1];
        s_mono_pcm[i] = (int16_t)((left + right) / 2);
    }
    audio_sink_feed_network(s_mono_pcm, (size_t)samples_per_channel);
}

static void process_message(const snap_base_t *header, const uint8_t *payload)
{
    switch (header->type) {
        case SNAP_MSG_CODEC_HEADER:
            handle_codec_header(payload, header->size);
            break;
        case SNAP_MSG_WIRE_CHUNK:
            handle_wire_chunk(payload, header->size);
            break;
        case SNAP_MSG_SERVER_SETTINGS:
        case SNAP_MSG_TIME:
        case SNAP_MSG_BASE:
        case SNAP_MSG_HELLO:
        default:
            break;
    }
}

static void connection_loop(int socket_fd)
{
    static uint8_t discard_buffer[SNAP_DISCARD_BUFFER_SIZE];
    static uint8_t message_buffer[SNAP_MESSAGE_BUFFER_SIZE];

    audio_sink_set_network_active(true);

    while (s_run) {
        snap_base_t header;
        if (read_full(socket_fd, &header, sizeof(header)) != 0) {
            break;
        }

        if (header.size == 0U) {
            vTaskDelay(1);
            continue;
        }

        if (header.size > sizeof(message_buffer)) {
            ESP_LOGW(TAG, "Payload too large: %lu B, discarding", (unsigned long)header.size);
            uint32_t remaining = header.size;
            while (remaining > 0U && s_run) {
                const uint32_t part =
                    (remaining > sizeof(discard_buffer)) ? (uint32_t)sizeof(discard_buffer) : remaining;
                if (read_full(socket_fd, discard_buffer, part) != 0) {
                    audio_sink_set_network_active(false);
                    return;
                }
                remaining -= part;
                vTaskDelay(1);
            }
            continue;
        }

        if (read_full(socket_fd, message_buffer, header.size) != 0) {
            break;
        }

        process_message(&header, message_buffer);

        /* One tick of slack per message so a burst of 20 ms Opus packets
         * (~50/s) can't starve the idle task under TCP backpressure. */
        vTaskDelay(1);
    }

    audio_sink_set_network_active(false);
}

static void wait_for_reconnect_condition(TickType_t timeout_ticks)
{
    xEventGroupWaitBits(s_evt, EVT_NETWORK_AVAILABLE | EVT_STATE_CHANGED,
                        pdTRUE, pdFALSE, timeout_ticks);
}

static void snap_task(void *arg)
{
    (void)arg;

    while (s_run) {
        if (!s_network_available) {
            xEventGroupWaitBits(s_evt, EVT_NETWORK_AVAILABLE, pdFALSE, pdTRUE, portMAX_DELAY);
            continue;
        }

        const int socket_fd = tcp_connect();
        if (socket_fd < 0) {
            if (s_run && s_network_available) {
                wait_for_reconnect_condition(pdMS_TO_TICKS(SNAP_CONNECT_RETRY_MS));
            }
            continue;
        }

        if (!s_network_available) {
            shutdown(socket_fd, SHUT_RDWR);
            close(socket_fd);
            continue;
        }

        s_sock = socket_fd;
        memset(s_codec, 0, sizeof(s_codec));
        opus_decoder_reset();

        if (send_hello(socket_fd) == 0) {
            connection_loop(socket_fd);
        }

        shutdown(socket_fd, SHUT_RDWR);
        close(socket_fd);
        if (s_sock == socket_fd) {
            s_sock = -1;
        }
        opus_decoder_reset();

        if (s_run && s_network_available) {
            ESP_LOGI(TAG, "Connection lost, reconnecting in %d ms", SNAP_RECONNECT_DELAY_MS);
            wait_for_reconnect_condition(pdMS_TO_TICKS(SNAP_RECONNECT_DELAY_MS));
        }
    }

    audio_sink_set_network_active(false);
    opus_decoder_reset();
    s_sock = -1;
    s_task_started = false;
    vTaskDelete(NULL);
}

void snapclient_set_network_available(bool available)
{
    if (s_network_available == available) {
        return;
    }

    s_network_available = available;

    if (available) {
        ESP_LOGI(TAG, "Network up: reconnect released");
        if (s_evt != NULL) {
            xEventGroupSetBits(s_evt, EVT_NETWORK_AVAILABLE | EVT_STATE_CHANGED);
        }
        return;
    }

    ESP_LOGW(TAG, "Network down: aborting any open connection");
    if (s_evt != NULL) {
        xEventGroupClearBits(s_evt, EVT_NETWORK_AVAILABLE);
        xEventGroupSetBits(s_evt, EVT_STATE_CHANGED);
    }

    const int socket_fd = s_sock;
    if (socket_fd >= 0) {
        shutdown(socket_fd, SHUT_RDWR);
    }
}

esp_err_t snapclient_start(const char *host, uint16_t port)
{
    if (s_task_started) {
        return ESP_OK;
    }
    if (host == NULL || host[0] == '\0' || port == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_host, host, sizeof(s_host));
    s_port = port;

    if (s_evt == NULL) {
        s_evt = xEventGroupCreate();
        if (s_evt == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_run = true;
    s_task_started = true;
    s_sock = -1;
    memset(s_codec, 0, sizeof(s_codec));

    if (s_network_available) {
        xEventGroupSetBits(s_evt, EVT_NETWORK_AVAILABLE);
    } else {
        xEventGroupClearBits(s_evt, EVT_NETWORK_AVAILABLE);
    }

    if (xTaskCreatePinnedToCore(snap_task, "snapclient", SNAP_TASK_STACK_SIZE, NULL,
                                SNAP_TASK_PRIORITY, NULL, SNAP_TASK_CORE) != pdPASS) {
        s_task_started = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Snapclient started (server %s:%u)", s_host, (unsigned)s_port);
    return ESP_OK;
}
