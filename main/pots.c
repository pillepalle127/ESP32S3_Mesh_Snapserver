/**
 * @file pots.c
 * @brief Volume and delay potentiometers on ADC1.
 */

#include "pots.h"

#include <stdbool.h>
#include <stddef.h>

#include "audio_i2s.h"
#include "audio_sink.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pinmap.h"
#include "sdkconfig.h"

/*
 * ADC1 on the ESP32-S3 is GPIO 1 to 10. ADC2 shares its hardware with the
 * Wi-Fi radio and cannot be read while the radio is up -- which in this
 * project is always -- so a knob there would work on the bench and fail
 * as soon as the mesh comes up.
 *
 * Which of those the I2S bus or the LED already has is not decided here:
 * that depends on the whole pin assignment, which can change in the same
 * save as the knob pins (see device_config_pin_set_valid()).
 */
const char *pots_pin_blocked_reason(uint8_t gpio)
{
    if (!pinmap_is_adc1(gpio)) {
        return "not on ADC1 (only GPIO 1-10 work while Wi-Fi runs)";
    }
    return pinmap_blocked_reason(gpio);
}

#if CONFIG_SNAPSERVER_POTS_ENABLE

#include "driver/rtc_io.h"
#include "esp_adc/adc_oneshot.h"

static const char *TAG = "POTS";

/*
 * 50 ms between readings: turning a knob feels immediate, the task costs
 * nothing measurable, and the output stage ramps each new volume across a
 * 20 ms frame anyway.
 */
#define POLL_INTERVAL_MS 50U

/* Averaged per poll. A single ESP32-S3 conversion wanders by tens of counts. */
#define OVERSAMPLE 16U

/*
 * A change must exceed the dead band before it counts -- a bit over 1 % of
 * travel, above the noise left after averaging, below what a hand holds
 * still. Without it the last bits dither and the output creeps.
 */
#define ADC_FULL_SCALE   4095
#define DEAD_BAND_COUNTS 48

/*
 * Positions this close to an end are snapped to it. A divider never quite
 * reaches its rails, and a volume knob that cannot reach silence -- or a
 * delay knob that cannot reach its limit -- is a defect, not rounding.
 */
#define END_STOP_COUNTS 64

typedef struct {
    bool running;
    adc_channel_t channel;
    int raw; /* last accepted reading, -1 before the first */
} knob_t;

static adc_oneshot_unit_handle_t s_adc;
static knob_t s_volume = { .raw = -1 };
static knob_t s_delay = { .raw = -1 };

/* Written from other tasks; plain loads/stores of these sizes are atomic. */
static volatile uint16_t s_delay_range_ms = DEVICE_POTS_DEFAULT_DELAY_RANGE_MS;
static volatile bool s_delay_armed;
static volatile uint8_t s_role;
static volatile uint16_t s_buffer_ms;

static volatile int s_volume_percent = -1;
static volatile int16_t s_delay_ms;
static volatile bool s_delay_valid;

static int read_averaged(adc_channel_t channel)
{
    int sum = 0;
    unsigned taken = 0U;
    for (unsigned i = 0U; i < OVERSAMPLE; ++i) {
        int value = 0;
        if (adc_oneshot_read(s_adc, channel, &value) == ESP_OK) {
            sum += value;
            ++taken;
        }
    }
    return (taken > 0U) ? (sum / (int)taken) : -1;
}

/*
 * Returns true when the knob has moved enough to act on, and stores the
 * new position (with end stops applied) in knob->raw. End stops are exempt
 * from the dead band so they stay reachable by a small last turn.
 */
static bool update_knob(knob_t *knob)
{
    const int raw = read_averaged(knob->channel);
    if (raw < 0) {
        return false;
    }

    const bool at_end = (raw <= END_STOP_COUNTS) ||
                        (raw >= ADC_FULL_SCALE - END_STOP_COUNTS);
    const bool moved = (knob->raw < 0) || at_end ||
                       (raw > knob->raw + DEAD_BAND_COUNTS) ||
                       (raw < knob->raw - DEAD_BAND_COUNTS);
    if (!moved) {
        return false;
    }

    int clamped = raw;
    if (clamped <= END_STOP_COUNTS) {
        clamped = 0;
    } else if (clamped >= ADC_FULL_SCALE - END_STOP_COUNTS) {
        clamped = ADC_FULL_SCALE;
    }
    if (clamped == knob->raw) {
        return false;
    }
    knob->raw = clamped;
    return true;
}

static float position(const knob_t *knob)
{
    return (float)knob->raw / (float)ADC_FULL_SCALE;
}

static void pots_task(void *arg)
{
    (void)arg;

    int16_t applied_delay_ms = 0;
    bool delay_applied = false;

    for (;;) {
        if (s_volume.running && update_knob(&s_volume)) {
            /*
             * Cubic, the curve audio_sink_set_volume() uses for the
             * Snapcast volume. Linear would crowd everything useful into
             * the top quarter of the rotation.
             */
            const float p = position(&s_volume);
            audio_i2s_set_master_volume(p * p * p);
            s_volume_percent = (s_volume.raw * 100 + ADC_FULL_SCALE / 2) / ADC_FULL_SCALE;
        }

        if (s_delay.running) {
            (void)update_knob(&s_delay);

            if (s_delay.raw >= 0) {
                /*
                 * Recomputed every poll rather than only on movement, so a
                 * range change from the config page takes effect without
                 * touching the knob. Linear: this is an offset in time, not
                 * a loudness, and centre has to mean zero.
                 */
                const float span = (position(&s_delay) * 2.0f) - 1.0f;
                const float ms = span * (float)s_delay_range_ms;
                const int16_t delay_ms = (int16_t)((ms >= 0.0f) ? (ms + 0.5f) : (ms - 0.5f));
                s_delay_ms = delay_ms;
                s_delay_valid = true;

                if (s_delay_armed && (!delay_applied || delay_ms != applied_delay_ms)) {
                    audio_sink_apply_delay_trim(s_role, s_buffer_ms, delay_ms);
                    applied_delay_ms = delay_ms;
                    delay_applied = true;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

/*
 * adc_oneshot_config_channel() explicitly disables both pulls on the pad,
 * so the pull has to be set after it -- in the wrong order it is silently
 * undone and an open input floats.
 */
static esp_err_t open_knob(knob_t *knob, uint8_t gpio, bool pull_up, const char *name)
{
    adc_unit_t unit = ADC_UNIT_1;
    esp_err_t err = adc_oneshot_io_to_channel(gpio, &unit, &knob->channel);
    if (err != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "%s knob: GPIO %u is not on ADC1", name, (unsigned)gpio);
        return (err != ESP_OK) ? err : ESP_ERR_NOT_SUPPORTED;
    }

    /*
     * 12 dB attenuation covers the full 0-3.3 V swing of the divider.
     * Accuracy does not matter: this is a knob position, not a measurement.
     */
    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc, knob->channel, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s knob: channel config failed: %s", name, esp_err_to_name(err));
        return err;
    }

    if (pull_up) {
        err = rtc_gpio_pulldown_dis(gpio);
        if (err == ESP_OK) {
            err = rtc_gpio_pullup_en(gpio);
        }
    } else {
        err = rtc_gpio_pullup_dis(gpio);
        if (err == ESP_OK) {
            err = rtc_gpio_pulldown_en(gpio);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s knob: pull on GPIO %u failed: %s",
                 name, (unsigned)gpio, esp_err_to_name(err));
        return err;
    }

    knob->running = true;
    ESP_LOGI(TAG, "%s knob on GPIO %u (pull-%s)", name, (unsigned)gpio, pull_up ? "up" : "down");
    return ESP_OK;
}

esp_err_t pots_start(const device_pots_t *cfg)
{
    s_delay_range_ms = cfg->delay_range_ms;

    if (cfg->volume_gpio == 0U && cfg->delay_gpio == 0U) {
        ESP_LOGI(TAG, "No knobs configured; full volume, delay trim from config");
        return ESP_OK;
    }

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 init failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Each knob is independent: one failing to open leaves the other
     * running. Volume pulls up (unplugged = full volume), delay pulls
     * down (unplugged = a defined end, not a wandering value).
     */
    esp_err_t first_error = ESP_OK;
    if (cfg->volume_gpio != 0U) {
        err = open_knob(&s_volume, cfg->volume_gpio, true, "Volume");
        if (err != ESP_OK) {
            first_error = err;
        }
    }
    if (cfg->delay_gpio != 0U) {
        err = open_knob(&s_delay, cfg->delay_gpio, false, "Delay");
        if (err != ESP_OK && first_error == ESP_OK) {
            first_error = err;
        }
    }

    if (!s_volume.running && !s_delay.running) {
        return first_error;
    }

    BaseType_t created = xTaskCreate(pots_task, "pots", 3072, NULL, 2, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed");
        s_volume.running = false;
        s_delay.running = false;
        return ESP_ERR_NO_MEM;
    }
    return first_error;
}

void pots_enable_delay(uint8_t role, uint16_t buffer_ms)
{
    s_role = role;
    s_buffer_ms = buffer_ms;
    s_delay_armed = true;
}

void pots_set_delay_range(uint16_t range_ms)
{
    s_delay_range_ms = range_ms;
}

bool pots_delay_active(void)
{
    return s_delay.running;
}

int pots_volume_percent(void)
{
    return s_volume.running ? s_volume_percent : -1;
}

bool pots_delay_ms(int16_t *out)
{
    if (!s_delay.running || !s_delay_valid) {
        return false;
    }
    *out = s_delay_ms;
    return true;
}

#else /* CONFIG_SNAPSERVER_POTS_ENABLE */

esp_err_t pots_start(const device_pots_t *cfg)
{
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}

void pots_enable_delay(uint8_t role, uint16_t buffer_ms)
{
    (void)role;
    (void)buffer_ms;
}

void pots_set_delay_range(uint16_t range_ms)
{
    (void)range_ms;
}

bool pots_delay_active(void)
{
    return false;
}

int pots_volume_percent(void)
{
    return -1;
}

bool pots_delay_ms(int16_t *out)
{
    (void)out;
    return false;
}

#endif /* CONFIG_SNAPSERVER_POTS_ENABLE */
