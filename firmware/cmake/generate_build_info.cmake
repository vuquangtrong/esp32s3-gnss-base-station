find_package(Git QUIET)
set(GIT_COMMIT "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE _git_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _git_result
    )
    if(_git_result EQUAL 0 AND NOT "${_git_commit}" STREQUAL "")
        set(GIT_COMMIT "${_git_commit}")
    endif()
endif()

string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M:%S")

configure_file("${INPUT_FILE}" "${OUTPUT_FILE}" @ONLY)
