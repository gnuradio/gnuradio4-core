add_library(other_blocklib_support INTERFACE)
target_include_directories(other_blocklib_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

gr_add_block_library(
  OtherBlockLib
  MAKE_SHARED_LIBRARY
  MAKE_STATIC_LIBRARY
  HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/include/other_blocklib/Gain.hpp
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/other_blocklib.cpp
  LINK_LIBRARIES
  other_blocklib_support)
