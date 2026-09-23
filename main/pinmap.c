/**
 * @file pinmap.c
 * @brief Hardware limits on which GPIO may be assigned to a function.
 */
#include "pinmap.h"

#include <stddef.h>

#include "sdkconfig.h"

#define ADC1_FIRST_GPIO 1U
#define ADC1_LAST_GPIO  10U

const char *pinmap_blocked_reason(uint8_t gpio)
{
    if (gpio > PINMAP_GPIO_MAX || (gpio >= 22U && gpio <= 25U)) {
        return "does not exist";
    }
    /*
     * Sampled at reset: 0 and 46 select the boot mode, 3 the JTAG source,
     * 45 the flash supply voltage. Whatever hangs on them decides whether
     * the next boot works, so none of them is offered.
     */
    if (gpio == 0U || gpio == 3U || gpio == 45U || gpio == 46U) {
        return "strapping pin";
    }
    /* SPI flash, plus the PSRAM chip select on 26. */
    if (gpio >= 26U && gpio <= 32U) {
        return "SPI flash/PSRAM";
    }
#if CONFIG_SPIRAM_MODE_OCT
    if (gpio >= 33U && gpio <= 37U) {
        return "octal PSRAM";
    }
#endif
#if CONFIG_USJ_ENABLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
    if (gpio == 19U || gpio == 20U) {
        return "USB";
    }
#endif
#if CONFIG_ESP_CONSOLE_UART && (CONFIG_ESP_CONSOLE_UART_NUM == 0)
    if (gpio == 43U || gpio == 44U) {
        return "UART0 console";
    }
#endif
    return NULL;
}

bool pinmap_is_adc1(uint8_t gpio)
{
    return gpio >= ADC1_FIRST_GPIO && gpio <= ADC1_LAST_GPIO;
}
