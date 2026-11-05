if(NOT EMSCRIPTEN)
  # BASELINE builds for the target's baseline instruction set instead of the build machine's
  function(add_gr_benchmark BM_NAME)
    cmake_parse_arguments(
      PARSE_ARGV
      1
      BM
      "BASELINE"
      ""
      "")
    add_benchmark(${BM_NAME})
    target_link_libraries(
      ${BM_NAME}
      PRIVATE gnuradio-options
              gnuradio-core
              gnuradio-blocklib-core
              gnuradio-plugin
              ut
              ut-benchmark)
    if(EMSCRIPTEN OR CMAKE_CROSSCOMPILING) # performance benchmarks should be optimised
      target_compile_options(${BM_NAME} PRIVATE -O3)
    elseif(MSVC)
      target_compile_options(${BM_NAME} PRIVATE /O2) # cl.exe understands neither -O3 nor -march=native
    elseif(BM_BASELINE)
      # add_benchmark() has added -march=native to both option lists
      foreach(property COMPILE_OPTIONS LINK_OPTIONS)
        get_target_property(options ${BM_NAME} ${property})
        list(REMOVE_ITEM options -march=native)
        set_target_properties(${BM_NAME} PROPERTIES ${property} "${options}")
      endforeach()
      target_compile_options(${BM_NAME} PRIVATE -O3)
    else()
      target_compile_options(${BM_NAME} PRIVATE -O3 -march=native)
    endif()

    target_include_directories(${BM_NAME} PRIVATE "../test")
  endfunction()

  add_gr_benchmark(bm_Buffer)
  add_gr_benchmark(bm_ChainCost)
  add_gr_benchmark(bm_HistoryBuffer)
  add_gr_benchmark(bm_ProcessOneSimd)
  add_gr_benchmark(bm_Profiler)
  add_gr_benchmark(bm_SchedulerIdle)
  add_gr_benchmark(bm_SubnormalFlush BASELINE)
  add_gr_benchmark(bm_portLimits)
  message(STATUS "Skipping standard-block/algorithm benchmarks in gnuradio4-core")
endif()
