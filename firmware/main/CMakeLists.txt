set(srcs
    ublox.c
    main.c
)

idf_component_register(SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES
        fatfs
        esp_adc
        esp_driver_gpio
        esp_driver_uart
        esp_driver_sdmmc
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
