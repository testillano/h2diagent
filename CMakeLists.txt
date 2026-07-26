cmake_minimum_required(VERSION 3.14)
project(h2diagent VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_UNIT_TEST "Build unit tests" OFF)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build/Release/bin)

# Include paths
include_directories(${CMAKE_SOURCE_DIR}/include)

# Find packages
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(Threads REQUIRED)

# Main executable
add_executable(h2diagent
    src/main.cpp
    src/Gateway.cpp
)

target_link_libraries(h2diagent
    ert_logger
    ert_metrics
    ert_http2comm
    ert_diametercodec
    ert_diametercomm
    nghttp2_asio
    nghttp2
    Boost::system
    Boost::thread
    Threads::Threads
    ssl
    crypto
    z
    prometheus-cpp-pull
    prometheus-cpp-core
)

# Unit tests
if(BUILD_UNIT_TEST)
    enable_testing()
    find_package(GTest REQUIRED)

    add_executable(unit-test
        ut/main.cpp
        ut/Gateway_test.cpp
        src/Gateway.cpp
    )

    target_include_directories(unit-test PRIVATE ${CMAKE_SOURCE_DIR}/include)

    target_link_libraries(unit-test
        GTest::gtest
        GTest::gtest_main
        ert_logger
        ert_metrics
        ert_http2comm
        ert_diametercodec
        ert_diametercomm
        nghttp2_asio
        nghttp2
        Boost::system
        Boost::thread
        Threads::Threads
        ssl
        crypto
        z
        prometheus-cpp-pull
        prometheus-cpp-core
    )

    add_test(NAME unit-test COMMAND unit-test)
endif()
