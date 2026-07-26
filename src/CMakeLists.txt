add_executable(${H2DIAGENT_TARGET_NAME}
  main.cpp
  Gateway.cpp
)

target_include_directories(${H2DIAGENT_TARGET_NAME}
PRIVATE
  ${H2DIAGENT_INCLUDE_BUILD_DIR}
  ${CMAKE_PREFIX_PATH}/include
)

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

target_link_libraries(${H2DIAGENT_TARGET_NAME}
PRIVATE
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
