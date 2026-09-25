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
 * The level comes from measurements that already existed: the peak the
 * server's DSP stage records and the peak the client's player pushes, both
 * before the volume (see status_led_set_level_db()).
 *
 * What is shown is decided from two inputs, so that no task can overwrite
 * another's state: the connection state (status_led_set_state()) and the
 * activity on top of it (status_led_set_activity()). Priority, highest
 * first: provisioning, voice announcement, local input, connection state.
 * Playing the local input without a server therefore shows the meter, not
 * the "no server" blink.
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

/*
 * Connection state: BOOTING, PROVISIONING, NO_NETWORK, NO_SERVER or
 * PLAYING (connected). LOCAL_INPUT and VOICE_ANNOUNCEMENT are only ever
 * shown, never set here -- they come from status_led_set_activity().
 * Cheap and safe from any task; the LED task picks it up.
 */
void status_led_set_state(status_led_state_t state);

typedef enum {
    STATUS_LED_ACTIVITY_NONE = 0,
    STATUS_LED_ACTIVITY_LOCAL_INPUT,   /* shown as STATUS_LED_LOCAL_INPUT */
    STATUS_LED_ACTIVITY_VOICE,         /* shown as STATUS_LED_VOICE_ANNOUNCEMENT */
} status_led_activity_t;

/* What the audio path is doing; outranks the connection state. */
void status_led_set_activity(status_led_activity_t activity);

/*
 * Peak level in dBFS before the volume, -120 for silence. The client pushes
 * it once per frame; the server's level comes from its DSP stage instead
 * (audio_i2s_take_led_peak()). The LED shows the larger of the two.
 */
void status_led_set_level_db(float dbfs);
