############
# Versions #
############
cmake_minimum_required(VERSION 3.5.1)
project(h2diagent LANGUAGES CXX)

set(MAIN_PROJECT OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(MAIN_PROJECT ON)
endif()

#############
# Variables #
#############
set(H2DIAGENT_TARGET_NAME ${PROJECT_NAME})
set(H2DIAGENT_INCLUDE_BUILD_DIR "${PROJECT_SOURCE_DIR}/include")

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

###########
# Modules #
###########
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/")

# Build type
include(build_type)
set_cmake_build_type()

# Compiler flags
include(compiler_flags)
set_cmake_compiler_flags()

# Build directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build/${CMAKE_BUILD_TYPE}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build/${CMAKE_BUILD_TYPE}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build/${CMAKE_BUILD_TYPE}/lib)

# CMAKE_PREFIX_PATH
if(NOT CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH /usr/local)
endif()
message(STATUS "CMAKE_PREFIX_PATH is ${CMAKE_PREFIX_PATH}")

################
# Dependencies #
################
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(Threads REQUIRED)

##################
# Subdirectories #
##################
add_subdirectory(src)

###########
# Install #
###########
SET(MY_OWN_INSTALL_PREFIX "/opt/h2diagent" CACHE PATH "Prefix prepended to install directories")
SET(CMAKE_INSTALL_PREFIX "${MY_OWN_INSTALL_PREFIX}" CACHE INTERNAL "Prefix prepended to install directories" FORCE)

install(TARGETS ${H2DIAGENT_TARGET_NAME} RUNTIME DESTINATION bin)
