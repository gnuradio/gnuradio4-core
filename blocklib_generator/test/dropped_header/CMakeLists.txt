# A block library reduced to its generated registration: the headers are text for the generator, and the test that
# configures this project builds nothing.
cmake_minimum_required(VERSION 3.28)
project(gr_blocklib_dropped_header LANGUAGES CXX)

include("${MACROS}")

add_library(DroppedHeaderLib OBJECT)
gr_generate_block_instantiations(
  DroppedHeaderLib
  HEADERS
  ${HEADERS}
  MODULE_NAME_BASE
  DroppedHeaderLib)
