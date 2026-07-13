option(ESP32_MAC_OPCODE_PROFILER "Enable temporary 68K opcode profiling commands" OFF)
option(ESP32_MAC_HOT_OPCODE_IRAM "Place selected hot 68K opcode handlers in IRAM" ON)
option(ESP32_MAC_CACHE_ALIGN_CODE "Align generated code blocks to reduce cache-line sensitivity" ON)
option(ESP32_MAC_CACHE_ALIGN_BRANCHES "Also align jumps and loops; disabled by default due to Xtensa assembler issues" OFF)

idf_component_register(
    SRCS
        "../src/main.cpp"
        "../src/I2C_Driver.cpp"
        "../src/TCA9554PWR.cpp"
        "../src/Display_ST7701.cpp"
        "../src/disp.cpp"
        "../src/ble_input.cpp"
        "../src/input_host.cpp"
        "../src/sdcard.cpp"
        "../src/app_settings.cpp"
        "../src/storage_menu.cpp"
        "../src/storage_usb.cpp"
        "../src/storage_backend.cpp"
        "../src/console_shell.cpp"
        "../src/console_log.cpp"
        "../src/screenshot.cpp"
        "../src/pv_fdd.cpp"
        "../src/pv_sony_driver.cpp"
        "../src/hd.cpp"
        "../src/net_time.cpp"
        "../src/http_server.cpp"
        "../src/snd.cpp"
        "../src/snd_rmt.cpp"
        "../src/third_party/qrcode/qrcodegen.c"
        "../src/tme/emu.c"
        "../src/tme/emu_monitor.c"
        "../src/tme/vmu.c"
        "../src/tme/mmu.c"
        "../src/tme/rtc.c"
        "../src/tme/via.c"
        "../src/tme/scc.c"
        "../src/tme/iwm.c"
        "../src/tme/mouse.c"
        "../src/tme/ncr.c"
        "../src/tme/musashi/m68kcpu.c"
        "../src/tme/musashi/m68kops_runtime.c"
        "../src/tme/musashi/m68kopac.c"
        "../src/tme/musashi/m68kopdm.c"
        "../src/tme/musashi/m68kopnz.c"
    INCLUDE_DIRS
        "../src"
        "../src/tme"
        "../src/tme/musashi"
        "../src/third_party/qrcode"
    REQUIRES
        bt
        driver
        esp_driver_gpio
        esp_driver_gptimer
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_ledc
        esp_driver_rmt
        esp_driver_spi
        esp_driver_sdmmc
        esp_event
        esp_lcd
        esp_netif
        esp_http_server
        esp_wifi
        fatfs
        lwip
        mdns
        nvs_flash
        spi_flash
        espressif__esp_tinyusb
)

if(ESP32_MAC_OPCODE_PROFILER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESP32_MAC_OPCODE_PROFILER=1)
endif()

if(ESP32_MAC_HOT_OPCODE_IRAM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESP32_MAC_HOT_OPCODE_IRAM=1)
endif()

if(ESP32_MAC_CACHE_ALIGN_CODE)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-falign-functions=32>
        $<$<COMPILE_LANGUAGE:CXX>:-falign-functions=32>
    )
endif()

if(ESP32_MAC_CACHE_ALIGN_BRANCHES)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-falign-jumps=32>
        $<$<COMPILE_LANGUAGE:C>:-falign-loops=32>
        $<$<COMPILE_LANGUAGE:CXX>:-falign-jumps=32>
        $<$<COMPILE_LANGUAGE:CXX>:-falign-loops=32>
    )
endif()

if(DEFINED ESP32_MAC_EXTRA_C_FLAGS AND NOT ESP32_MAC_EXTRA_C_FLAGS STREQUAL "")
    separate_arguments(ESP32_MAC_EXTRA_C_FLAGS)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:${ESP32_MAC_EXTRA_C_FLAGS}>
    )
endif()

if(DEFINED ESP32_MAC_EXTRA_CXX_FLAGS AND NOT ESP32_MAC_EXTRA_CXX_FLAGS STREQUAL "")
    separate_arguments(ESP32_MAC_EXTRA_CXX_FLAGS)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${ESP32_MAC_EXTRA_CXX_FLAGS}>
    )
endif()
