/**
 * @file power_button.c
 * @brief Switch-off and switch-on by a held button (deep sleep).
 */

#include "power_button.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pinmap.h"
#include "soc/soc_caps.h"
#include "status_led.h"

static const char *TAG = "POWER_BUTTON";

#define POLL_MS 20U
/* Up for this long before the button counts as let go (contact bounce). */
#define RELEASE_MS 100U

static device_pins_t s_pins;

/* The GPIO armed as wake-up, kept through deep sleep: power_button_boot()
 * runs before the config is loaded. */
static RTC_DATA_ATTR uint8_t s_wake_button;

static bool pressed(void)
{
    return gpio_get_level((gpio_num_t)s_pins.power_button) == 0;
}

/* Blocks until the button has been up for RELEASE_MS. */
static void wait_for_release(void)
{
    uint32_t up_ms = 0;
    while (up_ms < RELEASE_MS) {
        up_ms = pressed() ? 0U : up_ms + POLL_MS;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

/* Takes gpio from whatever peripheral drives it, low, held through deep sleep. */
static void hold_low(uint8_t gpio)
{
    if (gpio == 0U) {
        return;
    }
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_set_level((gpio_num_t)gpio, 0);
    (void)gpio_config(&io);
    (void)gpio_hold_en((gpio_num_t)gpio);
}

/* Deep sleep until the button goes down. Must be up already: the wake-up
 * is on the level and would fire at once. */
static void sleep_until_press(gpio_num_t button)
{
    (void)esp_sleep_enable_ext0_wakeup(button, 0);
    /* The digital pull-up is gone in deep sleep; the RTC one takes over. */
    (void)rtc_gpio_pullup_en(button);
    (void)rtc_gpio_pulldown_dis(button);
    s_wake_button = (uint8_t)button;
    esp_deep_sleep_start();
}

/*
 * True if the button that woke the chip stays down for
 * POWER_BUTTON_WAKE_HOLD_MS. Up for RELEASE_MS counts as let go, so a
 * bouncing contact does not.
 */
static bool held_after_wake(gpio_num_t button)
{
    /* Still the RTC pin the wake-up used; set up again rather than relied on. */
    (void)rtc_gpio_init(button);
    (void)rtc_gpio_set_direction(button, RTC_GPIO_MODE_INPUT_ONLY);
    (void)rtc_gpio_pullup_en(button);
    (void)rtc_gpio_pulldown_dis(button);

    uint32_t up_ms = 0;
    for (uint32_t waited_ms = 0; waited_ms < POWER_BUTTON_WAKE_HOLD_MS; waited_ms += POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        up_ms = rtc_gpio_get_level(button) == 0 ? 0U : up_ms + POLL_MS;
        if (up_ms >= RELEASE_MS) {
            return false;
        }
    }
    return true;
}

static void switch_off(void)
{
    ESP_LOGW(TAG, "Button held %u ms: switching off", (unsigned)POWER_BUTTON_HOLD_MS);

    status_led_off();

    /*
     * Asleep, the outputs would float, and the DAC or DSP would clock in
     * whatever they pick up. Held low they read as silence. Only the ESP's
     * own outputs: as I2S slave, BCLK and LRCLK belong to the external
     * master and stay inputs.
     */
    hold_low(s_pins.i2s_dout);
    if (s_pins.i2s_slave == 0U) {
        hold_low(s_pins.i2s_bclk);
        hold_low(s_pins.i2s_lrclk);
    }
    hold_low(s_pins.status_led);
#if SOC_GPIO_SUPPORT_HOLD_IO_IN_DSLP && !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_en();
#endif

    wait_for_release();

    ESP_LOGI(TAG, "Off. Hold the button %u ms to switch on again.",
             (unsigned)POWER_BUTTON_WAKE_HOLD_MS);
    sleep_until_press((gpio_num_t)s_pins.power_button);
}

static void button_task(void *arg)
{
    (void)arg;

    /* Still down from the press that switched the device on: not a hold. */
    wait_for_release();

    uint32_t held_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        held_ms = pressed() ? held_ms + POLL_MS : 0U;
        if (held_ms >= POWER_BUTTON_HOLD_MS) {
            switch_off();
        }
    }
}

void power_button_boot(void)
{
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 && s_wake_button != 0U) {
        const gpio_num_t button = (gpio_num_t)s_wake_button;
        /*
         * A knock in a bag must not switch the device on. Back to sleep
         * before the pads below are released, so the outputs stay at
         * silence and the LED dark throughout.
         */
        if (!held_after_wake(button)) {
            ESP_LOGI(TAG, "Button let go within %u ms: back to sleep",
                     (unsigned)POWER_BUTTON_WAKE_HOLD_MS);
            sleep_until_press(button);
        }
        ESP_LOGI(TAG, "Switched on by the power button");
    }

    /*
     * The pads switch_off() held stay held after the wake-up until released.
     * Every usable pin, not just the current assignment: a save may have
     * moved the I2S bus since. Releasing a pin that is not held does nothing.
     */
#if SOC_GPIO_SUPPORT_HOLD_IO_IN_DSLP && !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_dis();
#endif
    for (uint8_t gpio = 1U; gpio <= PINMAP_GPIO_MAX; ++gpio) {
        if (pinmap_blocked_reason(gpio) == NULL) {
            (void)gpio_hold_dis((gpio_num_t)gpio);
        }
    }
}

esp_err_t power_button_start(const device_pins_t *pins)
{
    if (pins == NULL || pins->power_button == 0U) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_pins = *pins;

    const gpio_num_t button = (gpio_num_t)pins->power_button;
    /* The wake-up left it an RTC pin; back to a plain digital input. */
    if (rtc_gpio_is_valid_gpio(button)) {
        (void)rtc_gpio_deinit(button);
    }
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << pins->power_button,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t result = gpio_config(&io);
    if (result != ESP_OK) {
        return result;
    }

    if (xTaskCreate(button_task, "power_btn", 3072, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Power button on GPIO %u: hold %u ms to switch off",
             (unsigned)pins->power_button, (unsigned)POWER_BUTTON_HOLD_MS);
    return ESP_OK;
}
