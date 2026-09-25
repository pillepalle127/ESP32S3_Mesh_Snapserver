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
#include "pinmap.h"
#include "pots.h"
#include "sdkconfig.h"

static const char *TAG = "DEVICE_CONFIG";

#define DEVICE_CONFIG_NVS_NAMESPACE "devcfg"
#define DEVICE_CONFIG_NVS_KEY       "cfg"
#define DEVICE_POTS_NVS_KEY         "pots"
#define DEVICE_PINS_NVS_KEY         "pins"
#define DEVICE_LVOL_NVS_KEY         "lvol"
#define DEVICE_SVOL_NVS_KEY         "svol"

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

/* Guarded by s_cfg_lock like s_cfg. */
static device_local_volume_t s_local_volume = { .percent = 100U };
static device_local_volume_t s_stream_volume = { .percent = 100U };

static void load_volume_blob(nvs_handle_t handle, const char *key, device_local_volume_t *out);

static device_pots_t s_pots = {
    .volume_gpio = DEVICE_POTS_DEFAULT_VOLUME_GPIO,
    .delay_gpio = DEVICE_POTS_DEFAULT_DELAY_GPIO,
    .delay_range_ms = DEVICE_POTS_DEFAULT_DELAY_RANGE_MS,
};

static void seed_pot_defaults(device_pots_t *pots)
{
    pots->volume_gpio = DEVICE_POTS_DEFAULT_VOLUME_GPIO;
    pots->delay_gpio = DEVICE_POTS_DEFAULT_DELAY_GPIO;
    pots->delay_range_ms = DEVICE_POTS_DEFAULT_DELAY_RANGE_MS;
}

/* Guarded by s_cfg_lock like s_cfg. What the hardware was started with is
 * s_boot_pins; s_pins moves on with every save, ahead of the next reboot. */
static device_pins_t s_pins;
static device_pins_t s_boot_pins;
static bool s_pins_reverted;

static bool pins_valid(const device_pins_t *pins);

static void seed_pin_defaults(device_pins_t *pins)
{
    memset(pins, 0, sizeof(*pins));
    pins->i2s_bclk = AUDIO_I2S_DEFAULT_GPIO_BCLK;
    pins->i2s_lrclk = AUDIO_I2S_DEFAULT_GPIO_LRCLK;
    pins->i2s_din = AUDIO_I2S_DEFAULT_GPIO_DIN;
    pins->i2s_dout = AUDIO_I2S_DEFAULT_GPIO_DOUT;
#if CONFIG_SNAPSERVER_STATUS_LED_ENABLE
    pins->status_led = CONFIG_SNAPSERVER_STATUS_LED_GPIO;
#endif
    /* The Kconfig range for the LED allows pins the rules reject (or one
     * of the I2S pins); a default that fails its own check would leave no
     * valid pin set at all, so drop the LED instead. */
    if (!pins_valid(pins)) {
        pins->status_led = 0U;
    }
}

/* True if gpio is one of the pins *pins assigns. 0 is never taken. */
static bool pin_taken(const device_pins_t *pins, uint8_t gpio)
{
    if (gpio == 0U) {
        return false;
    }
    return gpio == pins->i2s_bclk || gpio == pins->i2s_lrclk ||
           gpio == pins->i2s_din || gpio == pins->i2s_dout ||
           gpio == pins->status_led;
}

static bool pins_valid(const device_pins_t *pins)
{
    if (pins->i2s_slave > 1U) {
        return false;
    }
    const uint8_t used[] = {
        pins->i2s_bclk, pins->i2s_lrclk, pins->i2s_din, pins->i2s_dout, pins->status_led,
    };
    const size_t count = sizeof(used) / sizeof(used[0]);
    for (size_t i = 0; i < count; ++i) {
        /* Only the LED (the last entry) may be left unassigned. */
        if (used[i] == 0U && i == count - 1U) {
            continue;
        }
        if (pinmap_blocked_reason(used[i]) != NULL) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (used[i] == used[j]) {
                return false;
            }
        }
    }
    return true;
}

/* Whether two pin sets assign different pins or I2S roles; trial_boots
 * does not count. The role is in here because a wrong one can leave the
 * device without an audio clock, which the trial boots must catch too. */
static bool pins_differ(const device_pins_t *a, const device_pins_t *b)
{
    return a->i2s_bclk != b->i2s_bclk || a->i2s_lrclk != b->i2s_lrclk ||
           a->i2s_din != b->i2s_din || a->i2s_dout != b->i2s_dout ||
           a->status_led != b->status_led || a->i2s_slave != b->i2s_slave;
}

static esp_err_t write_pins_blob(const device_pins_t *pins)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(result));
        return result;
    }
    result = nvs_set_blob(handle, DEVICE_PINS_NVS_KEY, pins, sizeof(*pins));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Storing pin assignment failed: %s", esp_err_to_name(result));
    }
    return result;
}

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
    if (cfg->delay_trim_ms < -DEVICE_CONFIG_DELAY_TRIM_MAX_MS ||
        cfg->delay_trim_ms > DEVICE_CONFIG_DELAY_TRIM_MAX_MS) {
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

    /*
     * Potentiometer settings are optional: a device updated from firmware
     * without them, or one that never saved them, simply gets the defaults
     * -- and is not treated as a first boot for that, because nothing the
     * user configured has been lost.
     */
    device_pots_t pots;
    size_t pots_len = sizeof(pots);
    const esp_err_t pots_result = nvs_get_blob(handle, DEVICE_POTS_NVS_KEY, &pots, &pots_len);

    /* Optional in the same way: missing means "the pins this firmware
     * always had". */
    device_pins_t pins;
    size_t pins_len = sizeof(pins);
    const esp_err_t pins_result = nvs_get_blob(handle, DEVICE_PINS_NVS_KEY, &pins, &pins_len);

    /* Optional too: missing or out of range means full volume, unmuted. */
    device_local_volume_t local_volume;
    load_volume_blob(handle, DEVICE_LVOL_NVS_KEY, &local_volume);
    device_local_volume_t stream_volume;
    load_volume_blob(handle, DEVICE_SVOL_NVS_KEY, &stream_volume);
    nvs_close(handle);

    if (pins_result != ESP_OK || pins_len != sizeof(pins) || !pins_valid(&pins)) {
        if (pins_result != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Stored pin assignment unusable, using defaults");
        }
        seed_pin_defaults(&pins);
    } else if (pins.trial_boots > DEVICE_PINS_TRIAL_BOOTS_MAX) {
        /*
         * This set has been booted DEVICE_PINS_TRIAL_BOOTS_MAX times and
         * never confirmed -- whatever stopped it (a crash in the I2S or
         * LED driver, most likely) will stop it again. Stored, so the page
         * shows the pins actually in use rather than the dropped ones.
         */
        ESP_LOGE(TAG,
                 "Pin assignment did not come up in %u boots, back to defaults",
                 (unsigned)DEVICE_PINS_TRIAL_BOOTS_MAX);
        seed_pin_defaults(&pins);
        s_pins_reverted = true;
        (void)write_pins_blob(&pins);
    } else if (pins.trial_boots != 0U) {
        /* Counted before anything touches the pins, so a crash on the way
         * up still leaves this boot on the record. */
        ESP_LOGW(TAG, "Pin assignment on trial, boot %u of %u",
                 (unsigned)pins.trial_boots, (unsigned)DEVICE_PINS_TRIAL_BOOTS_MAX);
        ++pins.trial_boots;
        (void)write_pins_blob(&pins);
    }

    if (pots_result != ESP_OK || pots_len != sizeof(pots) || !device_config_pots_valid(&pots)) {
        if (pots_result != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Stored knob settings unusable, using defaults");
        }
        seed_pot_defaults(&pots);
    }
    /*
     * Saves check knobs and pins as a pair, so a clash here means one of
     * the two fell back to defaults above. The knob gives way: a missing
     * knob plays at full volume, a knob reading an I2S line does not.
     */
    if (pin_taken(&pins, pots.volume_gpio)) {
        ESP_LOGW(TAG, "Volume knob pin %u is in use, knob disabled", (unsigned)pots.volume_gpio);
        pots.volume_gpio = 0U;
    }
    if (pin_taken(&pins, pots.delay_gpio)) {
        ESP_LOGW(TAG, "Delay knob pin %u is in use, knob disabled", (unsigned)pots.delay_gpio);
        pots.delay_gpio = 0U;
    }
    portENTER_CRITICAL(&s_cfg_lock);
    s_pots = pots;
    s_pins = pins;
    s_boot_pins = pins;
    s_local_volume = local_volume;
    s_stream_volume = stream_volume;
    portEXIT_CRITICAL(&s_cfg_lock);

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
        /* Knobs and pins go back to their defaults too -- a factory reset
         * that left a delay pin configured would be a surprise. */
        const esp_err_t pots_result = nvs_erase_key(handle, DEVICE_POTS_NVS_KEY);
        if (pots_result != ESP_OK && pots_result != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Erasing knob settings failed: %s", esp_err_to_name(pots_result));
        }
        const esp_err_t pins_result = nvs_erase_key(handle, DEVICE_PINS_NVS_KEY);
        if (pins_result != ESP_OK && pins_result != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Erasing pin assignment failed: %s", esp_err_to_name(pins_result));
        }
        const char *const volume_keys[] = { DEVICE_LVOL_NVS_KEY, DEVICE_SVOL_NVS_KEY };
        for (size_t i = 0; i < sizeof(volume_keys) / sizeof(volume_keys[0]); ++i) {
            const esp_err_t vol_result = nvs_erase_key(handle, volume_keys[i]);
            if (vol_result != ESP_OK && vol_result != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "Erasing %s failed: %s", volume_keys[i], esp_err_to_name(vol_result));
            }
        }
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

bool device_config_pots_valid(const device_pots_t *pots)
{
    if (pots == NULL) {
        return false;
    }
    if (pots->volume_gpio != 0U && pots_pin_blocked_reason(pots->volume_gpio) != NULL) {
        return false;
    }
    if (pots->delay_gpio != 0U && pots_pin_blocked_reason(pots->delay_gpio) != NULL) {
        return false;
    }
    if (pots->volume_gpio != 0U && pots->volume_gpio == pots->delay_gpio) {
        return false;
    }
    if (pots->delay_range_ms < DEVICE_POTS_DELAY_RANGE_MIN_MS ||
        pots->delay_range_ms > DEVICE_CONFIG_DELAY_TRIM_MAX_MS) {
        return false;
    }
    return true;
}

/* Volume blobs share one layout; missing or out of range means 100 %, unmuted. */
static void load_volume_blob(nvs_handle_t handle, const char *key, device_local_volume_t *out)
{
    size_t len = sizeof(*out);
    if (nvs_get_blob(handle, key, out, &len) != ESP_OK || len != sizeof(*out) ||
        out->percent > 100U) {
        memset(out, 0, sizeof(*out));
        out->percent = 100U;
    }
    out->muted = out->muted ? 1U : 0U;
}

static esp_err_t store_volume_blob(const char *key, device_local_volume_t *cache,
                                   const device_local_volume_t *volume)
{
    if (s_erased) {
        return ESP_ERR_INVALID_STATE;
    }
    if (volume == NULL || volume->percent > 100U) {
        return ESP_ERR_INVALID_ARG;
    }
    device_local_volume_t to_store = { .percent = volume->percent,
                                       .muted = volume->muted ? 1U : 0U };

    nvs_handle_t handle;
    esp_err_t result = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(result));
        return result;
    }
    result = nvs_set_blob(handle, key, &to_store, sizeof(to_store));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Storing %s failed: %s", key, esp_err_to_name(result));
        return result;
    }

    portENTER_CRITICAL(&s_cfg_lock);
    *cache = to_store;
    portEXIT_CRITICAL(&s_cfg_lock);
    return ESP_OK;
}

void device_config_get_local_volume(device_local_volume_t *out)
{
    portENTER_CRITICAL(&s_cfg_lock);
    *out = s_local_volume;
    portEXIT_CRITICAL(&s_cfg_lock);
}

esp_err_t device_config_save_local_volume(const device_local_volume_t *volume)
{
    return store_volume_blob(DEVICE_LVOL_NVS_KEY, &s_local_volume, volume);
}

void device_config_get_stream_volume(device_local_volume_t *out)
{
    portENTER_CRITICAL(&s_cfg_lock);
    *out = s_stream_volume;
    portEXIT_CRITICAL(&s_cfg_lock);
}

esp_err_t device_config_save_stream_volume(const device_local_volume_t *volume)
{
    return store_volume_blob(DEVICE_SVOL_NVS_KEY, &s_stream_volume, volume);
}

void device_config_get_pots(device_pots_t *out)
{
    portENTER_CRITICAL(&s_cfg_lock);
    *out = s_pots;
    portEXIT_CRITICAL(&s_cfg_lock);
}

bool device_config_pin_set_valid(const device_pins_t *pins, const device_pots_t *pots)
{
    if (pins == NULL || !pins_valid(pins) || !device_config_pots_valid(pots)) {
        return false;
    }
    return !pin_taken(pins, pots->volume_gpio) && !pin_taken(pins, pots->delay_gpio);
}

void device_config_get_pins(device_pins_t *out)
{
    portENTER_CRITICAL(&s_cfg_lock);
    *out = s_pins;
    portEXIT_CRITICAL(&s_cfg_lock);
}

bool device_config_pins_reverted(void)
{
    return s_pins_reverted;
}

esp_err_t device_config_save_pin_set(const device_pins_t *pins, const device_pots_t *pots)
{
    if (s_erased) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!device_config_pin_set_valid(pins, pots)) {
        return ESP_ERR_INVALID_ARG;
    }

    device_pins_t stored;
    portENTER_CRITICAL(&s_cfg_lock);
    stored = s_pins;
    portEXIT_CRITICAL(&s_cfg_lock);

    device_pins_t to_store = *pins;
    to_store.reserved = 0U;
    /* Unchanged pins keep whatever trial state they are in. */
    to_store.trial_boots = pins_differ(&stored, pins) ? 1U : stored.trial_boots;

    nvs_handle_t handle;
    esp_err_t result = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(result));
        return result;
    }
    /* One commit for both, so knobs and pins never land half-updated. */
    result = nvs_set_blob(handle, DEVICE_POTS_NVS_KEY, pots, sizeof(*pots));
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, DEVICE_PINS_NVS_KEY, &to_store, sizeof(to_store));
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Storing pins and knobs failed: %s", esp_err_to_name(result));
        return result;
    }

    portENTER_CRITICAL(&s_cfg_lock);
    s_pots = *pots;
    s_pins = to_store;
    portEXIT_CRITICAL(&s_cfg_lock);
    return ESP_OK;
}

void device_config_confirm_pins(void)
{
    device_pins_t current;
    portENTER_CRITICAL(&s_cfg_lock);
    current = s_pins;
    portEXIT_CRITICAL(&s_cfg_lock);

    /*
     * Only the set this boot actually started with can be vouched for. A
     * save that landed before startup finished is still untested and has
     * to keep its trial for the next boot.
     */
    if (s_erased || current.trial_boots == 0U || pins_differ(&current, &s_boot_pins)) {
        return;
    }

    current.trial_boots = 0U;
    if (write_pins_blob(&current) != ESP_OK) {
        return;
    }
    portENTER_CRITICAL(&s_cfg_lock);
    if (!pins_differ(&s_pins, &current)) {
        s_pins.trial_boots = 0U;
    }
    portEXIT_CRITICAL(&s_cfg_lock);
    ESP_LOGI(TAG, "Pin assignment confirmed");
}
