# Sanitizer wiring for Amarian.
#
# Sanitizer builds are first-class: the adversarial test suite in tests/ is meant to
# be run under ASan+UBSan, and the fuzz targets under ASan+UBSan with libFuzzer.

function(amarian_apply_sanitizers target)
    if(AMARIAN_SANITIZER STREQUAL "")
        return()
    endif()

    set(san_list "")
    if(AMARIAN_SANITIZER MATCHES "address")
        list(APPEND san_list address)
    endif()
    if(AMARIAN_SANITIZER MATCHES "undefined")
        list(APPEND san_list undefined)
    endif()
    if(AMARIAN_SANITIZER MATCHES "thread")
        list(APPEND san_list thread)
    endif()
    if(AMARIAN_SANITIZER MATCHES "memory")
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "MemorySanitizer requires clang")
        endif()
        list(APPEND san_list memory)
    endif()

    if(san_list STREQUAL "")
        message(FATAL_ERROR "AMARIAN_SANITIZER='${AMARIAN_SANITIZER}' matched no known sanitizer")
    endif()
    if("thread" IN_LIST san_list AND "address" IN_LIST san_list)
        message(FATAL_ERROR "ThreadSanitizer and AddressSanitizer are mutually exclusive")
    endif()

    list(JOIN san_list "," san_arg)
    set(san_flags -fsanitize=${san_arg} -fno-omit-frame-pointer -fno-optimize-sibling-calls -g)

    if("undefined" IN_LIST san_list)
        # Trap on the UB classes that matter for a validator instead of only reporting.
        list(APPEND san_flags -fno-sanitize-recover=undefined)
        list(APPEND san_flags -fsanitize=float-divide-by-zero)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            list(APPEND san_flags -fsanitize=integer -fno-sanitize=unsigned-integer-overflow
                                  -fno-sanitize=unsigned-shift-base)
        endif()
    endif()

    # -fwrapv (from hardening) would hide signed-overflow UB from UBSan.
    target_compile_options(${target} INTERFACE ${san_flags} -fno-wrapv)
    target_link_options(${target} INTERFACE -fsanitize=${san_arg})
    target_compile_definitions(${target} INTERFACE AMARIAN_SANITIZER_BUILD=1)

    message(STATUS "Sanitizers enabled: ${san_arg}")
endfunction()
