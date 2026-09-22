/**
 * @file pots.h
 * @brief Two potentiometer inputs on ADC1: volume and delay.
 *
 * Each knob is a 10 kOhm potentiometer wired as a divider -- one end to
 * 3V3, the other to GND, wiper to a GPIO. Which GPIO, if any, is part of
 * the runtime configuration (device_config_get_pots()) and chosen on the
 * web config page; only pins pots_pin_blocked_reason() accepts are offered.
 *
 * Volume sets this device's own speaker through audio_i2s_set_master_volume()
 * and nothing else -- the Opus stream a server sends is fed from a separate
 * copy. Its pin has a pull-up, so a pin that is selected but has no knob
 * fitted reads full scale and plays at 100 %.
 *
 * Delay replaces the delay_trim_ms field while its pin is set: centre is
 * 0 ms, the ends are -/+ the configured range. Its pin has a pull-down, so
 * an unplugged knob sits at the negative end rather than wandering. There
 * is no way to make an open input read "centre" with internal pulls, which
 * is why the delay knob defaults to no pin at all.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "device_config.h"
#include "esp_err.h"

/*
 * NULL if gpio can carry a knob; otherwise a short reason why not, for the
 * config page to show. Only checks the hardware -- whether the other knob
 * already has the pin is up to the caller.
 */
const char *pots_pin_blocked_reason(uint8_t gpio);

/*
 * Opens ADC1 for whichever pins are set and starts the polling task.
 * Returns ESP_ERR_NOT_SUPPORTED when the feature is compiled out; the
 * output stage then stays at full volume and delay_trim_ms applies as
 * configured. Pins take effect only here, so changing one needs a reboot.
 */
esp_err_t pots_start(const device_pots_t *cfg);

/*
 * Arms the delay knob. Called by app_main once the role's own delay setup
 * is done, so a first reading cannot race ahead of it and be overwritten.
 * Until then the knob is read but not applied.
 */
void pots_enable_delay(uint8_t role, uint16_t buffer_ms);

/* Range of the delay knob, applied live. */
void pots_set_delay_range(uint16_t range_ms);

/* True while a delay pin is set and running -- delay_trim_ms is ignored then. */
bool pots_delay_active(void);

/* Volume knob position in percent, -1 if no volume pin is running. */
int pots_volume_percent(void);

/* Current delay from the knob; false if no delay pin is running. */
bool pots_delay_ms(int16_t *out);
