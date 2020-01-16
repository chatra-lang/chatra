cmake_minimum_required(VERSION 3.5)
project(Chatra CXX)

set(CMAKE_CXX_STANDARD 11)
find_package(Threads REQUIRED)

# Compiler options
# These are used for keeping healthiness of Chatra's code, and are not required when building your project.
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
set(CMAKE_CXX_FLAGS "-Wall -W \
-Weverything \
-Wno-missing-prototypes \
-Wno-c++98-compat-pedantic -Wno-sign-conversion -Wno-float-equal -Wno-padded \
-Wno-shadow -Wno-shadow-field-in-constructor -Wno-shadow-uncaptured-local \
-Wno-global-constructors -Wno-exit-time-destructors \
-Wno-switch-enum -Wno-weak-vtables")
endif()

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
set(CMAKE_CXX_FLAGS "-pedantic -Wall -Wextra \
-Wcast-align -Wcast-qual -Wctor-dtor-privacy -Wdisabled-optimization -Wformat=2 \
-Winit-self -Wlogical-op -Wmissing-include-dirs -Wnoexcept \
-Wold-style-cast -Woverloaded-virtual -Wredundant-decls \
-Wsign-promo -Wstrict-null-sentinel -Wstrict-overflow=5 -Wundef \
-Wno-shadow -Wno-return-type -Wno-switch")
endif()

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
set(CMAKE_CXX_FLAGS "/GR /EHsc /source-charset:utf-8 /W4 /wd4458 /wd4063 /wd4996")
endif()

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address")
endif()

# Adds interpreter
add_subdirectory(chatra_core)
# Adds embedded libraries
add_subdirectory(chatra_emb)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CHATRA_ENABLE_LANGUAGE_TESTS "true")
else()
    set(CHATRA_ENABLE_LANGUAGE_TESTS "false")
endif()

if (CHATRA_ENABLE_LANGUAGE_TESTS)
    add_definitions(-DCHATRA_ENABLE_LANGUAGE_TESTS)
    add_subdirectory(test_scripts)
endif()

add_executable(chatra main.cpp)

if (CHATRA_ENABLE_LANGUAGE_TESTS)
    target_link_libraries(chatra chatra_core chatra_emb chatra_language_test ${CMAKE_THREAD_LIBS_INIT})
else()
    target_link_libraries(chatra chatra_core chatra_emb ${CMAKE_THREAD_LIBS_INIT})
endif()

if (CHATRA_ENABLE_LANGUAGE_TESTS)
	enable_testing()
	add_test(NAME language_test CONFIGURATIONS Debug 
		WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} 
		COMMAND $<TARGET_FILE:chatra> "--language-test" "--baseline"
		)
endif()
