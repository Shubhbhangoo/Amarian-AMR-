amarian_add_test(test_util unit
    util_args_test.cpp
    util_hex_test.cpp
    util_logging_test.cpp
    util_overflow_test.cpp
)
target_link_libraries(test_util PRIVATE amarian::util)
