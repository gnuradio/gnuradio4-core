# Run the block-registration generators over the marker fixture and compare everything they wrote with the recorded
# expectation and with each other. The generated files are concatenated in name order into one stream, each behind a
# header line naming it, so one configuration is one recorded file to read and to review. The path of the generator that
# wrote a file appears on its first line and is replaced by a fixed token, so the two generators' streams compare equal.
#
# cmake -DSCRIPT=<cmake generator> -DFIXTURE=<header> -DEXPECTED_DIR=<dir> -DWORK_DIR=<dir> -DMODE=<chunked|split>
# [-DPYTHON=<interpreter>] [-DPYTHON_GENERATOR=<script>] [-DEXTRA_HEADERS=<header>;<header>] -P RunGeneratorTest.cmake

cmake_minimum_required(VERSION 3.28)

foreach(
  _required
  SCRIPT
  FIXTURE
  EXPECTED_DIR
  WORK_DIR
  MODE)
  if(NOT DEFINED ${_required})
    message(FATAL_ERROR "RunGeneratorTest: ${_required} is required")
  endif()
endforeach()

if(MODE STREQUAL "split")
  set(_cmake_options "-DSPLIT=ON")
  set(_python_options "--split")
else()
  set(_cmake_options "-DMAX_PER_TU=16")
  set(_python_options "--max-per-tu" "16")
endif()

# Generate one header with one generator and reduce what it wrote to a single stream.
function(
  gr_generate_stream
  KIND
  TAG
  HEADER
  OUT_FILE)
  set(_generated "${WORK_DIR}/${MODE}/${TAG}/${KIND}/FixtureBlockLib")
  file(REMOVE_RECURSE "${WORK_DIR}/${MODE}/${TAG}/${KIND}")
  file(MAKE_DIRECTORY "${_generated}")

  if(KIND STREQUAL "python")
    set(_command ${PYTHON} "${PYTHON_GENERATOR}" --header "${HEADER}" --out-dir "${_generated}" ${_python_options})
    set(_generator "${PYTHON_GENERATOR}")
  else()
    set(_command ${CMAKE_COMMAND} "-DHEADER=${HEADER}" "-DOUT_DIR=${_generated}" ${_cmake_options} -P "${SCRIPT}")
    set(_generator "${SCRIPT}")
  endif()

  execute_process(
    COMMAND ${_command}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _error)
  if(NOT
     _result
     EQUAL
     0)
    message(FATAL_ERROR "the ${KIND} generator failed on ${HEADER}: ${_error}")
  endif()

  file(
    GLOB _produced
    RELATIVE "${_generated}"
    "${_generated}/*")
  list(SORT _produced)
  list(LENGTH _produced _count)
  if(_count EQUAL 0)
    message(FATAL_ERROR "the ${KIND} generator wrote nothing for ${HEADER}")
  endif()

  set(_stream "")
  foreach(_name IN LISTS _produced)
    file(READ "${_generated}/${_name}" _text)
    string(
      REPLACE "${_generator}"
              "GENERATOR"
              _text
              "${_text}")
    string(
      REPLACE "${HEADER}"
              "FIXTURE"
              _text
              "${_text}")
    string(APPEND _stream "==== ${_name} ====\n${_text}")
    if(NOT
       _text
       MATCHES
       "\n$")
      string(APPEND _stream "\n")
    endif()
  endforeach()
  file(WRITE "${OUT_FILE}" "${_stream}")
  set(GR_GENERATED_COUNT
      "${_count}"
      PARENT_SCOPE)
endfunction()

function(
  gr_compare_files
  LEFT
  RIGHT
  WHAT)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${LEFT}" "${RIGHT}" RESULT_VARIABLE _differs)
  if(NOT
     _differs
     EQUAL
     0)
    message(FATAL_ERROR "${WHAT}: ${LEFT} and ${RIGHT} differ")
  endif()
endfunction()

set(_cmake_stream "${WORK_DIR}/${MODE}/fixture-cmake.stream")
gr_generate_stream(
  cmake
  fixture
  "${FIXTURE}"
  "${_cmake_stream}")
set(_fixture_count "${GR_GENERATED_COUNT}")

set(_expected_file "${EXPECTED_DIR}/${MODE}.expected")
if(NOT EXISTS "${_expected_file}")
  message(FATAL_ERROR "no expectation recorded at ${_expected_file}")
endif()
gr_compare_files("${_expected_file}" "${_cmake_stream}" "the generated units differ from the expectation")

# The two generators are meant to be interchangeable, so one recorded expectation is enough only if they are checked
# against each other here, over the fixture and over a larger real input.
if(PYTHON AND PYTHON_GENERATOR)
  set(_python_stream "${WORK_DIR}/${MODE}/fixture-python.stream")
  gr_generate_stream(
    python
    fixture
    "${FIXTURE}"
    "${_python_stream}")
  gr_compare_files("${_cmake_stream}" "${_python_stream}" "the two generators disagree on the fixture")

  set(_extra 0)
  foreach(_header IN LISTS EXTRA_HEADERS)
    math(EXPR _extra "${_extra} + 1")
    gr_generate_stream(
      cmake
      "extra${_extra}"
      "${_header}"
      "${WORK_DIR}/${MODE}/extra${_extra}-cmake.stream")
    gr_generate_stream(
      python
      "extra${_extra}"
      "${_header}"
      "${WORK_DIR}/${MODE}/extra${_extra}-python.stream")
    gr_compare_files("${WORK_DIR}/${MODE}/extra${_extra}-cmake.stream"
                     "${WORK_DIR}/${MODE}/extra${_extra}-python.stream" "the two generators disagree on ${_header}")
  endforeach()
  message(STATUS "${MODE}: ${_fixture_count} generated files match the expectation; "
                 "both generators agree on the fixture and on ${_extra} further header(s)")
else()
  message(STATUS "${MODE}: ${_fixture_count} generated files match the expectation; "
                 "no Python interpreter, so only the CMake generator was run")
endif()
