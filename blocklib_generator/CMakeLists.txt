cmake_minimum_required(VERSION 3.28)
project(MyProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

option(ENABLE_SPLIT "Enable splitting of generated files (pass --split to the parser)" OFF)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

if(NOT GNURADIO_PARSE_REGISTRATIONS_TOOL_CXX_COMPLILER)
  set(GNURADIO_PARSE_REGISTRATIONS_TOOL_CXX_COMPLILER ${CMAKE_CXX_COMPILER})
endif()

message(
  STATUS "Using ${GNURADIO_PARSE_REGISTRATIONS_TOOL_CXX_COMPLILER} to compile the gnuradio_4_0_parse_registrations tool"
)

# --------------------------------------------------------------------------
# Configure the rest of the project
# --------------------------------------------------------------------------

# add core functionality such as registry
configure_file(GnuRadioBlockLibMacros.cmake.in GnuRadioBlockLibMacros.cmake @ONLY)

include(${PROJECT_BINARY_DIR}/GnuRadioBlockLibMacros.cmake)

set(GR_BLOCKLIB_CMAKECONFIG_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/GnuRadioBlockLib")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/GnuRadioBlockLibConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/GnuRadioBlockLibConfig.cmake" INSTALL_DESTINATION ${GR_BLOCKLIB_CMAKECONFIG_INSTALL_DIR})

install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/GnuRadioBlockLibConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/GnuRadioBlockLibMacros.cmake"
  DESTINATION "${GR_BLOCKLIB_CMAKECONFIG_INSTALL_DIR}"
  COMPONENT Devel)
