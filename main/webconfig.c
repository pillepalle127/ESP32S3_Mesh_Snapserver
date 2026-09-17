/**
 * @file webconfig.c
 * @brief Config web UI: HTTP server, JSON API, embedded page, mDNS.
 */
#include "webconfig.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "audio_i2s.h"
#include "audio_opus.h"
#include "audio_sink.h"
#include "cJSON.h"
#include "device_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mdns.h"
#include "provisioning.h"

static const char *TAG = "WEBCONFIG";

#define WEBCONFIG_MAX_BODY 1024
#define WEBCONFIG_REBOOT_DELAY_US (1500LL * 1000LL)

extern const uint8_t webconfig_page_html_start[] asm("_binary_webconfig_page_html_start");
extern const uint8_t webconfig_page_html_end[] asm("_binary_webconfig_page_html_end");

static httpd_handle_t s_server;
static esp_timer_handle_t s_reboot_timer;

/* ------------------------------------------------------------------ */
/* Deferred reboot                                                    */
/* ------------------------------------------------------------------ */

static void reboot_timer_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void schedule_reboot(void)
{
    /*
     * A reboot is coming either way, so any pending provisioning-AP timeout
     * would just be redundant noise in the log if it fired first -- cancel
     * it defensively rather than let both race.
     */
    provisioning_cancel_ap_timeout();

    /*
     * s_reboot_timer is created once in webconfig_start(), where a failure
     * can be logged properly. If that creation failed, the handle stays
     * NULL forever -- falling back to an immediate esp_restart() here means
     * the in-flight HTTP response gets cut off, but that beats the UI
     * claiming "rebooting..." while nothing happens.
     */
    if (s_reboot_timer == NULL) {
        ESP_LOGW(TAG, "No reboot timer available, restarting immediately");
        esp_restart();
        return;
    }
    esp_timer_start_once(s_reboot_timer, WEBCONFIG_REBOOT_DELAY_US);
}

/* ------------------------------------------------------------------ */
/* JSON helpers                                                       */
/* ------------------------------------------------------------------ */

static void parse_bool_field(const cJSON *root, const char *key, bool *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
    }
}

static bool parse_number_field(const cJSON *root, const char *key, double *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        *out = item->valuedouble;
        return true;
    }
    return false;
}

/* Empty string means "leave unchanged" -- used for mesh_ssid, since an SSID
 * can never usefully be blank, so an accidental empty submit should not
 * wipe a saved one. mesh_password uses parse_password_field() instead: an
 * empty password is a valid, deliberate choice (an open network), so it
 * must overwrite like any other value -- see config_is_valid() in
 * device_config.c, which already accepts a zero-length password. */
static void parse_string_field(const cJSON *root, const char *key, char *out, size_t out_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring[0] != '\0') {
        strlcpy(out, item->valuestring, out_len);
    }
}

static void parse_password_field(const cJSON *root, const char *key, char *out, size_t out_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item)) {
        strlcpy(out, item->valuestring, out_len);
    }
}

/*
 * esp_http_server runs handlers on a single task by default, so a client
 * that stalls mid-body would otherwise wedge every other request behind an
 * unbounded retry loop. Give up after a handful of consecutive timeouts
 * instead of retrying forever.
 */
#define WEBCONFIG_MAX_RECV_TIMEOUTS 5

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (req->content_len <= 0 || (size_t)req->content_len >= buf_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received = 0;
    int consecutive_timeouts = 0;
    while (received < (size_t)req->content_len) {
        const int ret = httpd_req_recv(req, buf + received,
                                       (size_t)req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++consecutive_timeouts >= WEBCONFIG_MAX_RECV_TIMEOUTS) {
                    return ESP_FAIL;
                }
                continue;
            }
            return ESP_FAIL;
        }
        consecutive_timeouts = 0;
        received += (size_t)ret;
    }
    buf[received] = '\0';
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    const esp_err_t result = httpd_resp_sendstr(req, json);
    free(json);
    return result;
}

/* ------------------------------------------------------------------ */
/* Route handlers                                                     */
/* ------------------------------------------------------------------ */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)webconfig_page_html_start,
                           webconfig_page_html_end - webconfig_page_html_start);
}

static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    device_config_t cfg;
    device_config_get(&cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "mesh_enable", cfg.mesh_enable);
    cJSON_AddStringToObject(root, "mesh_ssid", cfg.mesh_ssid);
    cJSON_AddNumberToObject(root, "mesh_channel", cfg.mesh_channel);
    cJSON_AddNumberToObject(root, "mesh_max_level", cfg.mesh_max_level);
    cJSON_AddBoolToObject(root, "dsp_bypass", cfg.dsp_bypass);
    cJSON_AddNumberToObject(root, "crossover_hz", cfg.crossover_hz);
    cJSON_AddNumberToObject(root, "sub_gain_db", cfg.sub_gain_db);
    cJSON_AddNumberToObject(root, "wideband_gain_db", cfg.wideband_gain_db);
    cJSON_AddNumberToObject(root, "sub_channel", cfg.sub_channel);
    cJSON_AddNumberToObject(root, "wideband_channel", cfg.wideband_channel);
    cJSON_AddNumberToObject(root, "opus_bitrate", cfg.opus_bitrate);
    cJSON_AddNumberToObject(root, "opus_complexity", cfg.opus_complexity);

    cJSON_AddNumberToObject(root, "role", cfg.role);
    cJSON_AddNumberToObject(root, "buffer_ms", cfg.buffer_ms);
    cJSON_AddNumberToObject(root, "delay_trim_ms", cfg.delay_trim_ms);
    cJSON_AddNumberToObject(root, "source_mode", cfg.source_mode);
    cJSON_AddNumberToObject(root, "local_input_threshold_db", cfg.local_input_threshold_db);
    cJSON_AddStringToObject(root, "server_host", cfg.server_host);

    /*
     * Only echo the real password back while the password-protected mesh AP
     * is the active network: anyone reaching this over the open, unprotected
     * provisioning AP hasn't proven they know it, so they don't get to read
     * it back. Over the mesh AP, reaching this endpoint at all already
     * requires knowing the Wi-Fi password, so showing it back is no
     * additional exposure -- same trust model as an OS "show saved Wi-Fi
     * password" prompt.
     *
     * An empty password (a deliberately open network) isn't a secret --
     * it's visible to anyone doing a Wi-Fi scan -- so it's always echoed.
     * A length in [1,7] is the one case withheld: config_is_valid() now
     * rejects saving that range, but a blob written before that check
     * existed could still have one stored, and mesh_root.c falls back to
     * WIFI_AUTH_OPEN for it -- so without this a config predating the
     * validation could have this handler hand out a password for a network
     * that isn't actually password-protected.
     */
    const size_t password_len = strlen(cfg.mesh_password);
    if (provisioning_get_active_reason() == PROVISIONING_REASON_NONE &&
        (password_len == 0U || password_len >= 8U)) {
        cJSON_AddStringToObject(root, "mesh_password", cfg.mesh_password);
    }

    return send_json(req, root);
}

/*
 * Settings that only take effect after a reboot: the mesh identity (root
 * SoftAP config and what the client looks for in a parent) and the role
 * itself (server vs. client starts a completely different set of tasks in
 * app_main.c).
 */
static bool settings_changed_needing_reboot(const device_config_t *a, const device_config_t *b)
{
    return a->mesh_enable != b->mesh_enable ||
           strcmp(a->mesh_ssid, b->mesh_ssid) != 0 ||
           strcmp(a->mesh_password, b->mesh_password) != 0 ||
           a->mesh_channel != b->mesh_channel ||
           a->mesh_max_level != b->mesh_max_level ||
           a->role != b->role;
}

static void apply_live_params(const device_config_t *cfg)
{
    const audio_dsp_params_t dsp_params = {
        .bypass = cfg->dsp_bypass,
        .crossover_hz = (float)cfg->crossover_hz,
        .sub_gain_db = cfg->sub_gain_db,
        .wideband_gain_db = cfg->wideband_gain_db,
        .sub_channel = cfg->sub_channel,
        .wideband_channel = cfg->wideband_channel,
    };
    if (audio_i2s_set_dsp_params(&dsp_params) != ESP_OK) {
        ESP_LOGW(TAG, "Rejected DSP params from saved config");
    }
    audio_opus_set_bitrate((int32_t)cfg->opus_bitrate);
    audio_opus_set_complexity((int32_t)cfg->opus_complexity);

    /*
     * No-ops unless the client role's audio_sink is actually running (it
     * guards every setter on its own s_started flag), so it's safe to call
     * these unconditionally regardless of which role is currently active.
     */
    audio_sink_set_source_mode(cfg->source_mode);
    audio_sink_set_local_input_threshold_db(cfg->local_input_threshold_db);
    audio_sink_set_delay_trim_ms(cfg->delay_trim_ms);
}

static esp_err_t api_config_post_handler(httpd_req_t *req)
{
    char body[WEBCONFIG_MAX_BODY];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request body");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_OK;
    }

    device_config_t old_cfg;
    device_config_get(&old_cfg);
    device_config_t next = old_cfg;
    double num = 0.0;

    parse_bool_field(root, "mesh_enable", &next.mesh_enable);
    parse_string_field(root, "mesh_ssid", next.mesh_ssid, sizeof(next.mesh_ssid));
    parse_password_field(root, "mesh_password", next.mesh_password, sizeof(next.mesh_password));
    if (parse_number_field(root, "mesh_channel", &num)) next.mesh_channel = (uint8_t)num;
    if (parse_number_field(root, "mesh_max_level", &num)) next.mesh_max_level = (uint8_t)num;

    parse_bool_field(root, "dsp_bypass", &next.dsp_bypass);
    if (parse_number_field(root, "crossover_hz", &num)) next.crossover_hz = (uint16_t)num;
    if (parse_number_field(root, "sub_gain_db", &num)) next.sub_gain_db = (float)num;
    if (parse_number_field(root, "wideband_gain_db", &num)) next.wideband_gain_db = (float)num;
    if (parse_number_field(root, "sub_channel", &num)) next.sub_channel = (uint8_t)num;
    if (parse_number_field(root, "wideband_channel", &num)) next.wideband_channel = (uint8_t)num;

    if (parse_number_field(root, "opus_bitrate", &num)) next.opus_bitrate = (uint32_t)num;
    if (parse_number_field(root, "opus_complexity", &num)) next.opus_complexity = (uint8_t)num;

    if (parse_number_field(root, "role", &num)) next.role = (uint8_t)num;
    if (parse_number_field(root, "buffer_ms", &num)) next.buffer_ms = (uint16_t)num;
    if (parse_number_field(root, "delay_trim_ms", &num)) next.delay_trim_ms = (int16_t)num;
    if (parse_number_field(root, "source_mode", &num)) next.source_mode = (uint8_t)num;
    if (parse_number_field(root, "local_input_threshold_db", &num)) {
        next.local_input_threshold_db = (int8_t)num;
    }
    /* server_host uses the password-style always-overwrite parser: an empty
     * value is meaningful here too (auto-discover via Mesh-Lite), same as
     * an empty mesh_password means "open network". */
    parse_password_field(root, "server_host", next.server_host, sizeof(next.server_host));

    cJSON_Delete(root);

    if (device_config_save(&next) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "config rejected (out of range?)");
        return ESP_OK;
    }

    device_config_t saved;
    device_config_get(&saved);
    apply_live_params(&saved);

    /*
     * A save while the (open, timeout-limited) provisioning AP is active
     * always reboots, even for a DSP/Opus-only change: the provisioning AP
     * no longer reboots on its own timeout (it just shuts the radio off,
     * see provisioning.c), so a save handing back to the normal mesh-vs-
     * provisioning boot decision is the only way out of provisioning mode
     * short of a power cycle.
     */
    const bool needs_reboot = settings_changed_needing_reboot(&old_cfg, &saved) ||
        provisioning_get_active_reason() != PROVISIONING_REASON_NONE;

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "reboot", needs_reboot);
    const esp_err_t send_result = send_json(req, resp);

    if (needs_reboot) {
        ESP_LOGW(TAG, "Config saved, rebooting shortly");
        schedule_reboot();
    }

    return send_result;
}

static esp_err_t api_factory_reset_post_handler(httpd_req_t *req)
{
    /*
     * If a grace-window task is mid-wait, it would otherwise write
     * device_config back to NVS a few seconds/minutes after this erase,
     * silently undoing the reset. See device_config_factory_reset()'s own
     * s_erased latch for the belt-and-suspenders half of this fix.
     */
    provisioning_cancel_grace_window();
    device_config_factory_reset();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "reboot", true);
    const esp_err_t send_result = send_json(req, resp);

    ESP_LOGW(TAG, "Factory reset requested via config page, rebooting shortly");
    schedule_reboot();

    return send_result;
}

/*
 * Same MAC-suffix convention as the mesh/relay SSIDs (e.g. "SnapMesh_
 * E314E5"), so the value shown here (and used for the mDNS hostname below)
 * lets you match a browser tab back to a specific physical board instead of
 * every device showing/advertising the identical name.
 */
static void get_device_id_suffix(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, out_len, "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    static const char *const reason_names[] = {
        [PROVISIONING_REASON_NONE] = "none",
        [PROVISIONING_REASON_NO_CONFIG] = "no_config",
        [PROVISIONING_REASON_BOOT_FAIL_STREAK] = "boot_fail_streak",
        [PROVISIONING_REASON_MESH_DISABLED] = "mesh_disabled",
    };

    device_config_t cfg;
    device_config_get(&cfg);
    const provisioning_reason_t reason = provisioning_get_active_reason();

    char device_id[7];
    get_device_id_suffix(device_id, sizeof(device_id));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "reason", reason_names[reason]);
    cJSON_AddNumberToObject(root, "boot_fail_count", cfg.boot_fail_count);
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddStringToObject(root, "device_id", device_id);

    return send_json(req, root);
}

/* ------------------------------------------------------------------ */
/* Startup                                                            */
/* ------------------------------------------------------------------ */

esp_err_t webconfig_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEBCONFIG_PORT;
    config.stack_size = 8192;

    esp_err_t result = httpd_start(&s_server, &config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(result));
        return result;
    }

    const esp_timer_create_args_t reboot_timer_args = {
        .callback = &reboot_timer_cb,
        .name = "webcfg_reboot",
    };
    result = esp_timer_create(&reboot_timer_args, &s_reboot_timer);
    if (result != ESP_OK) {
        ESP_LOGW(TAG,
                 "Creating reboot timer failed: %s -- Save will restart "
                 "immediately instead of after the HTTP response flushes",
                 esp_err_to_name(result));
        s_reboot_timer = NULL;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_get_handler },
        { .uri = "/api/config", .method = HTTP_GET, .handler = api_config_get_handler },
        { .uri = "/api/config", .method = HTTP_POST, .handler = api_config_post_handler },
        { .uri = "/api/factory-reset", .method = HTTP_POST, .handler = api_factory_reset_post_handler },
        { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_get_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        result = httpd_register_uri_handler(s_server, &routes[i]);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Registering %s failed: %s", routes[i].uri, esp_err_to_name(result));
            return result;
        }
    }

    ESP_LOGI(TAG, "Config web server listening on port %d", WEBCONFIG_PORT);

    result = mdns_init();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(result));
        return ESP_OK;
    }

    /*
     * Every node used to advertise the identical "snapserver.local", which
     * is indistinguishable to mDNS/DNS caches once you've visited more than
     * one device -- a stale resolution silently keeps pointing at whichever
     * one you looked at first. A unique per-role, per-MAC hostname avoids
     * that collision entirely.
     */
    device_config_t cfg;
    device_config_get(&cfg);
    char device_id[7];
    get_device_id_suffix(device_id, sizeof(device_id));

    char hostname[24];
    snprintf(hostname, sizeof(hostname), "%s-%s",
             (cfg.role == DEVICE_ROLE_CLIENT) ? "snapclient" : "snapserver",
             device_id);

    mdns_hostname_set(hostname);
    mdns_instance_name_set((cfg.role == DEVICE_ROLE_CLIENT) ? "ESP32-S3 Snapclient" : "ESP32-S3 Snapserver");
    mdns_service_add(NULL, "_http", "_tcp", WEBCONFIG_PORT, NULL, 0);
    ESP_LOGI(TAG, "mDNS ready: http://%s.local/", hostname);

    return ESP_OK;
}
