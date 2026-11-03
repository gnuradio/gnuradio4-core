# An installed tool reaches the libraries of its install tree through a run path relative to its own directory, so it
# runs without a library path in the environment. A library directory given as an absolute path outside the prefix is
# not followed. The run path of a tool in the build tree stays the one CMake sets, and a packager's CMAKE_INSTALL_RPATH
# is appended.
file(
  RELATIVE_PATH
  _gr_tools_bin_to_lib
  "${CMAKE_INSTALL_FULL_BINDIR}"
  "${CMAKE_INSTALL_FULL_LIBDIR}")
if(APPLE)
  set(GR_TOOLS_INSTALL_RPATH "@loader_path/${_gr_tools_bin_to_lib}")
else()
  set(GR_TOOLS_INSTALL_RPATH "$ORIGIN/${_gr_tools_bin_to_lib}")
endif()
list(APPEND GR_TOOLS_INSTALL_RPATH ${CMAKE_INSTALL_RPATH})

# rungraph's body, as a library: gr::tools::runGraph takes main's arguments and returns the exit status. It builds its
# graph by name, which needs the registry, and is built only with it. A program that links its block families instead of
# loading plugins, as under Emscripten, calls it from its own main. Its source includes core's internal
# ShippedSchedulers.hpp through a private include directory. The installed RunGraph.hpp does not include it, and a
# consumer needs no path into core/src.
if(GR_ENABLE_BLOCK_REGISTRY)
  add_library(gnuradio-rungraph STATIC RunGraph.cpp)
  add_library(gnuradio4::gnuradio-rungraph ALIAS gnuradio-rungraph)
  target_include_directories(gnuradio-rungraph PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                                      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_include_directories(gnuradio-rungraph PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
  target_link_libraries(gnuradio-rungraph PUBLIC gnuradio-options gnuradio-core gnuradio-blocklib-core)
  # where this installation puts its plugins, which the tool searches whatever the caller and the environment name
  target_compile_definitions(gnuradio-rungraph
                             PRIVATE GR_TOOLS_INSTALLED_PLUGIN_DIRECTORY="${GR_INSTALLED_PLUGIN_DIRECTORY}")
  install(
    TARGETS gnuradio-rungraph
    EXPORT gnuradio4Targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

# graphdoc and the headers it shares with its test. It is an ordinary consumer of the installed headers: nothing here is
# compiled into the libraries. Its document is read from the graph file, so it is built whether or not the registry is
# on.
add_library(gnuradio-doctools INTERFACE)
add_library(gnuradio4::gnuradio-doctools ALIAS gnuradio-doctools)
target_include_directories(gnuradio-doctools INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(gnuradio-doctools INTERFACE gnuradio-options gnuradio-core gnuradio-blocklib-core)

add_executable(graphdoc graphdoc_main.cpp)
target_link_libraries(graphdoc PRIVATE gnuradio-doctools)
if(TARGET gnuradio-plugin)
  target_link_libraries(graphdoc PRIVATE gnuradio-plugin)
endif()
# where this installation puts its plugins, which the tool searches whatever the caller and the environment name
target_compile_definitions(graphdoc PRIVATE GR_TOOLS_INSTALLED_PLUGIN_DIRECTORY="${GR_INSTALLED_PLUGIN_DIRECTORY}")
set_target_properties(graphdoc PROPERTIES INSTALL_RPATH "${GR_TOOLS_INSTALL_RPATH}")
install(TARGETS graphdoc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# rungraph, the program over gnuradio-rungraph
if(GR_ENABLE_BLOCK_REGISTRY)
  add_executable(rungraph rungraph_main.cpp)
  target_link_libraries(rungraph PRIVATE gnuradio-rungraph)
  if(TARGET gnuradio-plugin)
    target_link_libraries(rungraph PRIVATE gnuradio-plugin)
  endif()
  set_target_properties(rungraph PROPERTIES INSTALL_RPATH "${GR_TOOLS_INSTALL_RPATH}")
  install(TARGETS rungraph RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# grinfo, a consumer of the installed headers and of ShippedSchedulers.hpp as gnuradio-rungraph is. It reports the
# registered blocks, which only a build with the registry has.
if(GR_ENABLE_BLOCK_REGISTRY)
  add_executable(grinfo grinfo_main.cpp)
  target_include_directories(grinfo PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
  # gnuradio-doctools carries the writer of its Markdown and HTML forms
  target_link_libraries(grinfo PRIVATE gnuradio-doctools)
  if(TARGET gnuradio-plugin)
    target_link_libraries(grinfo PRIVATE gnuradio-plugin)
  endif()
  # what this installation is and where it puts its plugins; the project version is not in config.hpp, so the build
  # hands it over too
  target_compile_definitions(
    grinfo PRIVATE GR_TOOLS_CORE_VERSION="${PROJECT_VERSION}" GR_TOOLS_INSTALLED_PREFIX="${CMAKE_INSTALL_PREFIX}"
                   GR_TOOLS_INSTALLED_PLUGIN_DIRECTORY="${GR_INSTALLED_PLUGIN_DIRECTORY}")
  set_target_properties(grinfo PROPERTIES INSTALL_RPATH "${GR_TOOLS_INSTALL_RPATH}")
  install(TARGETS grinfo RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(ENABLE_TESTING)
  add_subdirectory(test)
endif()
