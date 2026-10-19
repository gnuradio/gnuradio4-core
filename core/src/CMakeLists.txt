if(ENABLE_TESTS)
  add_subdirectory(test)
  add_subdirectory(benchmarks)
endif()

if(NOT EMSCRIPTEN)
  # the libraries a plugin needs, named as one
  add_library(gnuradio-plugin INTERFACE)
  add_library(gnuradio4::gnuradio-plugin ALIAS gnuradio-plugin)
  target_link_libraries(gnuradio-plugin INTERFACE gnuradio-core gnuradio-blocklib-core gnuradio-options)

  install(TARGETS gnuradio-plugin EXPORT gnuradio4PluginTargets)
endif()

if(ENABLE_EXAMPLES)
  add_executable(main main.cpp)
  target_link_libraries(main PUBLIC gnuradio-options gnuradio-core magic_enum)
endif()
