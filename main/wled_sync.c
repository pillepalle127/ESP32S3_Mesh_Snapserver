/**
 * @file wled_sync.c
 * @brief Finds WLEDs, forwards a port to them and sends them the analysis,
 *        see wled_sync.h.
 */
#include "wled_sync.h"

#include <errno.h>
#include <string.h>

#include "audio_i2s.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip.h"
#include "lwip/lwip_napt.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/sockets.h"
#include "provisioning.h"
#include "sdkconfig.h"
#include "wled_analysis.h"
#include "wled_proxy.h"
#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
#include "esp_mesh_lite.h"
#endif

static const char *TAG = "WLED_SYNC";

/* WLED's default port for AudioReactive sync. */
#define SYNC_PORT          11988

/* Power of two, holding the FFT window plus one frame: the frame being
 * written never overlaps the window being read. */
#define RING_SAMPLES       2048U
#define RING_MASK          (RING_SAMPLES - 1U)
#define MAX_FEED_SAMPLES   (RING_SAMPLES - WLED_ANALYSIS_FFT_SIZE)

/* Below the audio tasks (5, 6) and alongside the stats tasks. */
#define TASK_PRIORITY      2
#define TASK_STACK         4096
#define TASK_STACK_CAPS    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* While silent, one packet every 500 ms keeps WLED's sync state alive (it
 * reports "no data" after 2.5 s without a packet). */
#define IDLE_EVERY_FRAMES  25U
#define STATS_EVERY_US     (60LL * 1000000LL)

/*
 * Finding WLEDs. Every 5 s the station list of our own access point. A new
 * station is asked at once and, while it does not answer as a WLED, every
 * 10 s during its first minute -- a WLED that has just joined often has no
 * web server up yet (the first try missed by a minute, 2026-10-10) -- and
 * every minute after that.
 */
#define MAX_WLEDS          4
#define MAX_STATIONS       ESP_WIFI_MAX_CONN_NUM
#define FIND_PERIOD_MS     5000
#define REPROBE_US         (60LL * 1000000LL)
#define REPROBE_EARLY_US   (10LL * 1000000LL)
#define EARLY_US           (60LL * 1000000LL)
#define PROBE_TIMEOUT_MS   1500
#define PROBE_BODY_MAX     8192
#define FIND_TASK_PRIORITY 1
#define FIND_TASK_STACK    6144

/*
 * Reaching a WLED from the whole mesh. Each WLED gets a port of its own,
 * from its MAC, the same on every node on the way: the node it hangs on
 * forwards the port to the WLED, every client above it forwards it to the
 * node below, and the server takes the connection itself (wled_proxy.c).
 * A forward asked for from below lapses unless renewed, every 30 s.
 */
#define PORT_BASE          20000U
#define PORT_SPAN          20000U
#define RENEW_US           (30LL * 1000000LL)
#define RELAY_LAPSE_US     (90LL * 1000000LL)
#define MAX_RELAYS         8
#define RELAY_TIMEOUT_MS   3000

typedef struct {
    uint8_t  mac[6];
    uint32_t ip;          /* lwIP byte order, 0 = no lease yet */
    bool     present;
    bool     is_wled;
    int64_t  probed_us;   /* 0 = not asked yet */
    int64_t  seen_us;     /* when it joined (or got its lease) */
    uint16_t port;        /* its port on the mesh, from the MAC */
    bool     forwarded;   /* our own forward of that port to it is set */
    int64_t  asked_us;    /* last time its address in the mesh was settled */
    char     name[33];
    char     url[WLED_SYNC_URL_LEN]; /* where the mesh reaches it, "" = not yet */
} station_t;

/* A forward this node keeps for a node below it. */
typedef struct {
    uint16_t port;        /* 0 = free */
    uint32_t to;
    int64_t  until_us;
} relay_t;

static bool s_server_role;

/* Analysis, set up when the first WLED appears. */
static int16_t *s_ring;
static int16_t *s_window;
static wled_analysis_t *s_analysis;
static TaskHandle_t s_task;
/* Samples written so far, wrapping. Published after the copy, so a reader
 * that sees a count also sees the samples. */
static uint32_t s_written;
/* True while there is a WLED to feed. */
static volatile bool s_enabled;

/* Stations and relays: the finder task, the web server (requests from
 * below, status) and nobody else, under s_lock. */
static SemaphoreHandle_t s_lock;
static station_t s_stations[MAX_STATIONS];
static relay_t s_relays[MAX_RELAYS];
static uint32_t s_mapped_on; /* mesh address the forwards point from */

/* What the sync task sends to, written by the finder task. */
static portMUX_TYPE s_targets_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_targets[MAX_WLEDS];
static size_t s_target_count;

/* ------------------------------------------------------------------ */
/* Audio side                                                         */
/* ------------------------------------------------------------------ */

void wled_sync_feed(const int16_t *mono, size_t samples)
{
    if (!s_enabled || mono == NULL || samples == 0U || samples > MAX_FEED_SAMPLES) {
        return;
    }

    const uint32_t written = __atomic_load_n(&s_written, __ATOMIC_RELAXED);
    const uint32_t pos = written & RING_MASK;
    const size_t first = (samples < RING_SAMPLES - pos) ? samples : RING_SAMPLES - pos;
    memcpy(&s_ring[pos], mono, first * sizeof(int16_t));
    memcpy(s_ring, mono + first, (samples - first) * sizeof(int16_t));
    __atomic_store_n(&s_written, written + (uint32_t)samples, __ATOMIC_RELEASE);

    xTaskNotifyGive(s_task);
}

/* The last WLED_ANALYSIS_FFT_SIZE samples up to `written`, oldest first. */
static void copy_window(uint32_t written)
{
    const uint32_t start = (written - WLED_ANALYSIS_FFT_SIZE) & RING_MASK;
    const size_t first = (WLED_ANALYSIS_FFT_SIZE < RING_SAMPLES - start)
                             ? WLED_ANALYSIS_FFT_SIZE
                             : RING_SAMPLES - start;
    memcpy(s_window, &s_ring[start], first * sizeof(int16_t));
    memcpy(s_window + first, s_ring, (WLED_ANALYSIS_FFT_SIZE - first) * sizeof(int16_t));
}

static void sync_task(void *arg)
{
    (void)arg;
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(TAG, "No socket: errno %d", errno);
        vTaskSuspend(NULL);
    }
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(SYNC_PORT),
    };

    uint32_t read = __atomic_load_n(&s_written, __ATOMIC_ACQUIRE);
    uint32_t idle_frames = 0U;

    /* For the log line, see below. */
    int64_t stats_since_us = esp_timer_get_time();
    uint32_t frames = 0U;
    uint32_t sent = 0U;
    uint32_t skipped = 0U;
    uint32_t send_errors = 0U;
    uint32_t beats = 0U;
    float level_sum = 0.0f;
    float level_max = 0.0f;
    int64_t busy_us = 0;
    int64_t max_us = 0;

    for (;;) {
        const uint32_t notes = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        const uint32_t written = __atomic_load_n(&s_written, __ATOMIC_ACQUIRE);
        const uint32_t fresh = written - read;
        read = written;
        if (!s_enabled || notes == 0U || fresh == 0U) {
            continue;
        }
        if (notes > 1U) {
            skipped += notes - 1U;
        }

        const int64_t start_us = esp_timer_get_time();
        copy_window(written);
        wled_sync_packet_t packet;
        const bool active = wled_analysis_run(s_analysis, s_window,
                                              (fresh < WLED_ANALYSIS_FFT_SIZE) ? fresh
                                                                               : WLED_ANALYSIS_FFT_SIZE,
                                              &packet);
        const int64_t now_us = esp_timer_get_time();
        const int64_t took_us = now_us - start_us;
        busy_us += took_us;
        if (took_us > max_us) {
            max_us = took_us;
        }
        ++frames;
        beats += packet.sample_peak;
        level_sum += packet.sample_smth;
        if (packet.sample_smth > level_max) {
            level_max = packet.sample_smth;
        }

        uint32_t targets[MAX_WLEDS];
        size_t target_count;
        portENTER_CRITICAL(&s_targets_lock);
        target_count = s_target_count;
        memcpy(targets, s_targets, sizeof(targets));
        portEXIT_CRITICAL(&s_targets_lock);

        idle_frames = active ? 0U : idle_frames + 1U;
        if (active || idle_frames % IDLE_EVERY_FRAMES == 0U) {
            for (size_t i = 0; i < target_count; ++i) {
                dst.sin_addr.s_addr = targets[i];
                if (sendto(fd, &packet, sizeof(packet), MSG_DONTWAIT,
                           (const struct sockaddr *)&dst, sizeof(dst)) == (int)sizeof(packet)) {
                    ++sent;
                } else {
                    ++send_errors;
                }
            }
        }

        if (now_us - stats_since_us >= STATS_EVERY_US) {
            /* Analysis time per 20 ms frame is the CPU this costs. */
            /* Beats and level are what WLED gets: level 0-255. */
            ESP_LOGI(TAG,
                     "%u frames, %u sent to %u WLED(s), %u skipped, %u send errors, "
                     "%u beats, level avg %.0f max %.0f, "
                     "analysis avg %lld us max %lld us (%.1f %% of a core)",
                     (unsigned)frames, (unsigned)sent, (unsigned)target_count, (unsigned)skipped,
                     (unsigned)send_errors, (unsigned)beats,
                     (double)(frames ? level_sum / (float)frames : 0.0f), (double)level_max,
                     frames ? busy_us / frames : 0LL, max_us,
                     (double)busy_us * 100.0 / (double)(now_us - stats_since_us));
            stats_since_us = now_us;
            frames = sent = skipped = send_errors = beats = 0U;
            level_sum = level_max = 0.0f;
            busy_us = max_us = 0;
        }
    }
}

/* Takes the analysis buffers and starts its task, once. */
static esp_err_t start_analysis(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    /*
     * What the analysis touches every frame goes to internal RAM where
     * there is room -- from PSRAM the same work took about twice as long.
     * The ring is written by the audio path and read once per frame, so it
     * can stay in PSRAM.
     */
    s_ring = heap_caps_calloc(RING_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_window = heap_caps_calloc(WLED_ANALYSIS_FFT_SIZE, sizeof(int16_t),
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_window == NULL) {
        s_window = heap_caps_calloc(WLED_ANALYSIS_FFT_SIZE, sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    /* 16-byte aligned for esp-dsp's SIMD FFT. */
    s_analysis = heap_caps_aligned_calloc(16, 1, sizeof(*s_analysis),
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_analysis == NULL) {
        s_analysis = heap_caps_aligned_calloc(16, 1, sizeof(*s_analysis),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_ring == NULL || s_window == NULL || s_analysis == NULL) {
        ESP_LOGE(TAG, "No memory for the analysis");
        heap_caps_free(s_ring);
        heap_caps_free(s_window);
        heap_caps_free(s_analysis);
        s_ring = s_window = NULL;
        s_analysis = NULL;
        return ESP_ERR_NO_MEM;
    }
    wled_analysis_init(s_analysis, AUDIO_I2S_SAMPLE_RATE);
    ESP_LOGI(TAG, "FFT %s (esp-dsp deviation %.1e), %u B in %s, internal free %u B",
             s_analysis->use_dsp ? "esp-dsp" : "own", (double)s_analysis->dsp_error,
             (unsigned)(sizeof(*s_analysis) + WLED_ANALYSIS_FFT_SIZE * sizeof(int16_t)),
             (esp_ptr_external_ram(s_analysis) || esp_ptr_external_ram(s_window)) ? "PSRAM"
                                                                                  : "internal RAM",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (xTaskCreatePinnedToCoreWithCaps(sync_task, "wled_sync", TASK_STACK, NULL, TASK_PRIORITY,
                                        &s_task, s_server_role ? 0 : 1,
                                        TASK_STACK_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the analysis task");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}


/* ------------------------------------------------------------------ */
/* Finding WLEDs                                                      */
/* ------------------------------------------------------------------ */

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

/* Asks http://ip/json/info whether this is a WLED; its name on success. */
static bool probe_wled(uint32_t ip, char *name, size_t name_len)
{
    const esp_ip4_addr_t addr = {.addr = ip};
    char url[40];
    snprintf(url, sizeof(url), "http://" IPSTR "/json/info", IP2STR(&addr));

    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = PROBE_TIMEOUT_MS,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return false;
    }

    bool wled = false;
    char *body = NULL;
    if (esp_http_client_open(client, 0) == ESP_OK &&
        esp_http_client_fetch_headers(client) >= 0 &&
        esp_http_client_get_status_code(client) == 200) {
        body = heap_caps_malloc(PROBE_BODY_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (body != NULL) {
            int total = 0;
            int got;
            while (total < PROBE_BODY_MAX &&
                   (got = esp_http_client_read(client, body + total, PROBE_BODY_MAX - total)) > 0) {
                total += got;
            }
            body[total] = '\0';

            cJSON *root = cJSON_Parse(body);
            const cJSON *brand = cJSON_GetObjectItemCaseSensitive(root, "brand");
            if (cJSON_IsString(brand) && strcmp(brand->valuestring, "WLED") == 0) {
                const cJSON *wled_name = cJSON_GetObjectItemCaseSensitive(root, "name");
                strlcpy(name, cJSON_IsString(wled_name) ? wled_name->valuestring : "WLED", name_len);
                wled = true;
            }
            cJSON_Delete(root);
        }
    }
    heap_caps_free(body);
    esp_http_client_cleanup(client);
    return wled;
}

/* lwIP's NAPT tables belong to its own thread. */
typedef struct {
    struct tcpip_api_call_data call;
    bool add;
    uint32_t maddr;
    uint16_t mport;
    uint32_t daddr;
    uint16_t dport;
} forward_call_t;

static err_t forward_in_tcpip(struct tcpip_api_call_data *call)
{
    forward_call_t *f = (forward_call_t *)call;
    /* ip_portmap_add() would keep an old entry for the same port next to
     * the new one, so always clear it first. */
    (void)ip_portmap_remove(IP_PROTO_TCP, f->mport);
    if (f->add && !ip_portmap_add(IP_PROTO_TCP, f->maddr, f->mport, f->daddr, f->dport)) {
        return ERR_MEM;
    }
    return ERR_OK;
}

/* TCP to port mport on our mesh address goes to daddr:dport. */
static bool set_forward(bool add, uint32_t maddr, uint16_t mport, uint32_t daddr, uint16_t dport)
{
    forward_call_t f = {.add = add, .maddr = maddr, .mport = mport, .daddr = daddr, .dport = dport};
    return tcpip_api_call(forward_in_tcpip, &f.call) == ERR_OK;
}

static uint16_t wled_port(const uint8_t *mac)
{
    return (uint16_t)(PORT_BASE + ((((uint32_t)mac[4] << 8) | mac[5]) % PORT_SPAN));
}

/* Our address in the parent's network and the parent's on ours. */
static void mesh_addresses(uint32_t *mesh_ip, uint32_t *gateway)
{
    *mesh_ip = 0U;
    *gateway = 0U;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (sta != NULL && esp_netif_get_ip_info(sta, &info) == ESP_OK) {
        *mesh_ip = info.ip.addr;
        *gateway = info.gw.addr;
    }
}

/* The server's own address: its access point is the network every station
 * in the mesh routes to. */
static uint32_t ap_address(void)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t info;
    if (ap == NULL || esp_netif_get_ip_info(ap, &info) != ESP_OK) {
        return 0U;
    }
    return info.ip.addr;
}

/* 1 = the server, 2 = right below it; 0 = not known. */
static uint8_t mesh_level(void)
{
#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
    /* Mesh-Lite is not started on a provisioning boot, and asking it for
     * the level then dereferences NULL. */
    if (provisioning_get_active_reason() != PROVISIONING_REASON_NONE) {
        return 0U;
    }
    return esp_mesh_lite_get_level();
#else
    return 0U;
#endif
}

/* Asks the node above to forward `port` to us; the address the mesh then
 * reaches it at. */
static bool ask_parent(uint32_t gateway, uint16_t port, char *url, size_t url_len)
{
    const esp_ip4_addr_t gw = {.addr = gateway};
    char target[48];
    snprintf(target, sizeof(target), "http://" IPSTR "/api/wled-forward", IP2STR(&gw));
    char body[24];
    const int body_len = snprintf(body, sizeof(body), "{\"port\":%u}", (unsigned)port);

    const esp_http_client_config_t config = {
        .url = target,
        .method = HTTP_METHOD_POST,
        .timeout_ms = RELAY_TIMEOUT_MS,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");

    bool ok = false;
    if (esp_http_client_open(client, body_len) == ESP_OK &&
        esp_http_client_write(client, body, body_len) == body_len &&
        esp_http_client_fetch_headers(client) >= 0 &&
        esp_http_client_get_status_code(client) == 200) {
        char resp[128];
        int total = 0;
        int got;
        while (total < (int)sizeof(resp) - 1 &&
               (got = esp_http_client_read(client, resp + total, (int)sizeof(resp) - 1 - total)) > 0) {
            total += got;
        }
        resp[total] = '\0';
        cJSON *root = cJSON_Parse(resp);
        const cJSON *answer = cJSON_GetObjectItemCaseSensitive(root, "url");
        if (cJSON_IsString(answer) && strlen(answer->valuestring) < url_len) {
            strlcpy(url, answer->valuestring, url_len);
            ok = true;
        }
        cJSON_Delete(root);
    }
    esp_http_client_cleanup(client);
    return ok;
}

/*
 * Where the rest of the mesh reaches `port` on our mesh address: whatever
 * the node above answers when asked to pass the port on. The chain ends at
 * the server, which takes the connection itself (wled_proxy.c) -- its
 * access point does not carry traffic between two of its stations, so a
 * second-level address alone is not enough.
 */
static bool reach_from_mesh(uint16_t port, char *url, size_t url_len)
{
    uint32_t mesh_ip;
    uint32_t gateway;
    mesh_addresses(&mesh_ip, &gateway);
    if (mesh_ip == 0U || mesh_level() < 2U) {
        return false;
    }
    return ask_parent(gateway, port, url, url_len);
}

/* The server's link for a port it proxies. */
static void server_url(uint16_t port, char *url, size_t url_len)
{
    const esp_ip4_addr_t at = {.addr = ap_address()};
    snprintf(url, url_len, "http://" IPSTR ":%u/", IP2STR(&at), (unsigned)port);
}

/* Forgets a station, and its forward with it. Under s_lock. */
static void forget_station(station_t *s)
{
    if (s->forwarded) {
        (void)set_forward(false, 0U, s->port, 0U, 0U);
    }
    memset(s, 0, sizeof(*s));
}

/* Marks who is on our access point now; forgets who left. */
static void scan_stations(void)
{
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
        list.num = 0;
    }
    const int num = (list.num > MAX_STATIONS) ? MAX_STATIONS : list.num;
    esp_netif_pair_mac_ip_t pairs[MAX_STATIONS] = {0};
    for (int i = 0; i < num; ++i) {
        memcpy(pairs[i].mac, list.sta[i].mac, sizeof(pairs[i].mac));
    }
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap == NULL || num == 0 || esp_netif_dhcps_get_clients_by_mac(ap, num, pairs) != ESP_OK) {
        for (int i = 0; i < num; ++i) {
            pairs[i].ip.addr = 0U;
        }
    }

    LOCK();
    for (size_t k = 0; k < MAX_STATIONS; ++k) {
        s_stations[k].present = false;
    }
    for (int i = 0; i < num; ++i) {
        station_t *s = NULL;
        for (size_t k = 0; k < MAX_STATIONS && s == NULL; ++k) {
            if ((s_stations[k].probed_us != 0 || s_stations[k].ip != 0U) &&
                memcmp(s_stations[k].mac, pairs[i].mac, 6) == 0) {
                s = &s_stations[k];
            }
        }
        for (size_t k = 0; k < MAX_STATIONS && s == NULL; ++k) {
            if (s_stations[k].probed_us == 0 && s_stations[k].ip == 0U) {
                s = &s_stations[k];
                memset(s, 0, sizeof(*s));
                memcpy(s->mac, pairs[i].mac, 6);
            }
        }
        if (s == NULL) {
            continue;
        }
        s->present = true;
        if (s->ip != pairs[i].ip.addr) {
            /* New lease: ask again; the forward points at the old one. */
            uint8_t mac[6];
            memcpy(mac, s->mac, sizeof(mac));
            forget_station(s);
            memcpy(s->mac, mac, sizeof(mac));
            s->present = true;
            s->ip = pairs[i].ip.addr;
            s->seen_us = esp_timer_get_time();
        }
    }
    for (size_t k = 0; k < MAX_STATIONS; ++k) {
        station_t *s = &s_stations[k];
        if (!s->present && (s->probed_us != 0 || s->ip != 0U)) {
            if (s->is_wled) {
                ESP_LOGI(TAG, "WLED \"%s\" left", s->name);
            }
            forget_station(s);
        }
    }
    UNLOCK();
}

/* Asks each new station (and each non-WLED once a minute) whether it is a
 * WLED. The HTTP request runs outside the lock. */
static void probe_stations(void)
{
    for (size_t k = 0; k < MAX_STATIONS; ++k) {
        const int64_t now_us = esp_timer_get_time();
        LOCK();
        station_t copy = s_stations[k];
        const int64_t interval = (now_us - copy.seen_us < EARLY_US) ? REPROBE_EARLY_US : REPROBE_US;
        const bool due = copy.present && copy.ip != 0U && !copy.is_wled &&
                         (copy.probed_us == 0 || now_us - copy.probed_us >= interval);
        if (due) {
            s_stations[k].probed_us = now_us;
        }
        UNLOCK();
        if (!due) {
            continue;
        }

        char name[sizeof(copy.name)];
        if (!probe_wled(copy.ip, name, sizeof(name))) {
            continue;
        }
        LOCK();
        station_t *s = &s_stations[k];
        if (s->present && s->ip == copy.ip && memcmp(s->mac, copy.mac, 6) == 0) {
            const esp_ip4_addr_t addr = {.addr = s->ip};
            s->is_wled = true;
            s->port = wled_port(s->mac);
            strlcpy(s->name, name, sizeof(s->name));
            ESP_LOGI(TAG, "WLED \"%s\" found at " IPSTR " (%02x:%02x:%02x:%02x:%02x:%02x)",
                     s->name, IP2STR(&addr), s->mac[0], s->mac[1], s->mac[2], s->mac[3],
                     s->mac[4], s->mac[5]);
        }
        UNLOCK();
    }
}

/* Drops every forward when our mesh address changes, and relays nobody
 * renewed. Under s_lock. */
static void tidy_forwards(uint32_t mesh_ip, int64_t now_us)
{
    const bool moved = mesh_ip != s_mapped_on;
    for (size_t k = 0; k < MAX_STATIONS && moved; ++k) {
        station_t *s = &s_stations[k];
        if (s->forwarded) {
            (void)set_forward(false, 0U, s->port, 0U, 0U);
            s->forwarded = false;
        }
        s->url[0] = '\0';
        s->asked_us = 0;
    }
    for (size_t r = 0; r < MAX_RELAYS; ++r) {
        relay_t *relay = &s_relays[r];
        if (relay->port != 0U && (moved || now_us >= relay->until_us)) {
            (void)set_forward(false, 0U, relay->port, 0U, 0U);
            memset(relay, 0, sizeof(*relay));
        }
    }
    s_mapped_on = mesh_ip;
}

/*
 * Makes each WLED reachable from the whole mesh and notes the address. On
 * the server its own address is: the server's network is where every
 * station in the mesh can route to.
 */
static void update_forwards(void)
{
    const int64_t now_us = esp_timer_get_time();
    uint32_t mesh_ip = 0U;
    uint32_t gateway;
    if (!s_server_role) {
        mesh_addresses(&mesh_ip, &gateway);
    }

    LOCK();
    if (!s_server_role) {
        tidy_forwards(mesh_ip, now_us);
    }
    UNLOCK();

    for (size_t k = 0; k < MAX_STATIONS; ++k) {
        LOCK();
        station_t copy = s_stations[k];
        UNLOCK();
        if (!copy.present || !copy.is_wled) {
            continue;
        }

        char url[WLED_SYNC_URL_LEN] = "";
        bool forwarded = copy.forwarded;
        if (s_server_role) {
            /* Renewed on every pass, like a request from below. */
            wled_proxy_set(copy.port, copy.ip, 80);
            server_url(copy.port, url, sizeof(url));
        } else {
            if (mesh_ip == 0U) {
                continue;
            }
            if (!forwarded) {
                forwarded = set_forward(true, mesh_ip, copy.port, copy.ip, 80);
            }
            if (copy.url[0] != '\0' && now_us - copy.asked_us < RENEW_US) {
                strlcpy(url, copy.url, sizeof(url));
            } else if (!reach_from_mesh(copy.port, url, sizeof(url))) {
                url[0] = '\0';
            }
        }

        LOCK();
        station_t *s = &s_stations[k];
        if (s->present && memcmp(s->mac, copy.mac, 6) == 0 && s->ip == copy.ip) {
            s->forwarded = forwarded;
            if (strcmp(s->url, url) != 0) {
                if (url[0] != '\0') {
                    ESP_LOGI(TAG, "WLED \"%s\" reachable from the mesh at %s", s->name, url);
                } else {
                    ESP_LOGW(TAG, "WLED \"%s\": no address in the mesh yet", s->name);
                }
                strlcpy(s->url, url, sizeof(s->url));
            }
            if (copy.url[0] == '\0' || now_us - copy.asked_us >= RENEW_US) {
                s->asked_us = now_us;
            }
        }
        UNLOCK();
    }
}

/* Hands the WLED addresses to the sync task and switches the analysis on
 * or off with them. */
static void publish_targets(void)
{
    uint32_t targets[MAX_WLEDS] = {0};
    size_t count = 0;
    LOCK();
    for (size_t k = 0; k < MAX_STATIONS && count < MAX_WLEDS; ++k) {
        if (s_stations[k].present && s_stations[k].is_wled) {
            targets[count++] = s_stations[k].ip;
        }
    }
    UNLOCK();

    if (count > 0U && start_analysis() != ESP_OK) {
        count = 0U;
    }
    portENTER_CRITICAL(&s_targets_lock);
    memcpy(s_targets, targets, sizeof(s_targets));
    s_target_count = count;
    portEXIT_CRITICAL(&s_targets_lock);

    const bool enabled = count > 0U;
    if (enabled != s_enabled) {
        ESP_LOGI(TAG, "Analysis %s", enabled ? "on" : "off (no WLED left)");
        s_enabled = enabled;
    }
}

static void find_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FIND_PERIOD_MS));
        scan_stations();
        probe_stations();
        update_forwards();
        publish_targets();
    }
}

/* ------------------------------------------------------------------ */
/* Public                                                             */
/* ------------------------------------------------------------------ */

esp_err_t wled_sync_relay_forward(uint32_t child_ip, uint16_t port, char *url, size_t url_len)
{
    if (s_lock == NULL || child_ip == 0U || port < PORT_BASE || port >= PORT_BASE + PORT_SPAN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_server_role) {
        /* The end of the chain: the server takes the connection itself. */
        wled_proxy_set(port, child_ip, port);
        server_url(port, url, url_len);
        return ESP_OK;
    }

    uint32_t mesh_ip;
    uint32_t gateway;
    mesh_addresses(&mesh_ip, &gateway);
    if (mesh_ip == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t now_us = esp_timer_get_time();
    bool set = false;
    LOCK();
    relay_t *relay = NULL;
    for (size_t r = 0; r < MAX_RELAYS && relay == NULL; ++r) {
        if (s_relays[r].port == port) {
            relay = &s_relays[r];
        }
    }
    for (size_t r = 0; r < MAX_RELAYS && relay == NULL; ++r) {
        if (s_relays[r].port == 0U) {
            relay = &s_relays[r];
        }
    }
    if (relay != NULL) {
        set = relay->port != port || relay->to != child_ip || s_mapped_on != mesh_ip;
        relay->port = port;
        relay->to = child_ip;
        relay->until_us = now_us + RELAY_LAPSE_US;
    }
    UNLOCK();
    if (relay == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (set && !set_forward(true, mesh_ip, port, child_ip, port)) {
        return ESP_FAIL;
    }

    return reach_from_mesh(port, url, url_len) ? ESP_OK : ESP_FAIL;
}

size_t wled_sync_list(wled_sync_info_t *out, size_t max)
{
    if (s_lock == NULL) {
        return 0;
    }
    size_t count = 0;
    LOCK();
    for (size_t k = 0; k < MAX_STATIONS && count < max; ++k) {
        const station_t *s = &s_stations[k];
        if (s->present && s->is_wled) {
            strlcpy(out[count].name, s->name, sizeof(out[count].name));
            strlcpy(out[count].url, s->url, sizeof(out[count].url));
            ++count;
        }
    }
    UNLOCK();
    return count;
}

esp_err_t wled_sync_start(bool server_role)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_server_role = server_role;
    /* Asking a phone for /json/info ends in a refused connection, which the
     * HTTP client logs as an error every time. Nothing else here uses it. */
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);
    esp_log_level_set("esp-tls", ESP_LOG_NONE);
    esp_log_level_set("transport_base", ESP_LOG_NONE);
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL ||
        xTaskCreatePinnedToCoreWithCaps(find_task, "wled_find", FIND_TASK_STACK, NULL,
                                        FIND_TASK_PRIORITY, NULL, tskNO_AFFINITY,
                                        TASK_STACK_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "Could not start looking for WLEDs");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
