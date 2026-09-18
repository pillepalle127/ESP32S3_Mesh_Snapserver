/**
 * @file mesh_client.c
 * @brief Joins the mesh as a non-root relay and starts the Snapcast client
 *        once a parent hands out an IP address.
 */
#include "mesh_client.h"

#include <string.h>

#include "device_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "provisioning.h"
#include "snapclient.h"
#include "snapserver.h"

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

static bool s_snapclient_started;

/*
 * Resolves which Snapserver to connect to: an explicit override from
 * device_config.server_host if set, otherwise ESP-Mesh-Lite's own root-IP
 * tracking, which stays correct across parent changes in a dynamic,
 * multi-hop mesh (unlike the local DHCP gateway, which is only the
 * immediate parent beyond the first hop, and unlike mDNS, whose link-local
 * multicast doesn't cross the NAPT boundary between levels).
 */
static void resolve_server_host(char *out, size_t out_len)
{
    device_config_t cfg;
    device_config_get(&cfg);

    if (cfg.server_host[0] != '\0') {
        strlcpy(out, cfg.server_host, out_len);
        return;
    }

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
