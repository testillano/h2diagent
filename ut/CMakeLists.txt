add_executable(unit-test
    helpers_test.cpp
)

target_include_directories(unit-test
PRIVATE
    ${H2DIAGENT_INCLUDE_BUILD_DIR}
)

target_link_libraries(unit-test
PRIVATE
    gtest
    gmock
    gtest_main
    Threads::Threads
)
