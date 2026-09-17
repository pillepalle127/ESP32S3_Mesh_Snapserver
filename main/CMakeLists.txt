idf_component_register(
    SRCS
        "app_main.c"
        "audio_i2s.c"
        "audio_opus.c"
        "device_config.c"
        "mesh_root.c"
        "provisioning.c"
        "snapserver.c"
		"snapcontrol.c" # <--
        "webconfig.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "webconfig_page.html"
    REQUIRES
        esp_timer
        esp_wifi
        esp_event
        esp_netif
        esp_driver_i2s
        esp_http_server
        lwip
        mdns
        nvs_flash
        esphome__micro-opus
        mesh_lite
        espressif__iot_bridge
		json
)
