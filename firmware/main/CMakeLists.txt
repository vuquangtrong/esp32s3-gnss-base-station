set(srcs
    config.c
    status.c
    storage.c
    battery.c
    wifi.c
    ublox.c
    gnss.c
    parser.c
    uart.c
    sdcard.c
    logger.c
    server.c
    ntrip_client.c
    ntrip_server.c
    firmware_update.c
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
        esp_http_client
        app_update
        esp_partition
    WHOLE_ARCHIVE
)

set(BUILD_INFO_HEADER "${CMAKE_CURRENT_BINARY_DIR}/build_info.h")

add_custom_target(gen_build_info ALL
    COMMAND ${CMAKE_COMMAND} -DOUT=${BUILD_INFO_HEADER} -DREPO_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/gen_build_info.cmake
    BYPRODUCTS ${BUILD_INFO_HEADER}
    COMMENT "Generating build_info.h"
    VERBATIM
)

add_dependencies(${COMPONENT_LIB} gen_build_info)
target_include_directories(${COMPONENT_LIB} PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    VERSION="${CMAKE_PROJECT_VERSION}"
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
