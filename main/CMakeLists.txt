idf_component_register(
    SRCS
        "app_main.c"
        "audio_i2s.c"
        "audio_opus.c"
        "audio_resample.c"
        "audio_sink.c"
        "client_store.c"
        "cpu_stats.c"
        "device_config.c"
        "mesh_client.c"
        "mesh_root.c"
        "pinmap.c"
        "provisioning.c"
        "snapclient.c"
        "snapserver.c"
        "status_led.c"
		"snapcontrol.c" # <--
        "pots.c"
        "power_button.c"
        "voice_announce.c"
        "webconfig.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_timer
        esp_wifi
        esp_event
        esp_netif
        esp_adc
        esp_driver_gpio
        esp_driver_i2s
        esp_driver_rmt
        esp_http_server
        lwip
        mdns
        nvs_flash
        esphome__micro-opus
        mesh_lite
        espressif__iot_bridge
		json
)

# The config page goes out gzip-compressed (49 KB -> 14 KB). Sending it
# uncompressed took ~300 ms per load and slowed the server's capture loop
# enough to lose ~50 ms of audio each time (2026-10-01).
idf_build_get_property(python PYTHON)
set(page_gz "${CMAKE_CURRENT_BINARY_DIR}/webconfig_page.html.gz")
add_custom_command(
    OUTPUT "${page_gz}"
    COMMAND "${python}" -c
        "import gzip,sys; d=open(sys.argv[1],'rb').read(); open(sys.argv[2],'wb').write(gzip.compress(d,9,mtime=0))"
        "${CMAKE_CURRENT_SOURCE_DIR}/webconfig_page.html" "${page_gz}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/webconfig_page.html"
    VERBATIM)
add_custom_target(webconfig_page_gz DEPENDS "${page_gz}")
add_dependencies(${COMPONENT_LIB} webconfig_page_gz)
target_add_binary_data(${COMPONENT_LIB} "${page_gz}" BINARY)
