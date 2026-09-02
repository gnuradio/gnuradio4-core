add_library(
  gnuradio-core STATIC
  src/Value.cpp
  src/ValueHelper.cpp
  src/Block.cpp
  src/BlockModel.cpp
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
         magic_enum
         vir)

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
  TARGETS gnuradio-core gnuradio-blocklib-core
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
