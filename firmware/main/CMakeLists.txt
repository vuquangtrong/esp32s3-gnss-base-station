set(srcs
    config.c
    status.c
    battery.c
    wifi.c
    main.c
)

idf_component_register(SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_adc
        esp_wifi
    WHOLE_ARCHIVE
)

git_describe(GIT_COMMIT ${CMAKE_CURRENT_SOURCE_DIR})
string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M:%S")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    VERSION="${CMAKE_PROJECT_VERSION}"
    GIT_COMMIT="${GIT_COMMIT}"
    BUILD_TIME="${BUILD_TIME}"
)
