/**
 * @file device_config.c
 * @brief NVS-backed runtime configuration: load/save/factory-reset.
 */
#include "device_config.h"

#include <string.h>

#include "audio_i2s.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "DEVICE_CONFIG";

#define DEVICE_CONFIG_NVS_NAMESPACE "devcfg"
#define DEVICE_CONFIG_NVS_KEY       "cfg"

static device_config_t s_cfg;
static bool s_loaded;
static bool s_first_boot;

static void seed_defaults(device_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->version = DEVICE_CONFIG_VERSION;

    cfg->mesh_enable = CONFIG_SNAPSERVER_ENABLE_MESH_LITE ? true : false;
    strlcpy(cfg->mesh_ssid, CONFIG_MESH_SOFTAP_SSID_PREFIX, sizeof(cfg->mesh_ssid));
    strlcpy(cfg->mesh_password, CONFIG_MESH_SOFTAP_PASSWORD, sizeof(cfg->mesh_password));
    cfg->mesh_channel = CONFIG_MESH_CHANNEL;
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
}

static bool config_is_valid(const device_config_t *cfg)
{
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

    if (found) {
        s_cfg = loaded;
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
        seed_defaults(&s_cfg);
        s_first_boot = true;
        result = write_blob(&s_cfg);
        if (result != ESP_OK) {
            return result;
        }
    }

    s_loaded = true;
    return ESP_OK;
}

esp_err_t device_config_save(const device_config_t *cfg)
{
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

    s_cfg = to_store;
    return ESP_OK;
}

esp_err_t device_config_set_boot_fail_count(uint8_t count)
{
    device_config_t to_store = s_cfg;
    to_store.boot_fail_count = count;

    const esp_err_t result = write_blob(&to_store);
    if (result != ESP_OK) {
        return result;
    }

    s_cfg = to_store;
    return ESP_OK;
}

esp_err_t device_config_factory_reset(void)
{
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

const device_config_t *device_config_get(void)
{
    return &s_cfg;
}

bool device_config_is_first_boot(void)
{
    return s_first_boot;
}
