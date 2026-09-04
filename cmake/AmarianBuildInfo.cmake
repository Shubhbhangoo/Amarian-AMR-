# Captures build-time facts into a generated header.
#
# Deliberately tolerant: a source tarball with no .git directory must still
# configure and build. Missing git information degrades to "unknown" rather
# than failing, but is never guessed at.

function(amarian_configure_build_info)
    set(AMARIAN_GIT_COMMIT "unknown")
    set(AMARIAN_GIT_DIRTY 0)

    find_package(Git QUIET)
    if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE _commit
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _commit_result)
        if(_commit_result EQUAL 0 AND _commit)
            set(AMARIAN_GIT_COMMIT "${_commit}")
        endif()

        # A dirty tree must be visible in --version output. A node operator
        # reporting a consensus bug needs to know their binary does not
        # correspond to any published commit.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE _dirty
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _dirty_result)
        if(_dirty_result EQUAL 0 AND _dirty)
            set(AMARIAN_GIT_DIRTY 1)
        endif()
    endif()

    if(AMARIAN_HARDENING)
        set(AMARIAN_HARDENING_01 1)
    else()
        set(AMARIAN_HARDENING_01 0)
    endif()

    set(_out_dir "${CMAKE_BINARY_DIR}/generated/amarian")
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/build_config.hpp.in"
        "${_out_dir}/build_config.hpp"
        @ONLY)

    target_include_directories(amarian_settings INTERFACE
        "${CMAKE_BINARY_DIR}/generated")

    if(AMARIAN_GIT_DIRTY)
        message(STATUS "  git commit     : ${AMARIAN_GIT_COMMIT} (dirty tree)")
    else()
        message(STATUS "  git commit     : ${AMARIAN_GIT_COMMIT}")
    endif()
endfunction()
