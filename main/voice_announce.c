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
#include "audio_opus.h"
#include "audio_sink.h"
#include "device_config.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "snapserver.h"
#include "status_led.h"

static const char *TAG = "VOICE";

/* See TASK_STACK_CAPS in snapserver.c -- same reasoning, duplicated here
 * rather than shared: task stacks in PSRAM, only the TCB stays internal. */
#define TASK_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/*
 * The server task pushes the announcement mute (snapserver_refresh_
 * announcement() runs the whole ServerSettings send path, log lines
 * included). Both stacks are PSRAM, so being generous is free.
 */
#define VOICE_SERVER_TASK_STACK 10240
#define VOICE_CLIENT_TASK_STACK 10240
/* Same tier as server_task/CLIENT_TASK/SENDER_TASK (5), strictly below
 * audio_task's 6 -- existing priorities are deliberately left untouched,
 * see TODO.md on the reverted priority experiment. */
#define VOICE_TASK_PRIORITY        5

/*
 * voice_server_task only receives, filters and relays, and hands each
 * packet to the local speaker's playout (audio_i2s_push_voice()). It must
 * never wait: when it shared a loop with the decoder, every slow decode held
 * back the next relay, and the level-1 clients ran dry 15 % of the time
 * (2026-09-19). Core 0, next to the Wi-Fi and lwIP it feeds.
 *
 * Nobody here decodes any more. Both roles decode in the audio path, one
 * packet when its frame is due (voice_playout.h), on core 1: the server in
 * its capture loop, where the music encoder runs at complexity 0 during an
 * announcement, a client in its player task. That once broke the client's
 * 20 ms tick (2026-09-20) -- but then packets were decoded in bursts as they
 * arrived, in floating point at 2.8 ms each; now it is one packet per tick
 * at about 1.2 ms.
 */
#define VOICE_SERVER_TASK_CORE     0

/* The client's receive and relay task, on core 0 like the server's. */
#define VOICE_CLIENT_TASK_CORE     0

/*
 * Wire format.
 *
 * Phone -> server: a 4-byte little-endian sequence number, then one Opus
 * packet as the phone's encoder cut it (normally 20 ms of 16 kHz mono
 * speech).
 *
 * Server -> clients: the same, plus one byte between the sequence number
 * and the payload that says how many further hops the packet may take. The
 * server sets VOICE_RELAY_HOPS; a client with children forwards a copy with
 * one less while the byte is not zero, so a level-2 client hears the
 * announcement too. An empty payload is the server's silence primer, see
 * voice_server_task.
 *
 * Opus rather than raw PCM, which this first used: at 768 kbit/s per
 * level-1 client an announcement needed about nine times the music's
 * airtime, and on device that congested the shared channel -- the music
 * streams collapsed and recovered in bursts, internal heap fell to 14 kB,
 * and the queues it built were the latency one could hear. Opus at voice
 * bitrates is some 25 times smaller for ~10 ms more delay.
 */
#define VOICE_HEADER_BYTES           4U   /* phone -> server              */
#define VOICE_MESH_HEADER_BYTES      5U   /* server -> clients, with hops */
#define VOICE_MAX_PAYLOAD         1275U   /* largest legal Opus packet    */
#define VOICE_MAX_PACKET (VOICE_MESH_HEADER_BYTES + VOICE_MAX_PAYLOAD)

/*
 * How many hops past the root an announcement may travel. 1 means the
 * server, its direct clients and their children.
 *
 * Why not further: every hop inherits the tail risk of a mesh that is
 * rearranging -- a relay can stall for seconds while it rejoins, which
 * music rides out on its buffer and an announcement cannot. Why not zero,
 * as this started: clients a few metres from the server routinely attach to
 * each other rather than to it, because a neighbour two metres away is the
 * stronger signal (seen on device: -9 dBm to the neighbour against -64 dBm
 * to the server). Those nodes were silent although they stood in the same
 * room.
 */
#define VOICE_RELAY_HOPS             1U

/* Diagnostics: echo back one in this many packets, see voice_server_task. */
#define VOICE_ECHO_EVERY            50U

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
#define VOICE_SESSION_GAP_US   (1000LL * 1000LL)

/* Client side: how often an idle receiver wakes to see whether an
 * announcement has ended and its statistics are due. */
#define VOICE_CLIENT_IDLE_LOG_MS  500

/* ------------------------------------------------------------------ */
/* Arrival timing, shared by both roles                               */
/* ------------------------------------------------------------------ */

/*
 * How evenly the packets of one announcement arrived, logged at its end:
 * the phone's 20 ms packets at the server, the server's at each client.
 * Gaps say how long the playout had to bridge; "bunched" packets (under
 * 5 ms after the one before) say how much arrived at once afterwards.
 * Together they show which leg delivers in bursts.
 */
#define ARRIVAL_BUNCH_US 5000LL

typedef struct {
    uint32_t packets;
    uint32_t lost;          /* sequence numbers that never came */
    uint32_t gaps_40;       /* intervals over 40, 60, 100 ms */
    uint32_t gaps_60;
    uint32_t gaps_100;
    int64_t  max_gap_us;
    uint32_t bunched;
    uint32_t run;           /* current run of bunched packets */
    uint32_t longest_run;
    int64_t  last_us;
} arrival_stats_t;

static void arrival_note(arrival_stats_t *a, int64_t now_us, uint32_t lost)
{
    if (a->packets > 0U) {
        const int64_t gap = now_us - a->last_us;
        if (gap > a->max_gap_us) {
            a->max_gap_us = gap;
        }
        a->gaps_40 += gap > 40000LL;
        a->gaps_60 += gap > 60000LL;
        a->gaps_100 += gap > 100000LL;
        if (gap < ARRIVAL_BUNCH_US) {
            ++a->bunched;
            if (++a->run > a->longest_run) {
                a->longest_run = a->run;
            }
        } else {
            a->run = 0;
        }
    }
    ++a->packets;
    a->lost += lost;
    a->last_us = now_us;
}

/* Logs and clears. */
static void arrival_log(arrival_stats_t *a, const char *leg)
{
    if (a->packets > 1U) {
        ESP_LOGI(TAG,
                 "%s arrival: packets=%lu lost=%lu gaps>40ms=%lu >60ms=%lu >100ms=%lu "
                 "max=%lld ms bunched=%lu longest bunch=%lu",
                 leg, (unsigned long)a->packets, (unsigned long)a->lost,
                 (unsigned long)a->gaps_40, (unsigned long)a->gaps_60,
                 (unsigned long)a->gaps_100, a->max_gap_us / 1000LL,
                 (unsigned long)a->bunched, (unsigned long)a->longest_run);
    }
    memset(a, 0, sizeof(*a));
}

/* ------------------------------------------------------------------ */
/* Server: control state, relay, watchdogs                            */
/* ------------------------------------------------------------------ */

static int s_server_fd = -1;
static TaskHandle_t s_server_task;

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

/*
 * The music encoder and the announcement decoder share core 1, and during an
 * announcement the encoder alone took 78 % of it -- the decoder then missed
 * 15 % of the packets and this device's own speaker sounded broken
 * (2026-09-20). Nobody hears the music while an announcement runs: it is
 * encoded only to keep the clients' timeline intact, and they discard or
 * mute it. So it is encoded as cheaply as Opus can for the duration.
 */
#define VOICE_MUSIC_COMPLEXITY 0

static void lower_music_complexity(void)
{
    (void)audio_opus_set_complexity(VOICE_MUSIC_COMPLEXITY);
}

static void restore_music_complexity(void)
{
    device_config_t cfg;
    device_config_get(&cfg);
    (void)audio_opus_set_complexity((int32_t)cfg.opus_complexity);
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
    restore_music_complexity();
    audio_i2s_set_voice_active(false);
    snapserver_set_announcement(false);
    status_led_set_activity(STATUS_LED_ACTIVITY_NONE);
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
    lower_music_complexity();
    audio_i2s_set_voice_active(true);
    snapserver_set_announcement(true);
    status_led_set_activity(STATUS_LED_ACTIVITY_VOICE);
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
static void relay_to_level1(const uint8_t *payload, size_t payload_len,
                            uint32_t *out_sequence)
{
    if (payload_len > VOICE_MAX_PAYLOAD) {
        return;
    }

    /* Static, not on the stack: one caller, and the stack is in PSRAM. */
    static uint8_t packet[VOICE_MAX_PACKET];
    const uint32_t sequence = (*out_sequence)++;
    memcpy(packet, &sequence, sizeof(sequence));
    packet[VOICE_HEADER_BYTES] = (uint8_t)VOICE_RELAY_HOPS;
    if (payload_len > 0U) {
        memcpy(packet + VOICE_MESH_HEADER_BYTES, payload, payload_len);
    }
    const size_t len = VOICE_MESH_HEADER_BYTES + payload_len;

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

static void voice_server_task(void *arg)
{
    (void)arg;
    uint8_t buf[VOICE_MAX_PACKET + 32];

    uint32_t seen_session = 0;
    bool was_active = false;
    bool have_audio = false;
    bool have_phone_sequence = false;
    uint32_t phone_sequence = 0;
    uint32_t out_sequence = 0;
    int64_t last_audio_us = 0;
    int64_t last_primer_us = 0;
    int64_t last_refresh_us = 0;
    arrival_stats_t arrival = {0};

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
            }
            was_active = active;
            const size_t targets = refresh_level1_ips();
            snapserver_refresh_announcement();
            last_refresh_us = now;
            if (active) {
                ESP_LOGI(TAG, "Announcement: %u level-1 target(s)", (unsigned)targets);
            } else {
                arrival_log(&arrival, "Phone -> server");
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
                arrival_note(&arrival, now, lost);
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

        /*
         * Every VOICE_ECHO_EVERY-th packet goes straight back to the phone,
         * which measures the round trip from it. That closes the last gap
         * in the announcement's latency budget: capture and encoding are
         * measured in the app, queueing and output here, but the Wi-Fi leg
         * was the one part nobody could see. Four bytes, so the phone can
         * tell an echo from anything else, and rare enough to cost nothing.
         */
        if (got_audio && (phone_sequence % VOICE_ECHO_EVERY) == 0U) {
            uint8_t echo[VOICE_HEADER_BYTES];
            memcpy(echo, &phone_sequence, sizeof(phone_sequence));
            (void)sendto(s_server_fd, echo, sizeof(echo), 0,
                         (struct sockaddr *)&from, sizeof(from));
        }

        if (got_audio) {
            /* The clients decode for themselves -- the Opus packet goes out
             * untouched, only the header is the server's own. */
            const size_t payload_len = (size_t)n - VOICE_HEADER_BYTES;
            relay_to_level1(buf + VOICE_HEADER_BYTES, payload_len, &out_sequence);

            /* This device's own speaker: decoded when its frame is due. */
            audio_i2s_push_voice(phone_sequence, buf + VOICE_HEADER_BYTES, payload_len);
        } else if (!have_audio && now - last_primer_us >= VOICE_FRAME_US) {
            /*
             * Until the phone's microphone is up, level-1 clients get an
             * empty packet -- 10 ms of silence -- at the normal rate: it
             * switches them to the announcement immediately, so their music
             * stops together with everyone else's instead of running on for
             * however long the phone takes.
             */
            relay_to_level1(NULL, 0, &out_sequence);
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
    const esp_err_t init_err = audio_i2s_voice_init();
    if (init_err != ESP_OK) {
        return init_err;
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

/*
 * Children of this node, i.e. the stations on its own SoftAP. Announcements
 * are relayed to them while the packet's hop byte allows it, which is what
 * gets a level-2 client -- invisible to the server behind its parent's NAPT
 * -- to hear an announcement at all.
 */
#define VOICE_MAX_CHILDREN ESP_WIFI_MAX_CONN_NUM

static char s_child_ips[VOICE_MAX_CHILDREN][16];
static size_t s_child_count;
static int64_t s_children_refreshed_us;

/*
 * Resolves the children's addresses: the AP's station list gives the MACs,
 * the DHCP server the address it handed each of them. Both are cheap, but
 * not free, so this runs once per announcement and then about once a
 * second, same cadence as the server's level-1 refresh.
 */
static void refresh_children(void)
{
    s_child_count = 0;

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL) {
        return;
    }

    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK || sta_list.num == 0) {
        return;
    }

    esp_netif_pair_mac_ip_t pairs[VOICE_MAX_CHILDREN] = {0};
    const int num = (sta_list.num > VOICE_MAX_CHILDREN) ? VOICE_MAX_CHILDREN : sta_list.num;
    for (int i = 0; i < num; ++i) {
        memcpy(pairs[i].mac, sta_list.sta[i].mac, sizeof(pairs[i].mac));
    }
    if (esp_netif_dhcps_get_clients_by_mac(ap_netif, num, pairs) != ESP_OK) {
        return;
    }

    for (int i = 0; i < num; ++i) {
        if (pairs[i].ip.addr == 0) {
            continue; /* associated but no lease yet */
        }
        esp_ip4addr_ntoa(&pairs[i].ip, s_child_ips[s_child_count],
                         (int)sizeof(s_child_ips[s_child_count]));
        ++s_child_count;
    }
}

/* Forwards one packet, with its hop count already decremented, to every
 * child. Called before decoding: a slow decode must not delay the branch
 * below us, the same lesson the server learned the hard way. */
static void relay_to_children(uint8_t *packet, size_t len, uint8_t hops_left)
{
    packet[VOICE_HEADER_BYTES] = hops_left;

    for (size_t i = 0; i < s_child_count; ++i) {
        struct sockaddr_in dst = {0};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(VOICE_ANNOUNCE_PORT);
        if (inet_pton(AF_INET, s_child_ips[i], &dst.sin_addr) != 1) {
            continue;
        }
        (void)sendto(s_client_fd, packet, len, 0,
                     (struct sockaddr *)&dst, sizeof(dst));
    }
}

static void voice_client_task(void *arg)
{
    (void)arg;

    uint8_t buf[VOICE_MAX_PACKET + 32];
    uint32_t last_sequence = 0;
    bool have_sequence = false;
    int64_t last_packet_us = 0;
    arrival_stats_t arrival = {0};

    for (;;) {
        const int n = recvfrom(s_client_fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n < 0) {
            /* Receive timeout (VOICE_CLIENT_IDLE_LOG_MS): once an
             * announcement is over, log it now rather than at the start of
             * the next one. */
            if (arrival.packets > 0U && esp_timer_get_time() - last_packet_us > VOICE_SESSION_GAP_US) {
                arrival_log(&arrival, "Server -> client");
            }
            continue;
        }
        if (n < (int)VOICE_MESH_HEADER_BYTES || n > (int)VOICE_MAX_PACKET) {
            continue;
        }

        uint32_t sequence;
        memcpy(&sequence, buf, sizeof(sequence));

        const int64_t now = esp_timer_get_time();
        if (now - last_packet_us > VOICE_SESSION_GAP_US) {
            /* New session, see VOICE_SESSION_GAP_US. */
            have_sequence = false;
            arrival_log(&arrival, "Previous announcement: server -> client");
            audio_sink_reset_voice();
            refresh_children();
            s_children_refreshed_us = now;
            if (s_child_count != 0U) {
                ESP_LOGI(TAG, "Announcement: relaying to %u child node(s)",
                         (unsigned)s_child_count);
            }
        } else if (now - s_children_refreshed_us > VOICE_REFRESH_US) {
            refresh_children();
            s_children_refreshed_us = now;
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

        /* Pass it on first, decode afterwards. */
        const uint8_t hops_left = buf[VOICE_HEADER_BYTES];
        if (hops_left > 0U && s_child_count != 0U) {
            relay_to_children(buf, (size_t)n, (uint8_t)(hops_left - 1U));
        }

        /* Played when its frame is due (voice_playout.h); an empty payload
         * is the server's 10 ms silence primer. */
        const size_t payload_len = (size_t)n - VOICE_MESH_HEADER_BYTES;
        if (payload_len > 0U) {
            arrival_note(&arrival, now, lost);
        }
        audio_sink_push_voice(sequence, buf + VOICE_MESH_HEADER_BYTES, payload_len);
    }
}

esp_err_t voice_receive_start(void)
{
    if (s_client_fd >= 0) {
        return ESP_OK; /* idempotent */
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

    /* The same socket forwards to this node's children, see
     * relay_to_children(): a stuck child must not hold up the others. */
    const struct timeval client_snd_timeout = { .tv_sec = 0, .tv_usec = VOICE_SEND_TIMEOUT_US };
    (void)setsockopt(s_client_fd, SOL_SOCKET, SO_SNDTIMEO,
                     &client_snd_timeout, sizeof(client_snd_timeout));
    const struct timeval client_rcv_timeout = { .tv_sec = 0,
                                                .tv_usec = VOICE_CLIENT_IDLE_LOG_MS * 1000 };
    (void)setsockopt(s_client_fd, SOL_SOCKET, SO_RCVTIMEO,
                     &client_rcv_timeout, sizeof(client_rcv_timeout));

    if (xTaskCreatePinnedToCoreWithCaps(voice_client_task,
                                        "voice_client",
                                        VOICE_CLIENT_TASK_STACK,
                                        NULL,
                                        VOICE_TASK_PRIORITY,
                                        &s_client_task,
                                        VOICE_CLIENT_TASK_CORE,
                                        TASK_STACK_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "Could not create voice_client_task");
        close(s_client_fd);
        s_client_fd = -1;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Voice announcement receiver listening on UDP:%d (Opus)", VOICE_ANNOUNCE_PORT);
    return ESP_OK;
}
