set(SI_MAIN_SRCS
        "app_main.c"
        "hid_device.c"
        "net_manager.c"
        "video_input.c"
        "web_server.c")

set(SI_MAIN_REQUIRES
        app_update
        esp_eth
        esp_event
        esp_hw_support
        esp_http_client
        esp_http_server
        esp_driver_jpeg
        esp_netif
        esp_psram
        esp_tinyusb
        esp_timer
        esp-tls
        json
        mbedtls
        nvs_flash
        spi_flash
        usb
        usb_host_uvc)

idf_component_register(
    SRCS
        ${SI_MAIN_SRCS}
    INCLUDE_DIRS "."
    EMBED_TXTFILES
        "www/index.html"
        "www/kvm.html"
        "www/settings.html"
    REQUIRES
        ${SI_MAIN_REQUIRES}
)
