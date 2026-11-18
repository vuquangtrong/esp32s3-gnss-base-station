string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M:%S")

execute_process(
    COMMAND git -C "${REPO_DIR}" describe --always --tags --dirty
    RESULT_VARIABLE _git_res
    OUTPUT_VARIABLE GIT_COMMIT
    ERROR_VARIABLE _git_err
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _git_res EQUAL 0 OR GIT_COMMIT STREQUAL "")
    set(GIT_COMMIT "unknown")
endif()

set(_content "#pragma once\n\n#define BUILD_TIME \"${BUILD_TIME}\"\n#define GIT_COMMIT \"${GIT_COMMIT}\"\n")
file(WRITE "${OUT}.tmp" "${_content}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OUT}.tmp" "${OUT}")
file(REMOVE "${OUT}.tmp")
