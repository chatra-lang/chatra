add_library(chatra_language_test
        Test.cpp
        TestNative.cpp
        test_native.cha
        )

target_link_libraries(chatra_language_test chatra_core)
