set(srcs
    config.c
    status.c
    wifi.c
    server.c
    ublox.c
    parser.c
    logger.c
    uart.c
    main.c
)

idf_component_register(SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_http_server
        esp_driver_gpio
        esp_driver_uart
    WHOLE_ARCHIVE
)

string(TIMESTAMP BUILD_DATE "%d/%m/%Y")
git_describe(GIT_COMMIT ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PROJECT_VERSION="${CMAKE_PROJECT_VERSION}"
    BUILD_DATE="${BUILD_DATE}"
    GIT_COMMIT="${GIT_COMMIT}"
    ETAG_CACHE_MAX=4
)

set(WEB_SRC_DIR "${CMAKE_SOURCE_DIR}/www")
if(EXISTS ${WEB_SRC_DIR})
    littlefs_create_partition_image(www ${WEB_SRC_DIR} FLASH_IN_PROJECT)
else()
    message(FATAL_ERROR "${WEB_SRC_DIR} doesn't exist.")
endif()
