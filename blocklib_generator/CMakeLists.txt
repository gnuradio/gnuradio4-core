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
        "${CMAKE_CURRENT_SOURCE_DIR}/parse_registrations.py"
  DESTINATION "${GR_BLOCKLIB_CMAKECONFIG_INSTALL_DIR}"
  COMPONENT Devel)

if(ENABLE_TESTING)
  # the generators need no compiler, so their test is a script run against a recorded expectation; the two core plugin
  # headers are the larger input the two are compared on
  set(GR_BLOCKLIB_TEST_EXTRA_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/../core/test/plugins/good_base_plugin.hpp"
                                     "${CMAKE_CURRENT_SOURCE_DIR}/../core/test/plugins/good_registry_plugin.hpp")
  foreach(GR_BLOCKLIB_TEST_MODE chunked split)
    add_test(
      NAME qa_GrParseRegistrations_${GR_BLOCKLIB_TEST_MODE}
      COMMAND
        ${CMAKE_COMMAND} "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/GrParseRegistrations.cmake"
        "-DPYTHON=${Python3_EXECUTABLE}" "-DPYTHON_GENERATOR=${CMAKE_CURRENT_SOURCE_DIR}/parse_registrations.py"
        "-DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/test/fixture_markers.hpp"
        "-DEXTRA_HEADERS=${GR_BLOCKLIB_TEST_EXTRA_HEADERS}" "-DEXPECTED_DIR=${CMAKE_CURRENT_SOURCE_DIR}/test/expected"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test" "-DMODE=${GR_BLOCKLIB_TEST_MODE}" -P
        "${CMAKE_CURRENT_SOURCE_DIR}/test/RunGeneratorTest.cmake")
  endforeach()
endif()
