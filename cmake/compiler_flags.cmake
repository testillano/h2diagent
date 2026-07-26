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
  )

  add_compile_options(-frecord-gcc-switches)

  link_libraries(
      $<$<CONFIG:Debug>:--coverage>
  )

endfunction(set_cmake_compiler_flags)
