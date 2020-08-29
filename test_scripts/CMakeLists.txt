add_library(chatra_language_test
        Test.cpp
        TestNative.cpp
        test_native.cha
        )

target_link_libraries(chatra_language_test INTERFACE chatra_core)
target_include_directories(chatra_language_test PRIVATE ${PROJECT_SOURCE_DIR}/chatra)
