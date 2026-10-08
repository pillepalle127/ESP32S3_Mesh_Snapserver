/**
 * @file power_button.h
 * @brief Push button that switches the device off and on (deep sleep).
 *
 * A momentary button from an RTC GPIO to GND, no other parts; the pin's
 * internal pull-up holds it high, in deep sleep too. Which GPIO, if any,
 * is device_pins_t.power_button, chosen on the web config page.
 *
 * Held for POWER_BUTTON_HOLD_MS while running, it switches the device off:
 * LED dark, I2S output held at silence, then deep sleep once the button is
 * let go. Any press wakes the chip, but it only switches on if the button
 * stays down for POWER_BUTTON_WAKE_HOLD_MS; let go earlier, it goes
 * straight back to sleep. A short press while running does nothing.
 *
 * Deep sleep only stops the ESP. Whatever else hangs on the battery -- the
 * board's regulator, DAC, DSP, amplifier -- keeps drawing current.
 */
#pragma once

#include "esp_err.h"

#include "device_config.h"

#define POWER_BUTTON_HOLD_MS 2000U
#define POWER_BUTTON_WAKE_HOLD_MS 1000U

/*
 * After a wake-up by the button: back to deep sleep unless it is held for
 * POWER_BUTTON_WAKE_HOLD_MS (blocks that long). Then releases the pins the
 * last switch-off held at silence. Call first in app_main, before NVS is
 * touched or anything claims a pin.
 */
void power_button_boot(void);

/*
 * Starts watching the button on pins->power_button. ESP_ERR_NOT_SUPPORTED
 * when no pin is assigned. pins is the set the audio and LED were started
 * with; those are the pins held at silence on switch-off.
 */
esp_err_t power_button_start(const device_pins_t *pins);
