# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)

execute_process(
    COMMAND python ${CMAKE_SOURCE_DIR}/gen_www_crc32.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

project(gnss_station
    VERSION 2.0
    DESCRIPTION "GNSS Station"
    LANGUAGES C CXX
)
