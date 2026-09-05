# Plugins based on the blocklib generator
gr_add_block_plugin(
  GoodBase
  NO_INSTALL
  SOURCES
  good_base_plugin.cpp
  HEADERS
  good_base_plugin.hpp)

# the same, registering into an instance the plugin declares in a header of its own, which is how a plugin built outside
# this tree names its registry
gr_add_block_plugin(
  GoodRegistry
  NO_INSTALL
  REGISTRY_HEADER
  good_registry_instance.hpp
  REGISTRY_INSTANCE
  grPluginInstance
  SOURCES
  good_registry_plugin.cpp
  HEADERS
  good_registry_plugin.hpp)
target_include_directories(GoodRegistry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Ordinary plugins
function(add_non_blocklib_plugin PLUGIN_NAME)
  add_library(${PLUGIN_NAME} MODULE ${PLUGIN_NAME}.cpp)

  # target_compile_options(${PLUGIN_NAME} PRIVATE -fsanitize=address -Wall)
  target_compile_options(${PLUGIN_NAME} PRIVATE -Wall)

  target_include_directories(${PLUGIN_NAME} PUBLIC ${CMAKE_BINARY_DIR}/include)
  target_link_libraries(${PLUGIN_NAME} PUBLIC gnuradio-options gnuradio-core gnuradio-plugin)
endfunction()

add_non_blocklib_plugin(good_math_plugin)
add_non_blocklib_plugin(good_conversion_plugin)
add_non_blocklib_plugin(bad_plugin)

# Shared objects that register blocks without being plugins: no gr_plugin_make, entries reaching the registry from
# static initializers. Two go in one directory, because one alone does not show what unloading it does to a neighbor.
function(add_block_library LIBRARY_NAME DIRECTORY_NAME)
  add_library(${LIBRARY_NAME} MODULE ${LIBRARY_NAME}.cpp)
  target_link_libraries(${LIBRARY_NAME} PRIVATE gnuradio-options gnuradio-core gnuradio-blocklib-core)
  set_target_properties(${LIBRARY_NAME} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${TESTS_BINARY_PATH}/${DIRECTORY_NAME})
endfunction()

add_block_library(block_library block_library)
add_block_library(block_library_second block_library)

# The pair in this directory register one block under the same type name and alias, as duplicate installations of a
# block library do. The second to load replaces the first one's entries without changing how many there are.
add_block_library(block_library_duplicate block_library_duplicate)
add_block_library(block_library_duplicate_second block_library_duplicate)

# a name that reads as a shared object but is not one the scan opens; it is never dlopened, so an empty file suffices
file(MAKE_DIRECTORY ${TESTS_BINARY_PATH}/block_library_skipped)
file(TOUCH ${TESTS_BINARY_PATH}/block_library_skipped/libnotopened.so.1)
