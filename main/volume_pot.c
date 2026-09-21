/**
 * @file volume_pot.c
 * @brief Reads a potentiometer on ADC1 and drives the local master volume.
 */

#include "volume_pot.h"

#include "audio_i2s.h"

#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_SNAPSERVER_VOLUME_POT_ENABLE

#include "driver/rtc_io.h"
#include "esp_adc/adc_oneshot.h"

static const char *TAG = "VOLUME_POT";

/*
 * 50 ms between readings. Fast enough that turning the knob feels
 * immediate, slow enough that the task costs nothing measurable. The
 * output stage ramps each new value across a 20 ms frame anyway, so a
 * higher rate would not make the movement any smoother.
 */
#define POLL_INTERVAL_MS 50U

/*
 * Readings averaged per poll. The ESP32-S3 ADC is noisy enough that a
 * single conversion wanders by tens of counts; 16 of them average that
 * down without the task ever running long enough to matter.
 */
#define OVERSAMPLE 16U

/*
 * Full scale of the 12-bit reading, and the dead band a change has to
 * exceed before it is taken seriously. 48 of 4095 is a bit over 1 % of
 * knob travel -- above the residual noise after averaging, below what a
 * hand can hold still. Without it the last bits would dither and the gain
 * would creep continuously.
 */
#define ADC_FULL_SCALE 4095
#define DEAD_BAND_COUNTS 48

/*
 * Knob positions this close to an end are snapped to it. A divider never
 * quite reaches its rails -- wiper resistance at the bottom, the internal
 * pull-up pulling the top slightly past full scale -- and a volume control
 * that cannot be turned fully down is a defect, not a rounding error.
 */
#define END_STOP_COUNTS 64

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_channel;
static int s_percent = -1;

/*
 * Cubic, matching the curve audio_sink_set_volume() already uses for the
 * Snapcast volume. Loudness does not follow the voltage on the wiper, and
 * with a linear mapping everything useful is crowded into the top of the
 * rotation.
 */
static float position_to_gain(float position)
{
    return position * position * position;
}

static void volume_pot_task(void *arg)
{
    (void)arg;

    int last_raw = -1;

    for (;;) {
        int sum = 0;
        unsigned taken = 0U;
        for (unsigned i = 0U; i < OVERSAMPLE; ++i) {
            int value = 0;
            if (adc_oneshot_read(s_adc, s_channel, &value) == ESP_OK) {
                sum += value;
                ++taken;
            }
        }

        if (taken > 0U) {
            const int raw = sum / (int)taken;

            /*
             * Hold the previous value unless the knob has actually moved.
             * The end stops are exempt: they must be reachable even if the
             * move into them is smaller than the dead band.
             */
            const bool at_end = (raw <= END_STOP_COUNTS) ||
                                (raw >= ADC_FULL_SCALE - END_STOP_COUNTS);
            const bool moved = (last_raw < 0) || at_end ||
                               (raw > last_raw + DEAD_BAND_COUNTS) ||
                               (raw < last_raw - DEAD_BAND_COUNTS);

            if (moved) {
                last_raw = raw;

                int clamped = raw;
                if (clamped <= END_STOP_COUNTS) {
                    clamped = 0;
                } else if (clamped >= ADC_FULL_SCALE - END_STOP_COUNTS) {
                    clamped = ADC_FULL_SCALE;
                }

                const float position = (float)clamped / (float)ADC_FULL_SCALE;
                audio_i2s_set_master_volume(position_to_gain(position));
                s_percent = (clamped * 100 + ADC_FULL_SCALE / 2) / ADC_FULL_SCALE;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t volume_pot_start(void)
{
    const int gpio = CONFIG_SNAPSERVER_VOLUME_POT_GPIO;

    adc_unit_t unit = ADC_UNIT_1;
    esp_err_t err = adc_oneshot_io_to_channel(gpio, &unit, &s_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO %d has no ADC channel", gpio);
        return err;
    }
    /*
     * ADC2 shares its hardware with the Wi-Fi radio and cannot be read
     * while the radio is up, which in this project is always. A pin on the
     * wrong unit would read fine on the bench and fail the moment the mesh
     * comes up, so refuse it here instead.
     */
    if (unit != ADC_UNIT_1) {
        ESP_LOGE(TAG,
                 "GPIO %d is on ADC2, which is unusable while Wi-Fi runs -- "
                 "pick a pin on ADC1 (GPIO 1-10)",
                 gpio);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 init failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * 12 dB attenuation puts the full 0-3.3 V swing of the divider inside
     * the measurable range. Accuracy is irrelevant here: the reading is a
     * knob position, not a measurement of anything.
     */
    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc, s_channel, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Order matters: adc_oneshot_config_channel() explicitly disables both
     * pulls on the pad, so this has to come after it. The pull-up is what
     * makes an unfitted potentiometer read full scale instead of floating
     * at some arbitrary voltage -- without it, a board with no knob would
     * play at a random volume on every boot.
     *
     * It costs a little accuracy while a knob *is* fitted: roughly 45 kOhm
     * against the wiper's source impedance, which peaks at a quarter of
     * the track resistance in mid rotation. With 10 kOhm that is about
     * 2.5 kOhm, so the reading sits some 2.6 % high in the middle and is
     * exact at both ends. For a volume control that is inaudible.
     */
    err = rtc_gpio_pullup_en(gpio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Pull-up on GPIO %d failed: %s", gpio, esp_err_to_name(err));
        return err;
    }
    err = rtc_gpio_pulldown_dis(gpio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Pull-down off on GPIO %d failed: %s", gpio, esp_err_to_name(err));
        return err;
    }

    BaseType_t created = xTaskCreate(volume_pot_task, "volume_pot", 2560, NULL, 2, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "Volume knob on GPIO %d (ADC1); no knob fitted means full volume",
             gpio);
    return ESP_OK;
}

int volume_pot_percent(void)
{
    return s_percent;
}

#else /* CONFIG_SNAPSERVER_VOLUME_POT_ENABLE */

esp_err_t volume_pot_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

int volume_pot_percent(void)
{
    return -1;
}

#endif /* CONFIG_SNAPSERVER_VOLUME_POT_ENABLE */
