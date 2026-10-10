# Empty stand-in for espressif/esp_tinyusb.
#
# iot_bridge requires esp_tinyusb on the S3 unconditionally, but only uses it
# for USB networking (CONFIG_BRIDGE_DATA_FORWARDING_NETIF_USB, off here).
# The real component injects its own tusb_config.h into TinyUSB, which has
# no audio class and wins over ours (main/tusb/tusb_config.h), and it brings
# descriptor callbacks that clash with main/usb_audio.c. A project component
# of the same name overrides the managed one. Keeps the public requirements
# iot_bridge relies on (bridge_eth.c needs driver/gpio.h through them).
idf_component_register(REQUIRES fatfs vfs esp_driver_gpio)
