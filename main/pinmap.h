/**
 * @file pinmap.h
 * @brief Which GPIO may carry a signal on this module, independent of what.
 *
 * The pin assignment itself (I2S bus, status LED, knobs) is runtime
 * configuration, see device_pins_t / device_pots_t in device_config.h. This
 * is the hardware half of validating it: pins that do not exist, that the
 * module has already wired to flash, PSRAM, USB or the console, or that
 * are sampled at reset. Whether two functions want the same pin is not
 * checked here -- that is the caller's job, since it depends on the whole
 * assignment.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Highest GPIO number on the ESP32-S3. */
#define PINMAP_GPIO_MAX 48U

/*
 * NULL if gpio can carry a digital signal; otherwise a short reason why
 * not, for the config page to show. GPIO 0 is a strapping pin and so
 * always blocked, which is what lets 0 double as "no pin" in the config.
 */
const char *pinmap_blocked_reason(uint8_t gpio);

/* True for GPIO 1 to 10, the ADC1 pins that stay readable while Wi-Fi runs. */
bool pinmap_is_adc1(uint8_t gpio);
