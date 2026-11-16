cmake_minimum_required(VERSION 3.16)

project(http_caching_proxy
    VERSION 0.1.0
    DESCRIPTION "HTTP/1.1 caching proxy server built on Boost.Asio"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(PROXY_BUILD_TESTS "Build unit tests" ON)
option(PROXY_ENABLE_SANITIZERS "Build with AddressSanitizer and UBSan" OFF)

find_package(Threads REQUIRED)
find_package(Boost 1.70 REQUIRED)

# Core library: everything except main(), so tests can link against it.
add_library(proxy_core
    src/util/logger.cpp
    src/http/message.cpp
    src/http/parser.cpp
    src/http/url.cpp
    src/http/date.cpp
    src/cache/cache_control.cpp
    src/cache/http_cache.cpp
    src/proxy/config.cpp
    src/proxy/upstream.cpp
    src/proxy/tunnel.cpp
    src/proxy/session.cpp
    src/proxy/server.cpp
)

target_include_directories(proxy_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(proxy_core PUBLIC Boost::boost Threads::Threads)
target_compile_definitions(proxy_core PUBLIC BOOST_ASIO_NO_DEPRECATED)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(proxy_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

if(PROXY_ENABLE_SANITIZERS)
    target_compile_options(proxy_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(proxy_core PUBLIC -fsanitize=address,undefined)
endif()

add_executable(http_proxy src/main.cpp)
target_link_libraries(http_proxy PRIVATE proxy_core)

# Tests (Catch2 v3, fetched at configure time)
if(PROXY_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.4
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    add_executable(proxy_tests
        tests/test_lru_cache.cpp
        tests/test_parser.cpp
        tests/test_url.cpp
        tests/test_date.cpp
        tests/test_cache_control.cpp
        tests/test_http_cache.cpp
    )
    target_link_libraries(proxy_tests PRIVATE proxy_core Catch2::Catch2WithMain)

    enable_testing()
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(proxy_tests)
endif()