# Configure one block library four times in one build directory. Its two headers, Gain.hpp and Gain_ext.hpp, share the
# prefix of their generated file names. Each is dropped from the list once and listed again. Once a header is removed
# from the list, the generation directory holds no file generated from it, and the merged declarations and calls name
# the units of listed headers alone. The other header's files are untouched, and so are files the generators did not
# write. A header that returns to the list gets its units back. The fixture is copied under a directory whose name holds
# a character outside ASCII, which the header paths inside the generated units then carry.
#
# cmake -DPROJECT_DIR=<fixture project> -DMACROS=<configured macros> -DWORK_DIR=<dir> [-DGENERATOR=<cmake generator>]
# [-DCXX_COMPILER=<compiler>] -P RunDroppedHeaderTest.cmake

cmake_minimum_required(VERSION 3.28)

foreach(_required PROJECT_DIR MACROS WORK_DIR)
  if(NOT DEFINED ${_required})
    message(FATAL_ERROR "RunDroppedHeaderTest: ${_required} is required")
  endif()
endforeach()

set(_source "${WORK_DIR}/dropped_header_fixture_é")
file(REMOVE_RECURSE "${_source}")
file(COPY "${PROJECT_DIR}/" DESTINATION "${_source}")
set(_gain "${_source}/Gain.hpp")
set(_gain_ext "${_source}/Gain_ext.hpp")
set(_hand_written "hand_written.cpp" "hand_written.hpp.in")
set(_build "${WORK_DIR}/dropped_header")
set(_generated "${_build}/plugins/DroppedHeaderLib")
file(REMOVE_RECURSE "${_build}")

set(_generator_option "")
if(GENERATOR)
  list(
    APPEND
    _generator_option
    -G
    "${GENERATOR}")
endif()
if(CXX_COMPILER)
  list(APPEND _generator_option "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()

function(gr_configure_library STEP HEADERS)
  execute_process(
    COMMAND ${CMAKE_COMMAND} ${_generator_option} -S "${_source}" -B "${_build}" "-DMACROS=${MACROS}"
            "-DHEADERS=${HEADERS}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _error)
  if(NOT
     _result
     EQUAL
     0)
    message(FATAL_ERROR "${STEP}: the configure failed\n${_output}\n${_error}")
  endif()
endfunction()

# the units of a header carry its file name, then a unit number, in their own names and in the symbols of their
# initializers; the match is exact, so Gain's units never include Gain_ext's
function(
  gr_expect_units
  STEP
  HEADER
  LISTED)
  get_filename_component(_stem "${HEADER}" NAME_WE)
  file(
    GLOB _all
    RELATIVE "${_generated}"
    "${_generated}/*")
  set(_units ${_all})
  list(
    FILTER
    _units
    INCLUDE
    REGEX
    "^${_stem}_(block_)?[0-9]+(_[0-9]+)?(\\.cpp|_declarations\\.hpp\\.in|_raw_calls\\.hpp\\.in)$")
  file(READ "${_generated}/declarations.hpp" _declarations)
  file(READ "${_generated}/raw_calls.hpp" _calls)
  string(
    REGEX MATCH
          "_${_stem}_[0-9]"
          _in_declarations
          "${_declarations}")
  string(
    REGEX MATCH
          "_${_stem}_[0-9]"
          _in_calls
          "${_calls}")
  if(LISTED)
    if(NOT _units
       OR NOT _in_declarations
       OR NOT _in_calls)
      message(FATAL_ERROR "${STEP}: ${_stem} is listed, but its units are missing (files: '${_units}')")
    endif()
  else()
    if(_units)
      message(FATAL_ERROR "${STEP}: ${_stem} left the list, but its generated files remain: ${_units}")
    endif()
    if(_in_declarations OR _in_calls)
      message(FATAL_ERROR "${STEP}: ${_stem} left the list, but the merged declarations or calls still name its units")
    endif()
  endif()
endfunction()

function(gr_expect_hand_written STEP)
  foreach(_name IN LISTS _hand_written)
    if(NOT EXISTS "${_generated}/${_name}")
      message(FATAL_ERROR "${STEP}: ${_name}, which no generator wrote, was deleted")
    endif()
  endforeach()
endfunction()

gr_configure_library("both headers" "${_gain};${_gain_ext}")
gr_expect_units("both headers" "${_gain}" TRUE)
gr_expect_units("both headers" "${_gain_ext}" TRUE)

foreach(_name IN LISTS _hand_written)
  file(WRITE "${_generated}/${_name}" "// written by hand\n")
endforeach()

gr_configure_library("Gain_ext dropped" "${_gain}")
gr_expect_units("Gain_ext dropped" "${_gain}" TRUE)
gr_expect_units("Gain_ext dropped" "${_gain_ext}" FALSE)
gr_expect_hand_written("Gain_ext dropped")

gr_configure_library("Gain dropped, Gain_ext listed again" "${_gain_ext}")
gr_expect_units("Gain dropped, Gain_ext listed again" "${_gain}" FALSE)
gr_expect_units("Gain dropped, Gain_ext listed again" "${_gain_ext}" TRUE)
gr_expect_hand_written("Gain dropped, Gain_ext listed again")

gr_configure_library("both listed again" "${_gain};${_gain_ext}")
gr_expect_units("both listed again" "${_gain}" TRUE)
gr_expect_units("both listed again" "${_gain_ext}" TRUE)
gr_expect_hand_written("both listed again")

message(
  STATUS "the units of a dropped header are deleted, the units of a header with a longer name that starts the same "
         "stay, files no generator wrote stay, and a header listed again is generated again")
