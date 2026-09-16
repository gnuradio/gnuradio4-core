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

# grinfo, an ordinary consumer of the installed headers as rungraph is. It reports the registered blocks, which only a
# build with the registry has.
if(GR_ENABLE_BLOCK_REGISTRY)
  add_executable(grinfo grinfo_main.cpp)
  target_link_libraries(grinfo PRIVATE gnuradio-options gnuradio-core gnuradio-blocklib-core)
  if(TARGET gnuradio-plugin)
    target_link_libraries(grinfo PRIVATE gnuradio-plugin)
  endif()
  # what this installation is and where it puts its plugins; the project version is not in config.hpp, so the build
  # hands it over too
  target_compile_definitions(
    grinfo PRIVATE GR_TOOLS_CORE_VERSION="${PROJECT_VERSION}" GR_TOOLS_INSTALLED_PREFIX="${CMAKE_INSTALL_PREFIX}"
                   GR_TOOLS_INSTALLED_PLUGIN_DIRECTORY="${CMAKE_INSTALL_FULL_LIBDIR}/gnuradio-4/plugins")
  install(TARGETS grinfo RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(ENABLE_TESTING)
  add_subdirectory(test)
endif()
