add_executable(unit-test
    helpers_test.cpp
    diameterClientPool_test.cpp
)

target_include_directories(unit-test
PRIVATE
    ${H2DIAGENT_INCLUDE_BUILD_DIR}
    ${CMAKE_PREFIX_PATH}/include
)

# DiameterClientPool wraps real diametercomm::DiameterClient objects, so the
# pool test needs to link diametercomm and its dependency stack (same imported
# libraries as the main h2diagent binary; see src/CMakeLists.txt).
add_library(ut_ert_metrics STATIC IMPORTED)
add_library(ut_ert_logger STATIC IMPORTED)
add_library(ut_ert_diametercomm STATIC IMPORTED)
add_library(ut_prometheus_cpp_pull STATIC IMPORTED)
add_library(ut_prometheus_cpp_core STATIC IMPORTED)
add_library(ut_boost_system STATIC IMPORTED)

set_property(TARGET ut_ert_metrics PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_metrics.a)
set_property(TARGET ut_ert_logger PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_logger.a)
set_property(TARGET ut_ert_diametercomm PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/ert/libert_diametercomm.a)
set_property(TARGET ut_prometheus_cpp_pull PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/libprometheus-cpp-pull.a)
set_property(TARGET ut_prometheus_cpp_core PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/libprometheus-cpp-core.a)
set_property(TARGET ut_boost_system PROPERTY IMPORTED_LOCATION ${CMAKE_PREFIX_PATH}/lib/libboost_system.a)

target_link_libraries(unit-test
PRIVATE
    gtest
    gmock
    gtest_main
    # diametercomm + its transitive deps (order matters for static linking)
    ut_ert_diametercomm
    ut_ert_metrics
    ut_ert_logger
    ut_prometheus_cpp_pull
    ut_prometheus_cpp_core
    z
    ut_boost_system
    ssl
    crypto
    Boost::thread
    Threads::Threads
)
