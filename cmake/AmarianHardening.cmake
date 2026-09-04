# Hardening policy for Amarian.
#
# A validating node parses hostile data from the network on every block and
# transaction, so hardening stays on in release builds. _GLIBCXX_ASSERTIONS turns
# out-of-range container access into a controlled abort instead of silent memory
# corruption; the cost is a few percent and is worth paying for consensus code.

function(amarian_apply_hardening target)
    if(NOT AMARIAN_HARDENING)
        return()
    endif()

    set(hardening_compile
        -fstack-protector-strong
        -fno-strict-aliasing
        -fno-delete-null-pointer-checks
        -fwrapv
    )
    set(hardening_defines
        _GLIBCXX_ASSERTIONS
        _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE
    )
    set(hardening_link "")

    # _FORTIFY_SOURCE requires optimisation; enabling it at -O0 only emits a warning.
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND hardening_defines _FORTIFY_SOURCE=3)
    endif()

    include(CheckCXXCompilerFlag)
    foreach(flag -fstack-clash-protection -fcf-protection=full)
        string(MAKE_C_IDENTIFIER "HAVE${flag}" flag_var)
        check_cxx_compiler_flag(${flag} ${flag_var})
        if(${flag_var})
            list(APPEND hardening_compile ${flag})
        endif()
    endforeach()

    if(UNIX AND NOT APPLE)
        list(APPEND hardening_link -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
    endif()

    # Sanitizers replace these guards and conflict with some of them.
    if(AMARIAN_SANITIZER STREQUAL "")
        target_compile_options(${target} INTERFACE ${hardening_compile})
        target_link_options(${target} INTERFACE ${hardening_link})
    endif()
    target_compile_definitions(${target} INTERFACE ${hardening_defines})
endfunction()
