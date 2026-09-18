/**
 * @file status_led.c
 * @brief WS2812 status/level indicator driven straight from the RMT driver.
 */
#include "status_led.h"

#include <math.h>
#include <string.h>

#include "audio_i2s.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "STATUS_LED";

#if CONFIG_SNAPSERVER_STATUS_LED_ENABLE

/*
 * 10 MHz gives 100 ns per tick, which divides the WS2812 bit timings evenly
 * and keeps every duration below the 15-bit field the RMT symbol allows.
 */
#define LED_RESOLUTION_HZ   10000000
#define LED_T0H_TICKS               3   /* 0.3 us */
#define LED_T0L_TICKS               9   /* 0.9 us */
#define LED_T1H_TICKS               9   /* 0.9 us */
#define LED_T1L_TICKS               3   /* 0.3 us */
#define LED_RESET_TICKS           500   /* 50 us, comfortably over the
                                         *
                                         * datasheet's minimum */

/*
 * 30 Hz. Fast enough that the level reads as movement rather than as steps,
 * slow enough to be irrelevant next to everything else on the device -- the
 * task spends its life asleep and sends 24 bits when it wakes.
 */
#define LED_UPDATE_MS              33
#define LED_TASK_STACK           3072
#define LED_TASK_PRIORITY           1

/*
 * Level window. -50 dBFS is about where quiet passages sit, 0 is full scale,
 * and mapping that span onto brightness keeps the LED moving with the music
 * instead of sitting at either end.
 */
#define LED_LEVEL_FLOOR_DB      -50.0f
#define LED_MIN_BRIGHTNESS       0.06f  /* never fully dark: the colour is
                                         * the status and must stay readable
                                         * through silence */

/*
 * Decay per update when the level falls: full scale to dark in three
 * updates, about 100 ms. Instant attack with a release is still what keeps
 * the meter legible, but 0.12 (over 250 ms) left it sitting near the top
 * through anything continuous.
 */
#define LED_DECAY_PER_UPDATE     0.35f

/*
 * Amplitude window the brightness is spread across.
 *
 * Music measured on this device peaks at 18000-24000 of 32767, so mapping
 * raw amplitude straight to brightness uses barely half the range and never
 * leaves the upper end -- the LED reads as permanently on. Anything under
 * the floor goes dark, the ceiling is full brightness, and the span between
 * carries the movement. The ceiling is deliberately below full scale: a
 * meter that only reaches maximum on a clipped sample never reaches it.
 */
#define LED_LEVEL_LOW            0.12f
#define LED_LEVEL_HIGH           0.80f

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_rgb_t;

static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;
static volatile status_led_state_t s_state = STATUS_LED_BOOTING;
static volatile float s_level_db = -120.0f;

static led_rgb_t state_colour(status_led_state_t state)
{
    switch (state) {
    case STATUS_LED_PROVISIONING: return (led_rgb_t){   0,   0, 255 };
    case STATUS_LED_NO_NETWORK:   return (led_rgb_t){ 255,   0,   0 };
    case STATUS_LED_NO_SERVER:    return (led_rgb_t){ 255,  90,   0 };
    case STATUS_LED_PLAYING:      return (led_rgb_t){   0, 255,   0 };
    case STATUS_LED_LOCAL_INPUT:  return (led_rgb_t){   0, 200, 200 };
    case STATUS_LED_BOOTING:
    default:                      return (led_rgb_t){  60,  60,  60 };
    }
}

static esp_err_t led_write(led_rgb_t colour, float brightness)
{
    if (s_channel == NULL || s_encoder == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (brightness < 0.0f) {
        brightness = 0.0f;
    } else if (brightness > 1.0f) {
        brightness = 1.0f;
    }

    /*
     * Squared, because perceived brightness is far from linear in duty
     * cycle: without it everything above a third of scale looks the same.
     */
    const float scale = brightness * brightness;

    /* WS2812 takes green first. */
    const uint8_t grb[3] = {
        (uint8_t)((float)colour.g * scale),
        (uint8_t)((float)colour.r * scale),
        (uint8_t)((float)colour.b * scale),
    };

    const rmt_transmit_config_t tx_config = { .loop_count = 0 };
    const esp_err_t err = rmt_transmit(s_channel, s_encoder, grb, sizeof(grb), &tx_config);
    if (err != ESP_OK) {
        return err;
    }
    return rmt_tx_wait_all_done(s_channel, pdMS_TO_TICKS(50));
}

/*
 * Server and client measure their output in different places, so each
 * contributes what it already has: the server reads the peak the crossover
 * produced, the client pushes the RMS its player computed. Whichever arrives
 * is converted to the same 0..1 scale here.
 */
static float current_level(void)
{
    int16_t peak_left = 0;
    int16_t peak_right = 0;
    audio_i2s_take_led_peak(&peak_left, &peak_right);

    const int16_t peak = (peak_left > peak_right) ? peak_left : peak_right;

    float amplitude;
    if (peak > 0) {
        amplitude = (float)peak / 32767.0f;
    } else {
        const float db = s_level_db;
        if (db <= LED_LEVEL_FLOOR_DB) {
            return 0.0f;
        }
        amplitude = (db - LED_LEVEL_FLOOR_DB) / (0.0f - LED_LEVEL_FLOOR_DB);
    }

    /* Spread LED_LEVEL_LOW..LED_LEVEL_HIGH across the whole range. */
    const float spread =
        (amplitude - LED_LEVEL_LOW) / (LED_LEVEL_HIGH - LED_LEVEL_LOW);
    if (spread <= 0.0f) {
        return 0.0f;
    }
    return (spread > 1.0f) ? 1.0f : spread;
}

static void led_task(void *arg)
{
    (void)arg;
    float shown = 0.0f;

    for (;;) {
        const float level = current_level();

        /* Instant attack, gradual release -- see LED_DECAY_PER_UPDATE. */
        if (level > shown) {
            shown = level;
        } else {
            shown -= LED_DECAY_PER_UPDATE;
            if (shown < level) {
                shown = level;
            }
            if (shown < 0.0f) {
                shown = 0.0f;
            }
        }

        const float brightness =
            LED_MIN_BRIGHTNESS + shown * (1.0f - LED_MIN_BRIGHTNESS);
        (void)led_write(state_colour(s_state), brightness);

        vTaskDelay(pdMS_TO_TICKS(LED_UPDATE_MS));
    }
}

esp_err_t status_led_start(void)
{
    const rmt_tx_channel_config_t channel_config = {
        .gpio_num = CONFIG_SNAPSERVER_STATUS_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };

    esp_err_t err = rmt_new_tx_channel(&channel_config, &s_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RMT channel on GPIO %d failed: %s",
                 CONFIG_SNAPSERVER_STATUS_LED_GPIO, esp_err_to_name(err));
        return err;
    }

    const rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = { .level0 = 1, .duration0 = LED_T0H_TICKS,
                  .level1 = 0, .duration1 = LED_T0L_TICKS },
        .bit1 = { .level0 = 1, .duration0 = LED_T1H_TICKS,
                  .level1 = 0, .duration1 = LED_T1L_TICKS },
        .flags.msb_first = 1,
    };

    err = rmt_new_bytes_encoder(&encoder_config, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RMT encoder failed: %s", esp_err_to_name(err));
        return err;
    }

    err = rmt_enable(s_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Enabling the RMT channel failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Self-test. This is the only way to tell from the outside whether the
     * configured GPIO is the one this board wired its LED to -- three
     * flashes, or the pin is wrong.
     */
    static const led_rgb_t probe[] = {
        { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 },
    };
    for (size_t i = 0; i < sizeof(probe) / sizeof(probe[0]); ++i) {
        (void)led_write(probe[i], 1.0f);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    (void)led_write(state_colour(STATUS_LED_BOOTING), LED_MIN_BRIGHTNESS);

    if (xTaskCreate(led_task, "status_led", LED_TASK_STACK, NULL,
                    LED_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create the LED task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Status LED ready on GPIO %d (colour = state, brightness = level)",
             CONFIG_SNAPSERVER_STATUS_LED_GPIO);
    return ESP_OK;
}

void status_led_set_state(status_led_state_t state)
{
    s_state = state;
}

void status_led_set_level_db(float dbfs)
{
    s_level_db = dbfs;
}

#else /* !CONFIG_SNAPSERVER_STATUS_LED_ENABLE */

esp_err_t status_led_start(void) { return ESP_OK; }
void status_led_set_state(status_led_state_t state) { (void)state; }
void status_led_set_level_db(float dbfs) { (void)dbfs; }

#endif
