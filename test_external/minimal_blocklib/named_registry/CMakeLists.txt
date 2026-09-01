add_library(named_registry_blocklib_support INTERFACE)
target_include_directories(named_registry_blocklib_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# the registry options are what this library exists to exercise: its blocks go into the instance the project declares
# rather than into gr::globalBlockRegistry
gr_add_block_library(
  NamedRegistryBlockLib
  MAKE_STATIC_LIBRARY
  REGISTRY_HEADER
  named_registry_blocklib/Registry.hpp
  REGISTRY_INSTANCE
  namedBlockRegistry
  HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/include/named_registry_blocklib/Scale.hpp
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/named_registry_blocklib.cpp
  LINK_LIBRARIES
  named_registry_blocklib_support)
