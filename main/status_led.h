/**
 * @file status_led.h
 * @brief Single on-board WS2812 as a combined status indicator and level meter.
 *
 * While audio is playing the colour IS the meter -- green through yellow to
 * red, the way position works on a meter with more than one LED -- and the
 * LED stays steady. When there is no audio path there is no level to show,
 * so the colour carries the state instead and the blink rate carries its
 * severity. Steady therefore means nothing is wrong, without reaching for a
 * serial console.
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
    STATUS_LED_VOICE_ANNOUNCEMENT, /* level colour, fast pulse */
} status_led_state_t;

/*
 * Brings up the RMT channel on the LED pin from the pin assignment
 * (device_pins_t.status_led) and runs a red/green/blue self-test, which is
 * also how to tell whether that pin is right for a given board: the three
 * flashes appear, or the pin is wrong.
 *
 * Returns ESP_OK when the LED is disabled in Kconfig or no pin is
 * assigned, so callers do not need to care whether there is one.
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
