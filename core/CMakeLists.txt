add_library(
  gnuradio-core STATIC
  src/Value.cpp
  src/ValueHelper.cpp
  src/Block.cpp
  src/BlockModel.cpp
  src/Port.cpp
  src/Settings.cpp
  src/Graph.cpp
  src/PluginLoader.cpp
  src/PmtTypeHelpers.cpp)

add_library(gnuradio4::gnuradio-core ALIAS gnuradio-core)
target_include_directories(
  gnuradio-core
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/src>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
         $<INSTALL_INTERFACE:include/>)

target_link_libraries(
  gnuradio-core
  PUBLIC gnuradio-options
         gnuradio-meta
         gnuradio-shared-state
         magic_enum
         vir)

# two libraries hold what a program and every shared object it loads must share one copy of. Each compiles the core
# headers it needs and links no compiled core library. gnuradio-core links both PUBLIC: the link goes from the core
# archive to them, so a program that links gnuradio-core alone links.
#
# gnuradio-shared-state holds the values that gnuradio-4.0/SharedState.hpp declares and the type information of
# DynamicPort's port interfaces and of gr::exception.
#
# gnuradio-blocklib-core holds the global block registry and the global scheduler registry. PluginLoader.cpp in the core
# archive reads them, so a program that builds a gr::Graph needs the library. The library names no core library, so the
# link line of a consumer places it after the archive that calls it. GNU ld with --as-needed keeps a shared library only
# when an earlier object uses it.
if(EMSCRIPTEN)
  add_library(gnuradio-shared-state STATIC src/SharedState.cpp src/DynamicPort.cpp src/Exception.cpp)
else()
  add_library(gnuradio-shared-state SHARED src/SharedState.cpp src/DynamicPort.cpp src/Exception.cpp)
endif()
add_library(gnuradio4::gnuradio-shared-state ALIAS gnuradio-shared-state)
target_include_directories(gnuradio-shared-state PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/)
target_link_libraries(gnuradio-shared-state PRIVATE gnuradio-options gnuradio-meta magic_enum)
if(WIN32)
  target_compile_definitions(gnuradio-shared-state PRIVATE BUILDING_MYPLUGIN)
endif()

if(EMSCRIPTEN)
  add_library(gnuradio-blocklib-core STATIC src/BlockRegistry.cpp)
else()
  add_library(gnuradio-blocklib-core SHARED src/BlockRegistry.cpp)
endif()
add_library(gnuradio4::gnuradio-blocklib-core ALIAS gnuradio-blocklib-core)
message(INFO "PATH include " $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/>)
target_include_directories(gnuradio-blocklib-core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/>
                                                         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(gnuradio-blocklib-core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/
                                                          ${CMAKE_CURRENT_BINARY_DIR}/include)
target_link_libraries(gnuradio-blocklib-core PRIVATE gnuradio-options gnuradio-meta magic_enum)
if(WIN32)
  target_compile_definitions(gnuradio-blocklib-core PRIVATE BUILDING_MYPLUGIN)
  target_compile_definitions(gnuradio-core PUBLIC BUILDING_MYPLUGIN)
endif()
target_link_libraries(gnuradio-core PUBLIC gnuradio-blocklib-core)

# configure a header file to pass the CMake settings to the source code
configure_file("${PROJECT_SOURCE_DIR}/cmake/config.hpp.in"
               "${CMAKE_CURRENT_BINARY_DIR}/include/gnuradio-4.0/config.hpp" @ONLY)
# TODO: install configure file... but not really meaningful for header only library, since compile flags are defined by
# the user...
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/gnuradio-4.0/config.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/gnuradio-4.0")

install(
  TARGETS gnuradio-core gnuradio-blocklib-core gnuradio-shared-state
  EXPORT gnuradio4Targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# the whole include directory is the public surface, and it is installed as it is laid out: a PUBLIC_HEADER list would
# flatten the subdirectories into a second copy beside the first
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

add_subdirectory(src)

if(ENABLE_TESTING)
  add_subdirectory(test)
  add_subdirectory(benchmarks)
endif()
