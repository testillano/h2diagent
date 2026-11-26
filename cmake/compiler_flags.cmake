function(set_cmake_compiler_flags)

  add_compile_options(
    "-Wall"
    "-Werror"
    "-Wno-deprecated"
    "-Wwrite-strings"
    "-Wno-unknown-pragmas"
    "-Wno-sign-compare"
    "-Wno-maybe-uninitialized"
    "-Wno-unused"
    "-Wno-reorder"
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Debug>:--coverage>
    # Coverage counters updated atomically: the gateway and the unit tests run on
    # many threads (client connection threads, HTTP/2 threads), and plain counter
    # increments race into impossible negative counts that make lcov fail.
    $<$<CONFIG:Debug>:-fprofile-update=atomic>
  )

  add_compile_options(-frecord-gcc-switches)

  link_libraries(
      $<$<CONFIG:Debug>:--coverage>
  )

endfunction(set_cmake_compiler_flags)
