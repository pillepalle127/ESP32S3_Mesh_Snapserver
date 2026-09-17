/**
 * @file device_config.c
 * @brief NVS-backed runtime configuration: load/save/factory-reset.
 */
#include "device_config.h"

#include <string.h>

#include "audio_i2s.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "DEVICE_CONFIG";

#define DEVICE_CONFIG_NVS_NAMESPACE "devcfg"
#define DEVICE_CONFIG_NVS_KEY       "cfg"

/*
 * s_cfg is written from whichever task calls device_config_save()/
 * _set_boot_fail_count() (the HTTP task, or the provisioning grace-window
 * task) and read from device_config_get() by any of those plus app_main's
 * startup task. s_cfg_lock guards only the struct copy in and out; the NVS
 * I/O in write_blob() always happens outside the critical section.
 */
static device_config_t s_cfg;
static portMUX_TYPE s_cfg_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_first_boot;
/*
 * Latched by device_config_factory_reset(). Once set, device_config_save()
 * and device_config_set_boot_fail_count() become no-ops: a factory reset is
 * always immediately followed by a reboot (see webconfig.c), but anything
 * that was already mid-flight before the erase -- most notably the
 * provisioning grace-window task writing back a boot-fail count -- would
 * otherwise silently resurrect the erased config in the few seconds before
 * that reboot happens. provisioning_cancel_grace_window() covers the one
 * known such writer; this catches any other one too.
 */
static bool s_erased;

static void seed_defaults(device_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->version = DEVICE_CONFIG_VERSION;

    /*
     * A bool Kconfig option that's off isn't defined as 0 -- it isn't
     * defined at all, so CONFIG_SNAPSERVER_ENABLE_MESH_LITE only exists as
     * a token here inside an #if (where an undefined macro correctly reads
     * as 0 per the C standard). Used as a plain expression value it would
     * be an undeclared identifier, hence the #if/#else instead of a
     * ternary. MESH_SOFTAP_SSID_PREFIX/_PASSWORD/MESH_CHANNEL all `depends
     * on SNAPSERVER_ENABLE_MESH_LITE` in Kconfig.projbuild for the same
     * reason: they don't exist as macros at all in a build with that
     * option off.
     */
#if CONFIG_SNAPSERVER_ENABLE_MESH_LITE
    cfg->mesh_enable = true;
    strlcpy(cfg->mesh_ssid, CONFIG_MESH_SOFTAP_SSID_PREFIX, sizeof(cfg->mesh_ssid));
    strlcpy(cfg->mesh_password, CONFIG_MESH_SOFTAP_PASSWORD, sizeof(cfg->mesh_password));
    cfg->mesh_channel = CONFIG_MESH_CHANNEL;
#else
    cfg->mesh_enable = false;
    cfg->mesh_ssid[0] = '\0';
    cfg->mesh_password[0] = '\0';
    cfg->mesh_channel = 1;
#endif
    cfg->mesh_max_level = CONFIG_MESH_LITE_MAXIMUM_LEVEL_ALLOWED;

    cfg->dsp_bypass = false;
    cfg->crossover_hz = CONFIG_SNAPSERVER_CROSSOVER_HZ;
    cfg->sub_gain_db = 0.0f;
    cfg->wideband_gain_db = 0.0f;
    cfg->sub_channel = SUBWOOFER_OUTPUT_CHANNEL;
    cfg->wideband_channel = WIDEBAND_OUTPUT_CHANNEL;

    cfg->opus_bitrate = CONFIG_SNAPSERVER_OPUS_BITRATE;
    cfg->opus_complexity = CONFIG_SNAPSERVER_OPUS_COMPLEXITY;

    cfg->boot_fail_count = 0;

    cfg->role = DEVICE_ROLE_SERVER;
    cfg->buffer_ms = 3000;
    cfg->delay_trim_ms = 0;
    cfg->source_mode = SOURCE_MODE_AUTO;
    cfg->local_input_threshold_db = -40;
    cfg->server_host[0] = '\0';
}

static bool config_is_valid(const device_config_t *cfg)
{
    /*
     * mesh_root.c falls back to WIFI_AUTH_OPEN below 8 characters. A
     * password in [1,7] would silently run the AP open while api_config_get
     * still believes it's protected and echoes it back -- reject that
     * range outright. 0 stays allowed (a deliberately open network).
     */
    const size_t password_len = strlen(cfg->mesh_password);
    if (password_len >= 1U && password_len < 8U) {
        return false;
    }
    if (cfg->mesh_channel < 1U || cfg->mesh_channel > 13U) {
        return false;
    }
    if (cfg->mesh_max_level < 1U || cfg->mesh_max_level > 15U) {
        return false;
    }
    if (cfg->crossover_hz < 40U || cfg->crossover_hz > 500U) {
        return false;
    }
    if (cfg->sub_gain_db < -24.0f || cfg->sub_gain_db > 12.0f) {
        return false;
    }
    if (cfg->wideband_gain_db < -24.0f || cfg->wideband_gain_db > 12.0f) {
        return false;
    }
    if (cfg->sub_channel > 1U || cfg->wideband_channel > 1U ||
        cfg->sub_channel == cfg->wideband_channel) {
        return false;
    }
    if (cfg->opus_bitrate < 16000U || cfg->opus_bitrate > 192000U) {
        return false;
    }
    if (cfg->opus_complexity > 10U) {
        return false;
    }
    if (cfg->role != DEVICE_ROLE_SERVER && cfg->role != DEVICE_ROLE_CLIENT) {
        return false;
    }
    if (cfg->source_mode != SOURCE_MODE_AUTO &&
        cfg->source_mode != SOURCE_MODE_NETWORK_ONLY &&
        cfg->source_mode != SOURCE_MODE_LOCAL_ONLY) {
        return false;
    }
    /*
     * buffer_ms is the end-to-end Snapcast latency (announced to clients as
     * bufferMs, see snapserver.c) as well as the client's own playback
     * ring-buffer size. Lower bound keeps the ring buffer from being too
     * small to absorb normal jitter; upper bound is a sanity cap, not a
     * hardware limit.
     */
    if (cfg->buffer_ms < 200U || cfg->buffer_ms > 10000U) {
        return false;
    }
    if (cfg->delay_trim_ms < -2000 || cfg->delay_trim_ms > 2000) {
        return false;
    }
    if (cfg->local_input_threshold_db < -80 || cfg->local_input_threshold_db > 0) {
        return false;
    }
    return true;
}

static esp_err_t write_blob(const device_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(result));
        return result;
    }

    result = nvs_set_blob(handle, DEVICE_CONFIG_NVS_KEY, cfg, sizeof(*cfg));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Storing config failed: %s", esp_err_to_name(result));
    }

    nvs_close(handle);
    return result;
}

esp_err_t device_config_load(void)
{
    device_config_t loaded = {0};
    size_t len = sizeof(loaded);

    nvs_handle_t handle;
    esp_err_t result = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(result));
        return result;
    }

    result = nvs_get_blob(handle, DEVICE_CONFIG_NVS_KEY, &loaded, &len);
    nvs_close(handle);

    const bool found = (result == ESP_OK) && (len == sizeof(loaded)) &&
                        (loaded.version == DEVICE_CONFIG_VERSION) &&
                        config_is_valid(&loaded);

    device_config_t next;
    if (found) {
        next = loaded;
        s_first_boot = false;
        ESP_LOGI(TAG, "Config loaded from NVS");
    } else {
        if (result == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "No stored config found, seeding defaults");
        } else {
            ESP_LOGW(TAG,
                     "Stored config unusable (err=%s, len=%u), seeding defaults",
                     esp_err_to_name(result),
                     (unsigned)len);
        }
        seed_defaults(&next);
        s_first_boot = true;
        result = write_blob(&next);
        if (result != ESP_OK) {
            return result;
        }
    }

    portENTER_CRITICAL(&s_cfg_lock);
    s_cfg = next;
    portEXIT_CRITICAL(&s_cfg_lock);
    return ESP_OK;
}

esp_err_t device_config_save(const device_config_t *cfg)
{
    if (s_erased) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg == NULL || !config_is_valid(cfg)) {
        return ESP_ERR_INVALID_ARG;
    }

    device_config_t to_store = *cfg;
    to_store.version = DEVICE_CONFIG_VERSION;

    /*
     * An explicit save is a fresh, deliberate reconfiguration attempt, so
     * give the mesh boot streak a clean slate. Without this, a device stuck
     * in the provisioning-AP boot-fail-streak loop could never earn its way
     * back to a normal mesh boot: the streak only resets on a successful
     * mesh boot, and a streak boot never attempts one (see provisioning.c).
     */
    to_store.boot_fail_count = 0;

    const esp_err_t result = write_blob(&to_store);
    if (result != ESP_OK) {
        return result;
    }

    portENTER_CRITICAL(&s_cfg_lock);
    s_cfg = to_store;
    portEXIT_CRITICAL(&s_cfg_lock);
    return ESP_OK;
}

esp_err_t device_config_set_boot_fail_count(uint8_t count)
{
    if (s_erased) {
        return ESP_ERR_INVALID_STATE;
    }

    device_config_t to_store;
    portENTER_CRITICAL(&s_cfg_lock);
    to_store = s_cfg;
    portEXIT_CRITICAL(&s_cfg_lock);
    to_store.boot_fail_count = count;

    const esp_err_t result = write_blob(&to_store);
    if (result != ESP_OK) {
        return result;
    }

    portENTER_CRITICAL(&s_cfg_lock);
    s_cfg = to_store;
    portEXIT_CRITICAL(&s_cfg_lock);
    return ESP_OK;
}

esp_err_t device_config_factory_reset(void)
{
    /*
     * Set before touching NVS, not after: the point is to close the window
     * for a racing writer, and nvs_erase_key()+nvs_commit() below is exactly
     * the kind of operation a concurrent write could interleave with.
     */
    s_erased = true;

    nvs_handle_t handle;
    esp_err_t result = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(result));
        return result;
    }

    result = nvs_erase_key(handle, DEVICE_CONFIG_NVS_KEY);
    if (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND) {
        result = nvs_commit(handle);
    } else {
        ESP_LOGE(TAG, "Erasing config failed: %s", esp_err_to_name(result));
    }

    nvs_close(handle);
    return result;
}

void device_config_get(device_config_t *out)
{
    portENTER_CRITICAL(&s_cfg_lock);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_cfg_lock);
}

bool device_config_is_first_boot(void)
{
    return s_first_boot;
}
