cmake_minimum_required(VERSION 3.28)
project(MyProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

option(ENABLE_SPLIT "Enable splitting of generated files (pass SPLIT to the generator)" OFF)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

# add core functionality such as registry
configure_file(GnuRadioBlockLibMacros.cmake.in GnuRadioBlockLibMacros.cmake @ONLY)

include(${PROJECT_BINARY_DIR}/GnuRadioBlockLibMacros.cmake)

set(GR_BLOCKLIB_CMAKECONFIG_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/GnuRadioBlockLib")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/GnuRadioBlockLibConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/GnuRadioBlockLibConfig.cmake" INSTALL_DESTINATION ${GR_BLOCKLIB_CMAKECONFIG_INSTALL_DIR})

# the generator script is installed beside the macros, which is where they look for it
install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/GnuRadioBlockLibConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/GnuRadioBlockLibMacros.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/GrParseRegistrations.cmake"
  DESTINATION "${GR_BLOCKLIB_CMAKECONFIG_INSTALL_DIR}"
  COMPONENT Devel)
