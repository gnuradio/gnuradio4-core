cmake_minimum_required(VERSION 3.28)

project(gr4_minimal_blocklib LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(gnuradio4 CONFIG REQUIRED)
find_package(GnuRadioBlockLib CONFIG REQUIRED)

add_library(minimal_blocklib_support INTERFACE)
target_include_directories(minimal_blocklib_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

gr_add_block_library(
  MinimalBlockLib
  MAKE_SHARED_LIBRARY
  HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/include/minimal_blocklib/Gain.hpp
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/minimal_blocklib.cpp
  LINK_LIBRARIES
  minimal_blocklib_support)

add_executable(minimal_blocklib_consumer src/consumer.cpp)
target_link_libraries(minimal_blocklib_consumer PRIVATE MinimalBlockLibShared gnuradio4::gnuradio-core)
