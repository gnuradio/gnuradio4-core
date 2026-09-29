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
  if(NOT MSVC) # existing /W4 setting from general GR compilation applies here for msvc
    target_compile_options(${PLUGIN_NAME} PRIVATE -Wall)
  endif()

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

# One plugin at two plugin ABI versions, in a directory of their own: one at the version this core implements and one at
# version 1. A third plugin is at the version this core implements and registers a scheduler at version 1 when it loads.
# A single load of that directory reaches every outcome of the loader's version checks for plugins.
function(add_abi_probe_plugin PLUGIN_NAME)
  add_library(${PLUGIN_NAME} MODULE ${PLUGIN_NAME}.cpp)
  target_compile_options(${PLUGIN_NAME} PRIVATE -Wall)
  target_include_directories(${PLUGIN_NAME} PUBLIC ${CMAKE_BINARY_DIR}/include)
  target_link_libraries(${PLUGIN_NAME} PUBLIC gnuradio-options gnuradio-core gnuradio-plugin)
  set_target_properties(${PLUGIN_NAME} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${TESTS_BINARY_PATH}/plugin_abi)
endfunction()

add_abi_probe_plugin(abi_probe_plugin)
add_abi_probe_plugin(abi_probe_plugin_v1)
add_abi_probe_plugin(abi_probe_plugin_foreign_scheduler)

# One shared object that registers a scheduler without being a plugin, built at the plugin ABI version this core
# implements, at version 1 and without a version, in a directory of their own. A load of that directory keeps the first
# and refuses the other two.
function(add_scheduler_library LIBRARY_NAME)
  add_library(${LIBRARY_NAME} MODULE scheduler_library.cpp)
  target_link_libraries(${LIBRARY_NAME} PRIVATE gnuradio-options gnuradio-core gnuradio-blocklib-core)
  set_target_properties(${LIBRARY_NAME} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${TESTS_BINARY_PATH}/scheduler_library)
endfunction()

add_scheduler_library(scheduler_library)
add_scheduler_library(scheduler_library_v1)
target_compile_definitions(scheduler_library_v1 PRIVATE SCHEDULER_LIBRARY_ABI_VERSION=1)
add_scheduler_library(scheduler_library_unversioned)
target_compile_definitions(scheduler_library_unversioned PRIVATE SCHEDULER_LIBRARY_UNVERSIONED)

# a name that reads as a shared object but is not one the scan opens; it is never dlopened, so an empty file suffices
file(MAKE_DIRECTORY ${TESTS_BINARY_PATH}/block_library_skipped)
file(TOUCH ${TESTS_BINARY_PATH}/block_library_skipped/libnotopened.so.1)
