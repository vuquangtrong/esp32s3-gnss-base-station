FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/main/*.c)

idf_component_register(
    SRCS ${app_sources}
    PRIV_REQUIRES nvs_flash
    PRIV_REQUIRES spiffs
    PRIV_REQUIRES fatfs
    PRIV_REQUIRES esp_driver_gpio
    PRIV_REQUIRES esp_driver_tsens
    PRIV_REQUIRES esp_driver_sdspi
    PRIV_REQUIRES esp_driver_uart
    PRIV_REQUIRES esp_app_format
    PRIV_REQUIRES esp_adc
    PRIV_REQUIRES esp_wifi
    PRIV_REQUIRES esp_http_server
    PRIV_REQUIRES esp_http_client
    INCLUDE_DIRS "."
)
