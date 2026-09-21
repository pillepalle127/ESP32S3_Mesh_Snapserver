/**
 * @file volume_pot.h
 * @brief Volume knob on an ADC pin, for this device's own speaker.
 *
 * A 10 kOhm potentiometer wired as a divider -- one end to 3V3, the other
 * to GND, wiper to the configured GPIO. The reading is turned into a gain
 * and handed to audio_i2s_set_master_volume(), which applies it in the
 * output stage only. The Opus stream a server sends to its clients is fed
 * from a separate copy and is deliberately not affected, so the knob on one
 * box never changes what the others hear.
 *
 * With no potentiometer connected the pin is held high by an internal
 * pull-up and reads full scale, i.e. full volume. That is the intended
 * behaviour for boards that have no knob fitted: they simply play at 100 %
 * and need no configuration.
 */
#pragma once

#include "esp_err.h"

/*
 * Starts the ADC and the polling task. Returns ESP_ERR_NOT_SUPPORTED when
 * the feature is disabled in Kconfig, which the caller may ignore -- the
 * output stage then just stays at the full volume it defaults to.
 */
esp_err_t volume_pot_start(void);

/*
 * Knob position as a percentage, for the status line. Returns -1 while the
 * feature is disabled or before the first reading has been taken.
 */
int volume_pot_percent(void);
