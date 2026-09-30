cmake_minimum_required(VERSION 3.27)
# The fixture enables no language. The tree's own project() is then the first to read the compiler rules files, as in a
# top-level configure of the tree.
project(gr4_rules_override_fixture LANGUAGES NONE)
set(CMAKE_PROJECT_gnuradio_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/CheckTryCompile.cmake")
add_subdirectory(${GR4_SOURCE_DIR} gnuradio4)
