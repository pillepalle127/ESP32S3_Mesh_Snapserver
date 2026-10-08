/**
 * @file mesh_client.c
 * @brief Joins the mesh as a non-root relay and starts the Snapcast client
 *        once a parent hands out an IP address.
 */
#include "mesh_client.h"

#include <string.h>

#include "audio_sink.h"
#include "device_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "provisioning.h"
#include "snapclient.h"
#include "snapserver.h"
#include "webconfig.h"

#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
#include "esp_bridge.h"
#include "esp_mesh_lite.h"
#include "esp_mesh_lite_core.h"
#endif

/* Seconds a silent station may stay associated; see
 * provisioning_set_ap_idle_timeout(). */
#define AP_IDLE_TIMEOUT_S 30

static const char *TAG = "MESH_CLIENT";

#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE

/* Root guard, see root_guard_cb(): checked once a second, acted on after
 * this many level-1 readings in a row, never before this uptime, and only
 * once the local input has been off this long. */
#define ROOT_GUARD_PERIOD_US      (1000LL * 1000LL)
#define ROOT_GUARD_CONFIRM_CHECKS 5U
#define ROOT_GUARD_MIN_UPTIME_US  (60LL * 1000LL * 1000LL)
#define ROOT_GUARD_LOCAL_QUIET_US (60LL * 1000LL * 1000LL)

static bool s_snapclient_started;
static esp_timer_handle_t s_root_guard_timer;
static uint8_t s_last_level;
static uint32_t s_root_checks;
/* Last check that found the local input playing; 0 = not since boot. */
static int64_t s_local_input_us;
/* What the guard last logged while held off, so each state logs once. */
static bool s_root_logged_local;
static bool s_root_logged_countdown;

/*
 * Resolves which Snapserver to connect to: the mesh root, from
 * ESP-Mesh-Lite's own root-IP tracking, which stays correct across parent
 * changes in a dynamic, multi-hop mesh (unlike the local DHCP gateway, which is only the
 * immediate parent beyond the first hop, and unlike mDNS, whose link-local
 * multicast doesn't cross the NAPT boundary between levels).
 */
static void resolve_server_host(char *out, size_t out_len)
{
    esp_ip_addr_t root_ip;
    if (esp_mesh_lite_get_root_ip(ESP_IPADDR_TYPE_V4, &root_ip) == ESP_OK) {
        /*
         * esp_mesh_lite_get_root_ip() (precompiled, no source available)
         * hands the address back with its bytes in the opposite order from
         * what esp_ip4addr_ntoa() expects -- confirmed on-device: a root at
         * 192.168.5.1 came out as "1.5.168.192", the exact byte-reversal.
         * Swap it back before formatting.
         */
        root_ip.u_addr.ip4.addr = __builtin_bswap32(root_ip.u_addr.ip4.addr);
        esp_ip4addr_ntoa(&root_ip.u_addr.ip4, out, (int)out_len);
        return;
    }

    ESP_LOGW(TAG, "Root IP unavailable, falling back to %s", PROVISIONING_AP_IP_ADDR);
    strlcpy(out, PROVISIONING_AP_IP_ADDR, out_len);
}

/*
 * Last resort when the Snapserver stays unreachable: drop the STA link so
 * Mesh-Lite rebuilds its parent choice from a fresh scan.
 *
 * The case this exists for is a mesh island. When the root goes away, the
 * remaining nodes still see each other's beacons and can attach to one
 * another; the result is associated, holds a DHCP lease and looks entirely
 * healthy from inside, but has no route to the server and no reason to
 * rescan. Observed on device: after a server restart only one of five
 * clients came back, and the others needed a power cycle.
 */
static void force_mesh_rejoin(void)
{
    ESP_LOGW(TAG, "Snapserver unreachable, dropping the parent link to rescan");
    snapclient_set_network_available(false);
    const esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not drop the parent link: %s", esp_err_to_name(err));
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    char host[IP4ADDR_STRLEN_MAX];
    resolve_server_host(host, sizeof(host));

    ESP_LOGI(TAG, "Got mesh IP, Snapserver resolved to %s:%u", host, (unsigned)SNAPSERVER_PORT);

    /* Every time, not just the first: the root's address can differ after a
     * server restart or a parent change, and the client used to keep the one
     * it saw at startup. */
    snapclient_set_server_host(host);
    snapclient_set_host_resolver(resolve_server_host);
    snapclient_set_unreachable_cb(force_mesh_rejoin);
    snapclient_set_config_handler(webconfig_handle_remote_request);
    snapclient_set_network_available(true);

    if (!s_snapclient_started) {
        s_snapclient_started = true;
        const esp_err_t result = snapclient_start(host, SNAPSERVER_PORT);
        if (result != ESP_OK) {
            s_snapclient_started = false;
            ESP_LOGE(TAG, "Starting snapclient failed: %s", esp_err_to_name(result));
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Lost parent, aborting any open Snapserver connection");
        snapclient_set_network_available(false);
    }
}

/*
 * A client must never be the mesh root: it would open a second mesh under
 * the same SSID, find itself as the Snapserver, and anything that attached
 * to it would stay silent. esp_mesh_lite_set_disallowed_level(1) is meant to
 * rule that out, and Mesh-Lite 1.0.2's User_Guide says self-healing cannot
 * promote a node that is not allowed on level 1 -- but the library is
 * precompiled, so nothing here can confirm that holds when the root fails.
 * Nothing else in this firmware looks at the level after the join, and
 * force_mesh_rejoin() only drops a parent link a root does not have.
 *
 * So: on level 1 for ROOT_GUARD_CONFIRM_CHECKS seconds, restart and join
 * again as a relay. Not esp_wifi_stop(): that would leave the speaker
 * silent until someone power-cycles it, even when the server is back a few
 * seconds later. Not before ROOT_GUARD_MIN_UPTIME_US either, so a device
 * that keeps ending up as root restarts at most once a minute. And not
 * while it plays its local I2S input: that needs no mesh, and a restart
 * would only cut it. The restart comes ROOT_GUARD_LOCAL_QUIET_US after the
 * local input stopped; if it starts again in between, that wait starts
 * over once it stops again.
 *
 * Every level change is logged, which also shows what a node reports while
 * it has no parent.
 */
static void root_guard_cb(void *arg)
{
    (void)arg;

    const int64_t now_us = esp_timer_get_time();
    /* On every check, root or not: the wait counts from when the local
     * input actually stopped, even if that was before this became root. */
    const bool local_playing = audio_sink_current_source() == AUDIO_SINK_SOURCE_LOCAL_INPUT;
    if (local_playing) {
        s_local_input_us = now_us;
    }

    const uint8_t level = esp_mesh_lite_get_level();
    if (level != s_last_level) {
        ESP_LOGI(TAG, "Mesh level %u -> %u", (unsigned)s_last_level, (unsigned)level);
        s_last_level = level;
    }

    if (level != 1U) {
        s_root_checks = 0;
        s_root_logged_local = false;
        s_root_logged_countdown = false;
        return;
    }
    ++s_root_checks;
    if (s_root_checks == 1U) {
        ESP_LOGE(TAG, "Client is mesh root (level 1); restarting unless that clears");
    }
    if (s_root_checks < ROOT_GUARD_CONFIRM_CHECKS || now_us < ROOT_GUARD_MIN_UPTIME_US) {
        return;
    }
    if (local_playing) {
        if (!s_root_logged_local) {
            ESP_LOGW(TAG, "Mesh root, but playing the local input: restart %u s after it stops",
                     (unsigned)(ROOT_GUARD_LOCAL_QUIET_US / 1000000LL));
            s_root_logged_local = true;
            s_root_logged_countdown = false;
        }
        return;
    }
    if (s_local_input_us != 0 && now_us - s_local_input_us < ROOT_GUARD_LOCAL_QUIET_US) {
        if (!s_root_logged_countdown) {
            ESP_LOGW(TAG, "Local input stopped: restarting in %u s unless it starts again",
                     (unsigned)((ROOT_GUARD_LOCAL_QUIET_US - (now_us - s_local_input_us)) / 1000000LL));
            s_root_logged_countdown = true;
            s_root_logged_local = false;
        }
        return;
    }
    ESP_LOGE(TAG, "Still mesh root after %lu s: restarting to rejoin as a relay",
             (unsigned long)s_root_checks);
    esp_restart();
}

static void start_root_guard(void)
{
    const esp_timer_create_args_t args = {
        .callback = &root_guard_cb,
        .name = "root_guard",
    };
    if (esp_timer_create(&args, &s_root_guard_timer) != ESP_OK ||
        esp_timer_start_periodic(s_root_guard_timer, ROOT_GUARD_PERIOD_US) != ESP_OK) {
        ESP_LOGW(TAG, "Root guard unavailable: a client that becomes root stays root");
    }
}

static esp_err_t start_client_mesh(const device_config_t *cfg)
{
    ESP_LOGI(TAG, "Joining mesh as non-root relay");

    esp_bridge_create_all_netif();

    esp_err_t err = provisioning_clear_sta_wifi();
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Same SSID/password as the root, not a per-device suffix: Mesh-Lite
     * identifies a valid parent through a vendor IE embedded in the beacon
     * (see the "[vendor_ie]" log lines), not by matching the SSID text, so a
     * shared name across every node doesn't affect mesh joining at all --
     * it just means every relay behaves like one seamless network name
     * instead of a growing list of per-device SSIDs to sort through.
     *
     * Channel 1 here is a placeholder: once this node's STA side associates
     * with a parent, esp_wifi keeps AP and STA on the same radio channel
     * automatically (single-radio concurrent AP+STA), so whatever the
     * parent actually uses wins regardless of what's configured up front.
     */
    err = provisioning_configure_ap_wifi(cfg->mesh_ssid, cfg->mesh_password, 1);
    if (err != ESP_OK) {
        return err;
    }

    esp_mesh_lite_config_t config = ESP_MESH_LITE_DEFAULT_INIT();
    config.join_mesh_ignore_router_status = true;
    config.join_mesh_without_configured_wifi = true;
    config.leaf_node = false;
    config.max_level = cfg->mesh_max_level;

    esp_mesh_lite_init(&config);

    err = esp_mesh_lite_set_softap_info(cfg->mesh_ssid, cfg->mesh_password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not set Mesh-Lite SoftAP info: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_mesh_lite_set_disallowed_level(1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not disallow root level: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Tuned for a tube-shaped, highly dynamic mesh with frequent
     * rearrangements: retry the current parent quickly a few times (2 s
     * apart, up to 3 tries), then fall back to a full rescan every 5 s
     * rather than the (slower) library defaults of 5 s / 2 tries / 10 s.
     */
    esp_mesh_lite_set_wifi_reconnect_interval(2, 3, 5);

    /*
     * Fusion is what merges a mesh that has split into separate islands, and
     * it runs every 600 s by default -- ten minutes during which nodes that
     * attached to each other after the root vanished keep a network that
     * goes nowhere. Every 20 s instead, starting 20 s after boot so the
     * initial join is not disturbed.
     */
    esp_mesh_lite_fusion_config_t fusion = {
        .fusion_rssi_threshold = -85,
        .fusion_start_time_sec = 20,
        .fusion_frequency_sec = 20,
    };
    if (esp_mesh_lite_set_fusion_config(&fusion) != ESP_OK) {
        ESP_LOGW(TAG, "Could not shorten the mesh fusion interval");
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                              &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Registering Wi-Fi event handler failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &ip_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Registering IP event handler failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Wi-Fi power save off. ESP-IDF defaults a connected STA to
     * WIFI_PS_MIN_MODEM with listen interval 3, i.e. the radio may sleep up
     * to ~307 ms between beacons. For a node receiving a continuous
     * 96 kbit/s stream that shows up as stalled TCP (the server's send
     * buffer fills, every chunk gets skipped) and as missed management
     * frames -- observed on device as a 25 s blackout ending in the AP
     * dropping the station after six unanswered SA Query attempts. These
     * are mains-powered speakers, so there is nothing to save here.
     */
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_mesh_lite_core_log_enable(false);
    esp_mesh_lite_connect();
    esp_mesh_lite_start();
    /* Only now: Mesh-Lite has to be running before it is asked for the
     * level (see webconfig.c). */
    start_root_guard();

    /* After the start: Mesh-Lite reconfigures the SoftAP, so this has to
     * undo its PMF setting rather than pre-empt it. See the header. */
    (void)provisioning_disable_ap_pmf();
    (void)provisioning_set_ap_idle_timeout(AP_IDLE_TIMEOUT_S);

    ESP_LOGI(TAG, "Mesh client started: relay SSID=%s (shared with root)", cfg->mesh_ssid);

    return provisioning_arm_grace_window(IP_EVENT, IP_EVENT_STA_GOT_IP);
}
#endif

esp_err_t mesh_client_start(void)
{
    device_config_t cfg;
    device_config_get(&cfg);
    provisioning_reason_t reason = provisioning_decide(&cfg);

#if !CONFIG_SNAPSERVER_ENABLE_MESH_LITE
    if (reason == PROVISIONING_REASON_NONE) {
        ESP_LOGW(TAG, "Mesh-Lite not compiled into this firmware, forcing provisioning AP");
        reason = PROVISIONING_REASON_MESH_DISABLED;
    }
#endif

    if (reason != PROVISIONING_REASON_NONE) {
        return provisioning_start_fallback_ap(reason);
    }

#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
    return start_client_mesh(&cfg);
#else
    return ESP_OK; /* unreachable: reason is forced above when unavailable */
#endif
}
