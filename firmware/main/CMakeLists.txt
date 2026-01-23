FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/main/*.c)

idf_component_register(
    SRCS ${app_sources}
    PRIV_REQUIRES esp_driver_gpio
    PRIV_REQUIRES esp_driver_tsens
    PRIV_REQUIRES esp_driver_sdspi
    PRIV_REQUIRES esp_driver_uart
    PRIV_REQUIRES esp_adc
    PRIV_REQUIRES fatfs
    INCLUDE_DIRS "."
)
