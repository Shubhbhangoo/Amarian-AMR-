# Warning policy for Amarian.
#
# Consensus code is compiled with an aggressive warning set on both GCC and Clang.
# Anything that can silently change a numeric value or leave memory uninitialised
# is an error, not a warning.

function(amarian_apply_warnings target)
    if(MSVC)
        set(msvc_flags /W4 /permissive-)
        target_compile_definitions(${target} INTERFACE _CRT_SECURE_NO_WARNINGS)
        if(AMARIAN_WERROR)
            list(APPEND msvc_flags /WX)
        endif()
        target_compile_options(${target} INTERFACE ${msvc_flags})
        return()
    endif()

    set(common_flags
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wcast-qual
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        -Wextra-semi
        -Wundef
        -Wswitch-enum
        -Wfloat-equal
        -Wpointer-arith
        -Wwrite-strings
        -Wredundant-decls
        -Wmissing-declarations
        -Wdate-time
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        list(APPEND common_flags
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wuseless-cast
            -Wsuggest-override
            -Warith-conversion
            -Wstrict-null-sentinel
            -Wtrampolines
            -Wvla
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(APPEND common_flags
            -Wloop-analysis
            -Wrange-loop-analysis
            -Wshadow-all
            -Wthread-safety
            -Wunreachable-code-aggressive
            -Wvla
            -Wno-c++98-compat
            -Wno-c++98-compat-pedantic
        )
    endif()

    if(AMARIAN_WERROR)
        list(APPEND common_flags -Werror)
        # Deprecations from third-party headers must not break the build.
        list(APPEND common_flags -Wno-error=deprecated-declarations)
    endif()

    target_compile_options(${target} INTERFACE ${common_flags})
endfunction()
