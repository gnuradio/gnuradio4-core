# Configure the rules-override fixture, with or without the caller's rules file, and check its cache.
#
# With the rules file: CMAKE_CXX_FLAGS carries the flag the file sets, CMAKE_CXX_FLAGS_RELWITHDEBINFO holds the value
# the file sets, and a try_compile project sees the definition the file adds. Without it: none of the three. In both,
# CMAKE_CXX_FLAGS_RELEASE holds the project's Release defaults, and without the rules file
# CMAKE_CXX_FLAGS_RELWITHDEBINFO holds the project's RelWithDebInfo defaults.
#
# cmake -DSOURCE_DIR=<tree> -DFIXTURE_DIR=<dir> -DWORK_DIR=<dir> -DGENERATOR=<generator> -DINITIAL_CACHE=<file>
# -DEXPECTED_RELEASE=<flags> -DEXPECTED_RELWITHDEBINFO=<flags> [-DGENERATOR_PLATFORM=<platform>]
# [-DGENERATOR_TOOLSET=<toolset>] [-DHOOK=<rules file>] -P RunRulesOverrideTest.cmake

cmake_minimum_required(VERSION 3.27)

foreach(
  _required
  SOURCE_DIR
  FIXTURE_DIR
  WORK_DIR
  GENERATOR
  INITIAL_CACHE
  EXPECTED_RELEASE
  EXPECTED_RELWITHDEBINFO)
  if(NOT DEFINED ${_required})
    message(FATAL_ERROR "RunRulesOverrideTest: ${_required} is required")
  endif()
endforeach()

set(_arguments
    -S
    "${FIXTURE_DIR}"
    -B
    "${WORK_DIR}"
    -G
    "${GENERATOR}"
    -C
    "${INITIAL_CACHE}"
    "-DGR4_SOURCE_DIR=${SOURCE_DIR}")
if(GENERATOR_PLATFORM)
  list(
    APPEND
    _arguments
    -A
    "${GENERATOR_PLATFORM}")
endif()
if(GENERATOR_TOOLSET)
  list(
    APPEND
    _arguments
    -T
    "${GENERATOR_TOOLSET}")
endif()
if(HOOK)
  list(APPEND _arguments "-DCMAKE_USER_MAKE_RULES_OVERRIDE=${HOOK}")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
execute_process(
  COMMAND ${CMAKE_COMMAND} ${_arguments}
  RESULT_VARIABLE _result
  OUTPUT_VARIABLE _output
  ERROR_VARIABLE _error)
if(NOT
   _result
   EQUAL
   0)
  message(FATAL_ERROR "the fixture configure failed:\n${_output}\n${_error}")
endif()

set(_entries
    CMAKE_CXX_FLAGS
    CMAKE_CXX_FLAGS_RELEASE
    CMAKE_CXX_FLAGS_RELWITHDEBINFO
    GR4_RULES_HOOK_IN_TRY_COMPILE)
load_cache("${WORK_DIR}" READ_WITH_PREFIX _cache_ ${_entries})
foreach(_entry IN LISTS _entries)
  message(STATUS "${_entry}=${_cache_${_entry}}")
endforeach()

set(_failures "")
set(_hasHookFlag OFF)
if(" ${_cache_CMAKE_CXX_FLAGS} " MATCHES " -DGR4_USER_RULES_HOOK ")
  set(_hasHookFlag ON)
endif()
if(HOOK)
  set(_expectedRelWithDebInfo "-DGR4_USER_RULES_HOOK_TYPE")
  if(NOT _hasHookFlag)
    string(APPEND _failures "CMAKE_CXX_FLAGS lacks -DGR4_USER_RULES_HOOK, which the caller's rules file sets\n")
  endif()
  if(NOT _cache_GR4_RULES_HOOK_IN_TRY_COMPILE)
    string(APPEND _failures "a try_compile project does not see the definition the caller's rules file adds\n")
  endif()
else()
  set(_expectedRelWithDebInfo "${EXPECTED_RELWITHDEBINFO}")
  if(_hasHookFlag)
    string(APPEND _failures "CMAKE_CXX_FLAGS carries -DGR4_USER_RULES_HOOK without the caller's rules file\n")
  endif()
  if(_cache_GR4_RULES_HOOK_IN_TRY_COMPILE)
    string(APPEND _failures "a try_compile project sees the rules-file definition without the caller's rules file\n")
  endif()
endif()
if(NOT
   _cache_CMAKE_CXX_FLAGS_RELWITHDEBINFO
   STREQUAL
   _expectedRelWithDebInfo)
  string(
    APPEND
    _failures
    "CMAKE_CXX_FLAGS_RELWITHDEBINFO is '${_cache_CMAKE_CXX_FLAGS_RELWITHDEBINFO}', "
    "not '${_expectedRelWithDebInfo}'\n")
endif()
if(NOT
   _cache_CMAKE_CXX_FLAGS_RELEASE
   STREQUAL
   EXPECTED_RELEASE)
  string(
    APPEND
    _failures
    "CMAKE_CXX_FLAGS_RELEASE is '${_cache_CMAKE_CXX_FLAGS_RELEASE}', not the project's default "
    "'${EXPECTED_RELEASE}'\n")
endif()
if(_failures)
  message(FATAL_ERROR "${_failures}")
endif()
