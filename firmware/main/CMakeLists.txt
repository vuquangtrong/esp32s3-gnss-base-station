set(srcs
    config.c
    status.c
    battery.c
    wifi.c
    ublox.c
    gnss.c
    parser.c
    uart.c
    sdcard.c
    logger.c
    server.c
    main.c
)

idf_component_register(SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        fatfs
        esp_adc
        esp_wifi
        esp_driver_gpio
        esp_driver_uart
        esp_http_server
    WHOLE_ARCHIVE
)

git_describe(GIT_COMMIT ${CMAKE_CURRENT_SOURCE_DIR})
string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M:%S")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    VERSION="${CMAKE_PROJECT_VERSION}"
    GIT_COMMIT="${GIT_COMMIT}"
    BUILD_TIME="${BUILD_TIME}"
    ETAG_CACHE_MAX=4
)

if(NOT CONFIG_SOC_SDMMC_HOST_SUPPORTED)
    message(FATAL_ERROR "SDMMC host must be supported on this target")
endif()

set(WEB_SRC_DIR "${CMAKE_SOURCE_DIR}/www")
if(EXISTS ${WEB_SRC_DIR})
    littlefs_create_partition_image(www ${WEB_SRC_DIR} FLASH_IN_PROJECT)
else()
    message(FATAL_ERROR "${WEB_SRC_DIR} doesn't exist.")
endif()
