set(srcs
    config.c
    status.c
    storage.c
    sdcard.c
    fw_update.c
    ublox.c
    uart.c
    gnss.c
    battery.c
    factory_test.c
    wifi.c
    main.c
)

idf_component_register(SRCS ${srcs}
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        vfs
        fatfs
        app_update
        mbedtls
        esp_driver_gpio
        esp_driver_uart
        esp_adc
        esp_wifi
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(build_info_template "${CMAKE_SOURCE_DIR}/cmake/build_info.h.in")
    set(build_info_script "${CMAKE_SOURCE_DIR}/cmake/generate_build_info.cmake")
    set(build_info_header "${CMAKE_CURRENT_BINARY_DIR}/build_info.h")

    set(app_version "${CMAKE_PROJECT_VERSION}")
    if("${app_version}" STREQUAL "")
        idf_build_get_property(app_version PROJECT_VER)
    endif()

    add_custom_target(generate_build_info ALL
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP_VERSION=${app_version}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DINPUT_FILE=${build_info_template}"
            "-DOUTPUT_FILE=${build_info_header}"
            -P "${build_info_script}"
        BYPRODUCTS "${build_info_header}"
        COMMENT "Generating build_info.h"
        VERBATIM
    )

    add_dependencies(${COMPONENT_LIB} generate_build_info)
    target_include_directories(${COMPONENT_LIB} PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
endif()

set(WEB_SRC_DIR "${CMAKE_SOURCE_DIR}/www")
if(EXISTS ${WEB_SRC_DIR})
    littlefs_create_partition_image(www_0 ${WEB_SRC_DIR} FLASH_IN_PROJECT)
else()
    message(FATAL_ERROR "${WEB_SRC_DIR} doesn't exist.")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FACTORY_TEST=0
)
