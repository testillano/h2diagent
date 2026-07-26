function(set_cmake_build_type)

  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release PARENT_SCOPE)
  endif()
  message(STATUS "CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")

endfunction(set_cmake_build_type)
