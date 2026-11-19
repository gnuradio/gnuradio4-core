add_library(ut-benchmark INTERFACE benchmark.hpp)
add_library(gnuradio4::ut-benchmark ALIAS ut-benchmark)
target_include_directories(ut-benchmark INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                                                  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# Installed, it names Boost.UT's own target: the build's 'ut' alias exists only in a build with tests, and a project
# that builds benchmarks against a core built without them still gets the harness.
target_link_libraries(ut-benchmark INTERFACE $<BUILD_INTERFACE:ut> $<INSTALL_INTERFACE:Boost::ut>)
install(TARGETS ut-benchmark EXPORT gnuradio4Targets)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/benchmark.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

function(append_compiler_flags TARGET_NAME)
  if(MSVC)
    # None of these flags apply to msvc frontend type compilers
    return()
  endif()
  set(FLAGS_COMMON -Wall)
  if(EMSCRIPTEN OR CMAKE_CROSSCOMPILING)
    set(FLAGS_SPECIAL "")
  else()
    set(FLAGS_SPECIAL -march=native)
  endif()

  target_compile_options(${TARGET_NAME} PRIVATE ${FLAGS_COMMON} ${FLAGS_SPECIAL})
  target_link_options(
    ${TARGET_NAME}
    PRIVATE
    ${FLAGS_COMMON}
    ${FLAGS_SPECIAL})
endfunction()

function(add_benchmark BM_NAME)
  add_executable(${BM_NAME} ${BM_NAME}.cpp)
  append_compiler_flags(${BM_NAME})
  target_link_libraries(${BM_NAME} PRIVATE gnuradio4::gnuradio-options ut gnuradio4::ut-benchmark)
endfunction()

if(ENABLE_TESTING)
  add_benchmark(bm_example)
endif()
