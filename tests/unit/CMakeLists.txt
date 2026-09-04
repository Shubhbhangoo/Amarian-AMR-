amarian_add_test(test_util unit
    util_args_test.cpp
    util_hex_test.cpp
    util_logging_test.cpp
    util_overflow_test.cpp
    util_serialize_test.cpp
)
target_link_libraries(test_util PRIVATE amarian::util)

amarian_add_test(test_crypto unit
    crypto_hash_test.cpp
)
target_link_libraries(test_crypto PRIVATE amarian::crypto)

amarian_add_test(test_version unit
    version_test.cpp
)
target_link_libraries(test_version PRIVATE amarian::version)
