add_executable(unit-test "")

# TODO: add test subdirectories as they are created
# add_subdirectory(Gateway)

# Imported libraries
add_library(ert_logger STATIC IMPORTED)
add_library(ert_metrics STATIC IMPORTED)
add_library(ert_diametercodec STATIC IMPORTED)
add_library(ert_diametercomm STATIC IMPORTED)
add_library(ert_http2comm STATIC IMPORTED)

set_property(TARGET ert_logger PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_logger.a)
set_property(TARGET ert_metrics PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_metrics.a)
set_property(TARGET ert_diametercodec PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_diametercodec.a)
set_property(TARGET ert_diametercomm PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_diametercomm.a)
set_property(TARGET ert_http2comm PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_http2comm.a)

target_link_libraries(unit-test
PRIVATE
    gtest
    gmock
    gtest_main

    ert_http2comm
    ert_diametercomm
    ert_diametercodec
    ert_metrics
    ert_logger

    prometheus-cpp-pull
    prometheus-cpp-core
    z

    nlohmann_json_schema_validator

    nghttp2_asio
    nghttp2
    ssl
    crypto

    Boost::system
    Threads::Threads
)
