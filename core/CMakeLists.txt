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

# the shipped schedulers, registered under the keys a graph names them by, and the runtime that builds a graph by name;
# both need a registry to look the names up in
if(GR_ENABLE_BLOCK_REGISTRY)
  target_sources(gnuradio-core PRIVATE src/ShippedSchedulers.cpp src/Runtime.cpp)
endif()

add_library(gnuradio4::gnuradio-core ALIAS gnuradio-core)
# gr::installedPluginDirectory() returns the plugin directory of this installation, and the process's plugin loader
# searches it when GNURADIO4_PLUGIN_DIRECTORIES is not set
set_property(
  SOURCE src/PluginLoader.cpp
  APPEND
  PROPERTY COMPILE_DEFINITIONS GR_INSTALLED_PLUGIN_DIRECTORY="${GR_INSTALLED_PLUGIN_DIRECTORY}")
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

# the values that gnuradio-4.0/SharedState.hpp declares and the type information of DynamicPort's port interfaces and of
# gr::exception, one copy that every shared object of a program shares. The library compiles the core headers that build
# those values and links no compiled core library. gnuradio-core links it
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
target_link_libraries(gnuradio-blocklib-core PUBLIC gnuradio-core)
if(WIN32)
  target_compile_definitions(gnuradio-core PUBLIC BUILDING_MYPLUGIN)
endif()

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
if(GR_ENABLE_BLOCK_REGISTRY)
  install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
else()
  install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    PATTERN "Runtime.hpp" EXCLUDE)
endif()

add_subdirectory(src)

if(ENABLE_TESTING)
  add_subdirectory(test)
  add_subdirectory(benchmarks)
endif()
