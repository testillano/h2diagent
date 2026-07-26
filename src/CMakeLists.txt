add_executable(${H2DIAGENT_TARGET_NAME}
  main.cpp
  Gateway.cpp
)

target_include_directories(${H2DIAGENT_TARGET_NAME}
PRIVATE
  ${H2DIAGENT_INCLUDE_BUILD_DIR}
  ${CMAKE_PREFIX_PATH}/include
)

# To avoid setting absolute paths for libraries down at 'target_link_libraries':
add_library(ert_logger STATIC IMPORTED)
add_library(ert_metrics STATIC IMPORTED)
add_library(ert_queuedispatcher STATIC IMPORTED)
add_library(ert_http2comm STATIC IMPORTED)
add_library(ert_diametercodec STATIC IMPORTED)
add_library(ert_diametercomm STATIC IMPORTED)
add_library(prometheus-cpp-pull STATIC IMPORTED)
add_library(prometheus-cpp-core STATIC IMPORTED)
add_library(boost_system STATIC IMPORTED)
add_library(nghttp2_asio STATIC IMPORTED)
add_library(nghttp2 STATIC IMPORTED)
add_library(nlohmann_json_schema_validator STATIC IMPORTED)

set_property(TARGET ert_logger PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_logger.a)
set_property(TARGET ert_metrics PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_metrics.a)
set_property(TARGET ert_queuedispatcher PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_queuedispatcher.a)
set_property(TARGET ert_http2comm PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_http2comm.a)
set_property(TARGET ert_diametercodec PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_diametercodec.a)
set_property(TARGET ert_diametercomm PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_diametercomm.a)
set_property(TARGET prometheus-cpp-pull PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/libprometheus-cpp-pull.a)
set_property(TARGET prometheus-cpp-core PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/libprometheus-cpp-core.a)
set_property(TARGET boost_system PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/libboost_system.a)
set_property(TARGET nghttp2_asio PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/libnghttp2_asio.a)
set_property(TARGET nghttp2 PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/libnghttp2.a)
set_property(TARGET nlohmann_json_schema_validator PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/libnlohmann_json_schema_validator.a)

target_link_libraries(${H2DIAGENT_TARGET_NAME}
PRIVATE
${CMAKE_EXE_LINKER_FLAGS}
        ert_http2comm
        ert_diametercomm
        ert_diametercodec
        ert_queuedispatcher
        ert_metrics
        ert_logger

        prometheus-cpp-pull
        prometheus-cpp-core
        z

        boost_system
        nghttp2_asio
        nghttp2
        ssl
        crypto
        pthread

        nlohmann_json_schema_validator

        Boost::thread
        Threads::Threads
)
