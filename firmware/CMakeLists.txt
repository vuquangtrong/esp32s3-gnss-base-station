cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3-gnss)

# Generate CRC32 checksums for the files in data partition
add_custom_target(prebuild
    COMMAND python ${CMAKE_SOURCE_DIR}/scripts/gen_data_crc32.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# ESP-IDF internally defines the target app, so add prebuild as a dependency
add_dependencies(app prebuild)

# Build the SPIFFS image from the www directory and include it in the project
spiffs_create_partition_image(www data FLASH_IN_PROJECT)
