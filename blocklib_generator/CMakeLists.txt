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
# Manually configure and build the parser tool in a separate directory
# --------------------------------------------------------------------------
set(TOOLS_BUILD_DIR "${PROJECT_BINARY_DIR}/tools_build")

# tools/CMakeLists.txt pins its own build type, so the tool is always this configuration -- for
# multi-config generators that has to be repeated on the command line, which ignore CMAKE_BUILD_TYPE.
set(PARSER_TOOL_CONFIG Release)

# Where the sub-build records the executable's real path. It sits at the root of the tool build
# directory rather than next to the binary, because multi-config generators put the binary in a
# per-config subdirectory whose name this project cannot predict. Keeping it inside TOOLS_BUILD_DIR
# means deleting that directory invalidates the stamp along with it.
set(PARSER_STAMP_FILE "${TOOLS_BUILD_DIR}/gnuradio_4_0_parse_registrations-${PARSER_TOOL_CONFIG}.stamp")

if(NOT EXISTS "${PARSER_STAMP_FILE}")
  if(EXISTS "${TOOLS_BUILD_DIR}/CMakeCache.txt")
    # Either a tool build from before the stamp existed, or one made with a different generator --
    # CMake refuses to reconfigure that in place, so start over rather than fail.
    message(STATUS "Discarding stale parser tool build in ${TOOLS_BUILD_DIR}")
    file(REMOVE_RECURSE "${TOOLS_BUILD_DIR}")
  endif()

  set(PARSER_GENERATOR_ARGS -G${CMAKE_GENERATOR})
  if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND PARSER_GENERATOR_ARGS -A${CMAKE_GENERATOR_PLATFORM})
  endif()
  if(CMAKE_GENERATOR_TOOLSET)
    list(APPEND PARSER_GENERATOR_ARGS -T${CMAKE_GENERATOR_TOOLSET})
  endif()

  message(STATUS "Configuring parser tool in ${TOOLS_BUILD_DIR}...")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -S ${PROJECT_SOURCE_DIR}/tools -B ${TOOLS_BUILD_DIR} ${PARSER_GENERATOR_ARGS}
            -DCMAKE_CXX_COMPILER=${GNURADIO_PARSE_REGISTRATIONS_TOOL_CXX_COMPLILER}
            -DGR_PARSER_STAMP_DIR=${TOOLS_BUILD_DIR}
    RESULT_VARIABLE config_result
    OUTPUT_VARIABLE config_output
    ERROR_VARIABLE config_error
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
  if(NOT
     config_result
     EQUAL
     0)
    message(FATAL_ERROR "Failed to configure parser tool: ${config_error}")
  endif()
endif()

message(STATUS "Building parser tool in ${TOOLS_BUILD_DIR}...")
execute_process(
  COMMAND ${CMAKE_COMMAND} --build ${TOOLS_BUILD_DIR} --target gnuradio_4_0_parse_registrations --config
          ${PARSER_TOOL_CONFIG}
  WORKING_DIRECTORY ${TOOLS_BUILD_DIR}
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT
   build_result
   EQUAL
   0)
  message(FATAL_ERROR "Failed to build gnuradio_4_0_parse_registrations tool: ${build_error} ${build_output}")
endif()

if(NOT EXISTS "${PARSER_STAMP_FILE}")
  message(FATAL_ERROR "The parser tool build produced no ${PARSER_STAMP_FILE}. "
                      "Delete ${TOOLS_BUILD_DIR} and re-run CMake.")
endif()
file(READ "${PARSER_STAMP_FILE}" PARSER_EXECUTABLE)
string(STRIP "${PARSER_EXECUTABLE}" PARSER_EXECUTABLE)
if(NOT EXISTS "${PARSER_EXECUTABLE}")
  message(FATAL_ERROR "The parser tool build reported '${PARSER_EXECUTABLE}', which does not exist. "
                      "Delete ${TOOLS_BUILD_DIR} and re-run CMake.")
endif()
message(STATUS "Parser tool built: ${PARSER_EXECUTABLE}")

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

install(PROGRAMS ${PARSER_EXECUTABLE} DESTINATION ${CMAKE_INSTALL_BINDIR})
