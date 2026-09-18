/**
 * @file status_led.h
 * @brief Single on-board WS2812 as a combined status indicator and level meter.
 *
 * Colour says what the device is doing, brightness follows the audio level.
 * In normal operation that reads as a VU meter; when something is wrong the
 * colour says what, without reaching for a serial console.
 *
 * Both halves come from measurements that already existed: the server's
 * post-crossover output peak (audio_i2s.c) and the client's output RMS
 * (audio_sink.c). Nothing is computed twice for the LED.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * Ordered by how much is working, which is also the order the colours run
 * through as a device comes up: red -> blue/orange -> green.
 */
typedef enum {
    STATUS_LED_BOOTING = 0,     /* white, dim   */
    STATUS_LED_PROVISIONING,    /* blue         */
    STATUS_LED_NO_NETWORK,      /* red          */
    STATUS_LED_NO_SERVER,       /* orange       */
    STATUS_LED_PLAYING,         /* green        */
    STATUS_LED_LOCAL_INPUT,     /* cyan         */
} status_led_state_t;

/*
 * Brings up the RMT channel and runs a red/green/blue self-test, which is
 * also how to tell whether CONFIG_SNAPSERVER_STATUS_LED_GPIO is right for a
 * given board: the three flashes appear, or the pin is wrong.
 *
 * Returns ESP_OK when the LED is disabled in Kconfig, so callers do not need
 * to care whether it is compiled in.
 */
esp_err_t status_led_start(void);

/* Latest state. Cheap and safe from any task; the LED task picks it up. */
void status_led_set_state(status_led_state_t state);

/*
 * Audio level in dBFS, -120 for silence. A fallback: both roles push their
 * samples through the same crossover stage, so the LED normally reads the
 * post-DSP peak directly and only falls back to this when that peak is
 * silent -- which is also the honest answer, since the peak is measured
 * after the DSP and this is measured before it.
 */
void status_led_set_level_db(float dbfs);
