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
#include "client_store.h"
#include "device_config.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "pinmap.h"
#include "pots.h"
#include "provisioning.h"
#include "snapclient.h"
#include "snapserver.h"
#include "sdkconfig.h"
#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
#include "esp_mesh_lite.h"
#endif

static const char *TAG = "WEBCONFIG";

/* The full form with a 64-character password and the knob fields comes to
 * roughly 800 bytes; 1536 leaves room without growing the 8 KB handler
 * stack meaningfully. */
#define WEBCONFIG_MAX_BODY 1536
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

/*
 * The form's current values. Shared by GET /api/config and by the server's
 * device list, which asks a client for this over its Snapcast connection
 * (webconfig_handle_remote_request()).
 */
static cJSON *build_config_json(void)
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

    device_pots_t pots;
    device_config_get_pots(&pots);
#if CONFIG_SNAPSERVER_POTS_ENABLE
    cJSON_AddBoolToObject(root, "pots_supported", true);
#else
    cJSON_AddBoolToObject(root, "pots_supported", false);
#endif
    cJSON_AddNumberToObject(root, "pot_volume_gpio", pots.volume_gpio);
    cJSON_AddNumberToObject(root, "pot_delay_gpio", pots.delay_gpio);
    cJSON_AddNumberToObject(root, "pot_delay_range_ms", pots.delay_range_ms);
    cJSON_AddNumberToObject(root, "pot_delay_range_max_ms", DEVICE_CONFIG_DELAY_TRIM_MAX_MS);

    device_pins_t pins;
    device_config_get_pins(&pins);
    cJSON *pin_obj = cJSON_AddObjectToObject(root, "pins");
    cJSON_AddNumberToObject(pin_obj, "i2s_bclk", pins.i2s_bclk);
    cJSON_AddNumberToObject(pin_obj, "i2s_lrclk", pins.i2s_lrclk);
    cJSON_AddNumberToObject(pin_obj, "i2s_din", pins.i2s_din);
    cJSON_AddNumberToObject(pin_obj, "i2s_dout", pins.i2s_dout);
    cJSON_AddNumberToObject(pin_obj, "status_led", pins.status_led);
#if CONFIG_SNAPSERVER_STATUS_LED_ENABLE
    cJSON_AddBoolToObject(root, "status_led_supported", true);
#else
    cJSON_AddBoolToObject(root, "status_led_supported", false);
#endif

    /*
     * The hardware verdict for every GPIO the module has, so the page can
     * offer only usable pins and say why the others are missing. Which
     * function already holds a pin the page works out itself: the form
     * can move several at once, and only the complete set it submits can
     * be checked for clashes (device_config_pin_set_valid() does that
     * again on save). Knobs additionally need ADC1, see pots.c.
     */
    cJSON_AddNumberToObject(root, "gpio_max", PINMAP_GPIO_MAX);
    cJSON *blocked = cJSON_AddObjectToObject(root, "gpio_blocked");
    cJSON *adc = cJSON_AddArrayToObject(root, "adc1_pins");
    for (uint8_t gpio = 0U; gpio <= PINMAP_GPIO_MAX; ++gpio) {
        const char *reason = pinmap_blocked_reason(gpio);
        if (reason != NULL) {
            char key[4];
            snprintf(key, sizeof(key), "%u", (unsigned)gpio);
            cJSON_AddStringToObject(blocked, key, reason);
        }
        if (pinmap_is_adc1(gpio)) {
            cJSON_AddItemToArray(adc, cJSON_CreateNumber(gpio));
        }
    }

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

    return root;
}

static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    return send_json(req, build_config_json());
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
           a->role != b->role ||
           /* Both roles size a multi-second buffer from this at startup --
            * the client's playback ring, the server's local output delay
            * line -- so a change only takes effect on the next boot. */
           a->buffer_ms != b->buffer_ms;
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

    /*
     * The field only counts while no delay knob is running; otherwise the
     * knob owns the trim, and applying the saved number here would make
     * the speaker jump away from where the knob stands until it is next
     * moved.
     */
    if (!pots_delay_active()) {
        audio_sink_apply_delay_trim(cfg->role, cfg->buffer_ms, cfg->delay_trim_ms);
    }
}

/*
 * Stores and applies a submitted form. On failure *err says why and nothing
 * is stored. *reboot tells the caller to schedule_reboot() once its answer
 * is on its way.
 */
static esp_err_t apply_config_json(const cJSON *root, bool *reboot, const char **err)
{
    *reboot = false;
    *err = NULL;

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

    device_pots_t old_pots;
    device_config_get_pots(&old_pots);
    device_pots_t next_pots = old_pots;
    if (parse_number_field(root, "pot_volume_gpio", &num)) next_pots.volume_gpio = (uint8_t)num;
    if (parse_number_field(root, "pot_delay_gpio", &num)) next_pots.delay_gpio = (uint8_t)num;
    if (parse_number_field(root, "pot_delay_range_ms", &num)) {
        next_pots.delay_range_ms = (uint16_t)num;
    }

    device_pins_t old_pins;
    device_config_get_pins(&old_pins);
    device_pins_t next_pins = old_pins;
    const cJSON *pin_obj = cJSON_GetObjectItemCaseSensitive(root, "pins");
    if (cJSON_IsObject(pin_obj)) {
        if (parse_number_field(pin_obj, "i2s_bclk", &num)) next_pins.i2s_bclk = (uint8_t)num;
        if (parse_number_field(pin_obj, "i2s_lrclk", &num)) next_pins.i2s_lrclk = (uint8_t)num;
        if (parse_number_field(pin_obj, "i2s_din", &num)) next_pins.i2s_din = (uint8_t)num;
        if (parse_number_field(pin_obj, "i2s_dout", &num)) next_pins.i2s_dout = (uint8_t)num;
        if (parse_number_field(pin_obj, "status_led", &num)) next_pins.status_led = (uint8_t)num;
    }

    /*
     * Checked here, before anything is stored: a pin set the page would
     * never offer -- but the API is open -- must not get half-saved with
     * the rest of the form.
     */
    if (!device_config_pin_set_valid(&next_pins, &next_pots)) {
        *err = "pin assignment rejected (pin not usable or used twice?)";
        return ESP_ERR_INVALID_ARG;
    }

    if (device_config_save(&next) != ESP_OK) {
        *err = "config rejected (out of range?)";
        return ESP_ERR_INVALID_ARG;
    }
    if (device_config_save_pin_set(&next_pins, &next_pots) != ESP_OK) {
        *err = "storing pin assignment failed";
        return ESP_FAIL;
    }

    device_config_t saved;
    device_config_get(&saved);
    pots_set_delay_range(next_pots.delay_range_ms);
    apply_live_params(&saved);

    /* Pins are claimed once at start, so moving any of them needs a reboot. */
    const bool pins_changed = old_pots.volume_gpio != next_pots.volume_gpio ||
                              old_pots.delay_gpio != next_pots.delay_gpio ||
                              memcmp(&old_pins, &next_pins, sizeof(old_pins)) != 0;

    /*
     * A save while the (open, timeout-limited) provisioning AP is active
     * always reboots, even for a DSP/Opus-only change: the provisioning AP
     * no longer reboots on its own timeout (it just shuts the radio off,
     * see provisioning.c), so a save handing back to the normal mesh-vs-
     * provisioning boot decision is the only way out of provisioning mode
     * short of a power cycle.
     */
    *reboot = settings_changed_needing_reboot(&old_cfg, &saved) ||
        pins_changed ||
        provisioning_get_active_reason() != PROVISIONING_REASON_NONE;
    return ESP_OK;
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

    bool needs_reboot = false;
    const char *err = NULL;
    const esp_err_t result = apply_config_json(root, &needs_reboot, &err);
    cJSON_Delete(root);
    if (result != ESP_OK) {
        httpd_resp_send_err(req,
                            (result == ESP_ERR_INVALID_ARG) ? HTTPD_400_BAD_REQUEST
                                                            : HTTPD_500_INTERNAL_SERVER_ERROR,
                            err);
        return ESP_OK;
    }

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
    /* Volumes, delays and names given to clients go with it. */
    client_store_erase_all();

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

static cJSON *build_status_json(void)
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
    /* The release tag in a CI build, `git describe` otherwise. */
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);

    /* Live knob readings; null where no knob is running. */
    const int volume_percent = pots_volume_percent();
    if (volume_percent >= 0) {
        cJSON_AddNumberToObject(root, "pot_volume_percent", volume_percent);
    } else {
        cJSON_AddNullToObject(root, "pot_volume_percent");
    }
    int16_t delay_ms = 0;
    if (pots_delay_ms(&delay_ms)) {
        cJSON_AddNumberToObject(root, "pot_delay_ms", delay_ms);
    } else {
        cJSON_AddNullToObject(root, "pot_delay_ms");
    }
    cJSON_AddBoolToObject(root, "pot_delay_active", pots_delay_active());

    device_pins_t pins;
    device_config_get_pins(&pins);
    cJSON_AddBoolToObject(root, "pins_on_trial", pins.trial_boots != 0U);
    cJSON_AddBoolToObject(root, "pins_reverted", device_config_pins_reverted());

    /* Server role: how many speakers it feeds right now. */
    if (cfg.role == DEVICE_ROLE_SERVER) {
        size_t own = 0;
        const size_t total = snapserver_client_count(&own);
        cJSON *clients = cJSON_AddObjectToObject(root, "clients");
        cJSON_AddNumberToObject(clients, "total", (double)total);
        cJSON_AddNumberToObject(clients, "own", (double)own);
    }

    /* Client role: where its server is, so the page can link back to it.
     * The address is the one the client dials, which is reachable from
     * anywhere in the mesh -- unlike the client addresses the server lists,
     * which only work one hop down. */
    if (cfg.role == DEVICE_ROLE_CLIENT) {
        char server_host[64];
        const bool connected = snapclient_get_server(server_host, sizeof(server_host));
        if (server_host[0] != '\0') {
            cJSON_AddStringToObject(root, "server_host", server_host);
        } else {
            cJSON_AddNullToObject(root, "server_host");
        }
        cJSON_AddBoolToObject(root, "server_connected", connected);
#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
        /* Same count the server's device list shows: root = level 1. */
        const uint8_t level = esp_mesh_lite_get_level();
        if (level >= 2U) {
            cJSON_AddNumberToObject(root, "server_hops", level - 1U);
        } else {
            cJSON_AddNullToObject(root, "server_hops");
        }
#else
        cJSON_AddNullToObject(root, "server_hops");
#endif
        /* Stations on this node's own AP: the mesh nodes relaying through
         * it, plus any phone joined to it directly -- Mesh-Lite only counts
         * a node's children on the root, and only with node info reports. */
        wifi_sta_list_t stations;
        if (esp_wifi_ap_get_sta_list(&stations) == ESP_OK) {
            cJSON_AddNumberToObject(root, "children", stations.num);
        } else {
            cJSON_AddNullToObject(root, "children");
        }
    }

    return root;
}

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    return send_json(req, build_status_json());
}

/*
 * Client role: a request the server forwards from its device list over the
 * Snapcast connection (snapserver_remote_request()), because it cannot
 * reach this page itself once the client sits behind another node's NAPT.
 * Answers exactly what the matching route here would.
 */
char *webconfig_handle_remote_request(const char *json, size_t len)
{
    cJSON *request = cJSON_ParseWithLength(json, len);
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(request, "op");
    cJSON *resp = NULL;
    bool needs_reboot = false;

    if (!cJSON_IsString(op)) {
        resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "error", "bad request");
    } else if (strcmp(op->valuestring, "get_config") == 0) {
        resp = build_config_json();
    } else if (strcmp(op->valuestring, "status") == 0) {
        resp = build_status_json();
    } else if (strcmp(op->valuestring, "set_config") == 0) {
        const cJSON *config = cJSON_GetObjectItemCaseSensitive(request, "config");
        const char *err = "bad request";
        resp = cJSON_CreateObject();
        if (cJSON_IsObject(config) &&
            apply_config_json(config, &needs_reboot, &err) == ESP_OK) {
            cJSON_AddBoolToObject(resp, "reboot", needs_reboot);
        } else {
            cJSON_AddStringToObject(resp, "error", err);
        }
    } else {
        resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "error", "unknown op");
    }
    cJSON_Delete(request);

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    /* The timer leaves 1.5 s for the answer to get out first. */
    if (needs_reboot) {
        ESP_LOGW(TAG, "Config saved via the server, rebooting shortly");
        schedule_reboot();
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Device list (server role)                                          */
/* ------------------------------------------------------------------ */

/*
 * Client snapshot in PSRAM: SNAPSERVER_MAX_CLIENTS entries are ~4 kB,
 * half of this handler's stack. Same approach as snapcontrol.c.
 */
static size_t take_clients(snapserver_client_info_t **out)
{
    snapserver_client_info_t *buf =
        heap_caps_malloc(sizeof(*buf) * SNAPSERVER_MAX_CLIENTS, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        buf = malloc(sizeof(*buf) * SNAPSERVER_MAX_CLIENTS);
    }
    *out = buf;
    return (buf != NULL) ? snapserver_get_clients(buf, SNAPSERVER_MAX_CLIENTS) : 0U;
}

/*
 * Every speaker the server feeds, itself first. For a client, "delay_ms"
 * is its Snapcast latency with the sign turned round: latency makes a
 * client play earlier, and on this page delay means later, as it does for
 * the delay trim and the delay knob. Volume is the Snapcast volume, the
 * same one control apps set; a client's own volume knob multiplies with
 * it. The server's own row shows its knob and trim and is not editable
 * here -- those live in the config sections below.
 */
static esp_err_t api_devices_get_handler(httpd_req_t *req)
{
    device_config_t cfg;
    device_config_get(&cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "delay_max_ms", DEVICE_CONFIG_DELAY_TRIM_MAX_MS);
    if (cfg.role != DEVICE_ROLE_SERVER) {
        /* Clients know nothing about their siblings. */
        cJSON_AddNullToObject(root, "clients");
        return send_json(req, root);
    }

    char device_id[7];
    get_device_id_suffix(device_id, sizeof(device_id));
    cJSON *server = cJSON_AddObjectToObject(root, "server");
    cJSON_AddStringToObject(server, "id", device_id);
    cJSON_AddNumberToObject(server, "hops", 0);
    const int volume_percent = pots_volume_percent();
    if (volume_percent >= 0) {
        cJSON_AddNumberToObject(server, "volume_percent", volume_percent);
    } else {
        cJSON_AddNullToObject(server, "volume_percent");
    }
    int16_t knob_delay_ms = 0;
    cJSON_AddNumberToObject(server, "delay_ms",
                            pots_delay_ms(&knob_delay_ms) ? knob_delay_ms : cfg.delay_trim_ms);

    cJSON *list = cJSON_AddArrayToObject(root, "clients");
    snapserver_client_info_t *clients = NULL;
    const size_t count = take_clients(&clients);
    for (size_t i = 0; i < count; ++i) {
        const snapserver_client_info_t *c = &clients[i];
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "id", c->id);
        cJSON_AddStringToObject(entry, "name", c->name);
        cJSON_AddStringToObject(entry, "mac", c->mac);
        if (c->hops >= 0) {
            cJSON_AddNumberToObject(entry, "hops", c->hops);
        } else {
            cJSON_AddNullToObject(entry, "hops");
        }
        cJSON_AddBoolToObject(entry, "own", c->is_snapmesh);
        cJSON_AddNumberToObject(entry, "volume_percent", c->volume_percent);
        cJSON_AddBoolToObject(entry, "muted", c->muted);
        cJSON_AddNumberToObject(entry, "delay_ms", -c->latency_ms);
        cJSON_AddItemToArray(list, entry);
    }
    free(clients);

    return send_json(req, root);
}

/*
 * {"id": ..., and any of "volume_percent", "muted", "delay_ms", "name"}.
 * Fields left out keep their value. Applied at once and stored for the
 * client's next connect, see snapserver_set_client_volume().
 */
static esp_err_t api_devices_post_handler(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request body");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_OK;
    }

    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    char id[64] = "";
    if (cJSON_IsString(id_item)) {
        strlcpy(id, id_item->valuestring, sizeof(id));
    }

    /* Current values, so a request may change just one field. */
    snapserver_client_info_t *clients = NULL;
    const size_t count = take_clients(&clients);
    const snapserver_client_info_t *current = NULL;
    for (size_t i = 0; i < count && id[0] != '\0'; ++i) {
        if (strcmp(clients[i].id, id) == 0) {
            current = &clients[i];
        }
    }
    if (current == NULL) {
        free(clients);
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such client connected");
        return ESP_OK;
    }

    int32_t volume = current->volume_percent;
    bool muted = current->muted;
    int32_t delay_ms = -current->latency_ms;
    char name[64];
    strlcpy(name, current->name, sizeof(name));
    free(clients);

    double num = 0.0;
    const bool volume_given = parse_number_field(root, "volume_percent", &num);
    if (volume_given) volume = (int32_t)num;
    const bool muted_given = cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root, "muted"));
    parse_bool_field(root, "muted", &muted);
    const bool delay_given = parse_number_field(root, "delay_ms", &num);
    if (delay_given) delay_ms = (int32_t)num;
    const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
    const bool name_given = cJSON_IsString(name_item);
    if (name_given) {
        strlcpy(name, name_item->valuestring, sizeof(name));
    }
    cJSON_Delete(root);

    /* The client's ring holds 2 x bufferMs; the trim range fits in that. */
    if (volume < 0 || volume > 100 ||
        delay_ms < -DEVICE_CONFIG_DELAY_TRIM_MAX_MS || delay_ms > DEVICE_CONFIG_DELAY_TRIM_MAX_MS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "out of range");
        return ESP_OK;
    }

    if (volume_given || muted_given) {
        (void)snapserver_set_client_volume(id, volume, muted);
    }
    if (delay_given) {
        (void)snapserver_set_client_latency(id, -delay_ms);
    }
    if (name_given) {
        (void)snapserver_set_client_name(id, name);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(req, resp);
}

/* ------------------------------------------------------------------ */
/* One client's settings, through the server (server role)            */
/* ------------------------------------------------------------------ */

#define REMOTE_CONFIG_TIMEOUT_MS 3000
#define REMOTE_STATUS_TIMEOUT_MS 2000

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "?id=..." -- percent-decoded, since the page encodes the MAC's colons
 * and httpd_query_key_value() hands back the raw text. */
static bool query_device_id(httpd_req_t *req, char *id, size_t id_len)
{
    char query[192];
    char raw[160];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    size_t out = 0;
    for (size_t i = 0; raw[i] != '\0' && out + 1U < id_len; ++i) {
        const int hi = (raw[i] == '%') ? hex_value(raw[i + 1]) : -1;
        const int lo = (hi >= 0) ? hex_value(raw[i + 2]) : -1;
        if (lo >= 0) {
            id[out++] = (char)(hi * 16 + lo);
            i += 2;
        } else {
            id[out++] = (raw[i] == '+') ? ' ' : raw[i];
        }
    }
    id[out] = '\0';
    return out > 0U;
}

/* Takes ownership of request. Passes the client's answer through as is. */
static esp_err_t forward_to_client(httpd_req_t *req, cJSON *request, uint32_t timeout_ms)
{
    device_config_t cfg;
    device_config_get(&cfg);
    char id[64];
    if (cfg.role != DEVICE_ROLE_SERVER || !query_device_id(req, id, sizeof(id))) {
        cJSON_Delete(request);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such device");
        return ESP_OK;
    }

    char *json = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (json == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char *reply = NULL;
    const esp_err_t rc = snapserver_remote_request(id, json, &reply, timeout_ms);
    free(json);

    switch (rc) {
    case ESP_OK:
        break;
    case ESP_ERR_NOT_FOUND:
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "device not connected");
        return ESP_OK;
    case ESP_ERR_TIMEOUT:
        httpd_resp_set_status(req, "504 Gateway Timeout");
        httpd_resp_sendstr(req, "device did not answer");
        return ESP_OK;
    default:
        httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_sendstr(req, "connection to the device lost");
        return ESP_OK;
    }

    /* {"error": ...} is the client turning the request down. */
    cJSON *parsed = cJSON_Parse(reply);
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(parsed, "error");
    esp_err_t result = ESP_OK;
    if (parsed == NULL) {
        httpd_resp_set_status(req, "502 Bad Gateway");
        result = httpd_resp_sendstr(req, "invalid answer from the device");
    } else if (cJSON_IsString(error)) {
        result = httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error->valuestring);
    } else {
        httpd_resp_set_type(req, "application/json");
        result = httpd_resp_sendstr(req, reply);
    }
    cJSON_Delete(parsed);
    free(reply);
    return result;
}

static cJSON *remote_op(const char *op)
{
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "op", op);
    return request;
}

static esp_err_t api_device_config_get_handler(httpd_req_t *req)
{
    return forward_to_client(req, remote_op("get_config"), REMOTE_CONFIG_TIMEOUT_MS);
}

static esp_err_t api_device_status_get_handler(httpd_req_t *req)
{
    return forward_to_client(req, remote_op("status"), REMOTE_STATUS_TIMEOUT_MS);
}

/* Body: the same form as POST /api/config. */
static esp_err_t api_device_config_post_handler(httpd_req_t *req)
{
    char body[WEBCONFIG_MAX_BODY];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request body");
        return ESP_OK;
    }
    cJSON *config = cJSON_Parse(body);
    if (!cJSON_IsObject(config)) {
        cJSON_Delete(config);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_OK;
    }
    cJSON *request = remote_op("set_config");
    cJSON_AddItemToObject(request, "config", config);
    return forward_to_client(req, request, REMOTE_CONFIG_TIMEOUT_MS);
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
    /* The default of 8 is one short of the routes registered below. */
    config.max_uri_handlers = 12;
    /*
     * A browser keeps several connections open per host, an Android
     * WebView up to six, and a phone that roams to another mesh AP drops
     * them without a FIN. With the defaults the server never notices: the
     * dead sessions fill all max_open_sockets (7) and every new request
     * times out, while the announcement port, a server of its own, keeps
     * working. So when full, close the least recently used session, and
     * let TCP keep-alive find dead peers after about 5 + 3 x 5 s.
     */
    config.lru_purge_enable = true;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;

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
        { .uri = "/api/devices", .method = HTTP_GET, .handler = api_devices_get_handler },
        { .uri = "/api/devices", .method = HTTP_POST, .handler = api_devices_post_handler },
        { .uri = "/api/devices/config", .method = HTTP_GET, .handler = api_device_config_get_handler },
        { .uri = "/api/devices/config", .method = HTTP_POST, .handler = api_device_config_post_handler },
        { .uri = "/api/devices/status", .method = HTTP_GET, .handler = api_device_status_get_handler },
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
