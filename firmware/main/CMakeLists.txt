set(srcs
    "config.c"
    "main.c"
)

idf_component_register(SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
    WHOLE_ARCHIVE
)

git_describe(GIT_COMMIT ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PROJECT_VERSION="${CMAKE_PROJECT_VERSION}"
    GIT_COMMIT="${GIT_COMMIT}"
)
