# rungraph, an ordinary consumer of the installed headers: it is not compiled into the libraries. It builds its graph by
# name, which needs the registry, so it follows it.
if(GR_ENABLE_BLOCK_REGISTRY)
  add_executable(rungraph rungraph_main.cpp)
  target_link_libraries(rungraph PRIVATE gnuradio-options gnuradio-core gnuradio-blocklib-core)
  if(TARGET gnuradio-plugin)
    target_link_libraries(rungraph PRIVATE gnuradio-plugin)
  endif()
  # where this installation puts its plugins, which the tool searches whatever the caller and the environment name
  target_compile_definitions(
    rungraph PRIVATE GR_TOOLS_INSTALLED_PLUGIN_DIRECTORY="${CMAKE_INSTALL_FULL_LIBDIR}/gnuradio-4/plugins")
  install(TARGETS rungraph RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(ENABLE_TESTING)
  add_subdirectory(test)
endif()
