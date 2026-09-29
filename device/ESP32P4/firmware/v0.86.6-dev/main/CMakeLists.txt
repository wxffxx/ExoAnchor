set(SI_MAIN_SRCS
        "app/app_main.c"
        "adapters/hid_json.c"
        "adapters/http_api.c"
        "application/agent_api_settings.c"
        "application/agent_tools_settings.c"
        "application/auth_service.c"
        "application/boot_key_manager.c"
        "application/control_lease.c"
        "application/console_credentials.c"
        "application/device_settings.c"
        "application/diagnostics_service.c"
        "application/device_observation_service.c"
        "application/host_display_manager.c"
        "application/network_settings.c"
        "application/target_profile_settings.c"
        "application/video_control.c"
        "application/video_stream_metrics.c"
        "core/action_event.c"
        "core/authorization.c"
        "core/boot_key_sequence.c"
        "core/device_observation_utils.c"
        "core/discovery_rate_limit.c"
        "core/host_display_mode.c"
        "core/hid_abort_fence.c"
        "core/hid_owner.c"
        "core/hid_ascii.c"
        "core/network_config.c"
        "core/product_identity.c"
        "core/relay_protocol.c"
        "core/resource_operation_state.c"
        "core/storage_path.c"
        "core/terminal_control_state.c"
        "core/time_utils.c"
        "core/utf8_utils.c"
        "core/version_utils.c"
        "drivers/hid_device.c"
        "drivers/net_manager.c"
        "drivers/power_control.c"
        "drivers/storage_manager.c"
        "drivers/target_uart.c"
        "drivers/video_frame_store.c"
        "drivers/video_input.c"
        "drivers/video_mjpeg.c"
        "infrastructure/secret_store.c"
        "infrastructure/settings_schema.c"
        "infrastructure/settings_store.c"
        "services/diag_cli.c"
        "services/boot_key_http.c"
        "services/device_http.c"
        "services/host_display_http.c"
        "services/network_http.c"
        "services/network_discovery.c"
        "services/ssh_client.c"
        "services/ssh_hostkey_store.c"
        "services/status_ws.c"
        "services/web_server.c"
)

# Keep the destructive MS2109 EEPROM implementation out of normal product
# images.  The V2.4 validation board is the only composition that enables the
# test driver; production power control and the EEPROM emulator remain separate
# opt-in components.
if(CONFIG_SI_MS2109_EEPROM_EMULATOR_ENABLE)
    list(APPEND SI_MAIN_SRCS "drivers/ms2109_eeprom_emulator.c")
endif()

if(CONFIG_SI_MS2109_POWER_ENABLE)
    list(APPEND SI_MAIN_SRCS "drivers/ms2109_power.c")
endif()

if(CONFIG_SI_MS2109_TEST_ENABLE)
    list(APPEND SI_MAIN_SRCS "drivers/ms2109_test.c")
endif()

if(CONFIG_SI_EMBEDDED_AGENT)
    list(APPEND SI_MAIN_SRCS
        "adapters/agent_http_routes.c"
        "application/agent_prompt_service.c"
        "application/agent_request_broker.c"
        "application/agent_skill_service.c"
        "application/agent_task_service.c"
        "core/agent_execution_guard.c"
        "core/agent_page_context_checkpoint.c"
        "core/agent_router.c"
        "core/agent_task_runtime.c"
        "core/agent_verifier.c"
        "core/model_provider.c"
        "infrastructure/agent_event_store.c"
        "infrastructure/agent_history_writer.c"
        "infrastructure/agent_repository.c"
    )
endif()

if(CONFIG_SI_VIDEO_H264_EXPERIMENT)
    list(APPEND SI_MAIN_SRCS
        "services/video_h264_stream.c"
    )
endif()

set(SI_MAIN_REQUIRES
        app_update
        esp_app_format
        esp_eth
        esp_event
        esp_driver_gpio
        esp_driver_i2c
        esp_hw_support
        esp_http_client
        esp_http_server
        esp_driver_jpeg
        esp_driver_sdmmc
        esp_driver_uart
        esp_netif
        esp_partition
        esp_psram
        esp_ringbuf
        esp_tinyusb
        esp_timer
        esp-tls
        fatfs
        json
        mbedtls
        espressif__mdns
        nvs_flash
        sdmmc
        spi_flash
        skuodi__libssh2_esp
        usb
        usb_host_uvc)

if(CONFIG_SI_VIDEO_H264_EXPERIMENT)
    list(APPEND SI_MAIN_REQUIRES espressif__esp_h264)
endif()

set(SI_WEB_ASSETS
        "www/index.html"
        "www/kvm.html"
        "www/assets/ui-core.css"
        "www/assets/ui-core.js"
        "www/assets/ui-shell.css"
        "www/assets/ui-shell.js"
        "www/assets/exoanchor-ui-mark.svg"
        "www/assets/favicon.svg"
        "www/settings.html")

if(CONFIG_SI_UART_TERMINAL)
    list(APPEND SI_WEB_ASSETS
        "www/terminal.html"
        "www/vendor/xterm/xterm.css"
        "www/vendor/xterm/xterm.js"
        "www/vendor/xterm/addon-fit.js")
endif()

idf_component_register(
    SRCS
        ${SI_MAIN_SRCS}
    INCLUDE_DIRS
        "adapters"
        "application"
        "config"
        "core"
        "drivers"
        "infrastructure"
        "services"
    PRIV_INCLUDE_DIRS
        "${IDF_PATH}/components/esp_driver_i2c"
    EMBED_TXTFILES
        ${SI_WEB_ASSETS}
    REQUIRES
        ${SI_MAIN_REQUIRES}
)

# Keep every firmware-facing version surface aligned with the ESP-IDF app
# descriptor. The build profile supplies PROJECT_VER (dev or Stable).
target_compile_definitions(
    ${COMPONENT_LIB}
    PUBLIC SI_BMC_VERSION="${PROJECT_VER}"
)

if(CONFIG_SI_EMBEDDED_AGENT)
    target_add_binary_data(
        ${COMPONENT_LIB}
        "${CMAKE_CURRENT_LIST_DIR}/www/agent.html"
        TEXT
        RENAME_TO agent_html
    )

    # Every Skill uses the required SKILL.md filename. Copy them to unique
    # build basenames so ESP-IDF does not generate colliding SKILL.md.S
    # outputs or linker symbols.
    set(SI_AGENT_SKILL_EMBED_DIR "${CMAKE_CURRENT_BINARY_DIR}/agent_skill_embed")
    file(MAKE_DIRECTORY "${SI_AGENT_SKILL_EMBED_DIR}")
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/agent_skills/configure-uart-cli/SKILL.md"
        "${SI_AGENT_SKILL_EMBED_DIR}/configure_uart_cli_skill.md"
        COPYONLY
    )
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/agent_skills/configure-access-via-kvm/SKILL.md"
        "${SI_AGENT_SKILL_EMBED_DIR}/configure_access_via_kvm_skill.md"
        COPYONLY
    )
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/agent_skills/bridge-uart-ssh-access/SKILL.md"
        "${SI_AGENT_SKILL_EMBED_DIR}/bridge_uart_ssh_access_skill.md"
        COPYONLY
    )
    target_add_binary_data(
        ${COMPONENT_LIB}
        "${SI_AGENT_SKILL_EMBED_DIR}/configure_uart_cli_skill.md"
        TEXT
        RENAME_TO configure_uart_cli_skill
    )
    target_add_binary_data(
        ${COMPONENT_LIB}
        "${SI_AGENT_SKILL_EMBED_DIR}/configure_access_via_kvm_skill.md"
        TEXT
        RENAME_TO configure_access_via_kvm_skill
    )
    target_add_binary_data(
        ${COMPONENT_LIB}
        "${SI_AGENT_SKILL_EMBED_DIR}/bridge_uart_ssh_access_skill.md"
        TEXT
        RENAME_TO bridge_uart_ssh_access_skill
    )
endif()
