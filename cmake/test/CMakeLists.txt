# A configure of a small fixture that adds this tree checks that a rules file the caller names in
# CMAKE_USER_MAKE_RULES_OVERRIDE runs in the configure and in its try_compile projects, that the per-build-type values
# it sets stay in effect, and that the project's defaults apply to the others.

# The project's Release and RelWithDebInfo defaults for this compiler, read from the file that sets them.
function(gr_project_default_flags OUT_RELEASE OUT_RELWITHDEBINFO)
  include(${PROJECT_SOURCE_DIR}/cmake/FlagsOverride.cmake)
  set(${OUT_RELEASE}
      "${CMAKE_CXX_FLAGS_RELEASE_INIT}"
      PARENT_SCOPE)
  set(${OUT_RELWITHDEBINFO}
      "${CMAKE_CXX_FLAGS_RELWITHDEBINFO_INIT}"
      PARENT_SCOPE)
endfunction()
gr_project_default_flags(GR_RULES_TEST_RELEASE GR_RULES_TEST_RELWITHDEBINFO)

# The fixture's initial cache holds the compiler, the make program, the search paths and the dependency locations this
# build used. The fixture builds nothing.
set(GR_RULES_TEST_FORWARDED
    CMAKE_CXX_COMPILER
    CMAKE_MAKE_PROGRAM
    CMAKE_PREFIX_PATH
    CMAKE_FIND_ROOT_PATH
    ut_DIR
    httplib_DIR)
if(GR_USE_FETCHCONTENT_DEPS)
  # The fixture takes the fetched sources and configures them in its own tree.
  set(FETCHCONTENT_SOURCE_DIR_UT "${ut_SOURCE_DIR}")
  set(FETCHCONTENT_SOURCE_DIR_VIR-SIMD "${vir-simd_SOURCE_DIR}")
  list(
    APPEND
    GR_RULES_TEST_FORWARDED
    FETCHCONTENT_SOURCE_DIR_UT
    FETCHCONTENT_SOURCE_DIR_VIR-SIMD)
else()
  list(
    APPEND
    GR_RULES_TEST_FORWARDED
    GR_BOOST_UT_INCLUDE_DIR
    vir-simd_SOURCE_DIR)
endif()
set(GR_RULES_TEST_CACHE_TEXT "")
foreach(_option ENABLE_TESTING ENABLE_EXAMPLES ENABLE_TOOLS)
  string(APPEND GR_RULES_TEST_CACHE_TEXT "set(${_option} OFF CACHE BOOL \"\")\n")
endforeach()
set(_fetch OFF)
if(GR_USE_FETCHCONTENT_DEPS)
  set(_fetch ON)
endif()
string(APPEND GR_RULES_TEST_CACHE_TEXT "set(GR_USE_FETCHCONTENT_DEPS ${_fetch} CACHE BOOL \"\")\n")
foreach(_name IN LISTS GR_RULES_TEST_FORWARDED)
  if(${_name})
    string(APPEND GR_RULES_TEST_CACHE_TEXT "set(${_name} [==[${${_name}}]==] CACHE STRING \"\")\n")
  endif()
endforeach()
set(GR_RULES_TEST_CACHE "${CMAKE_CURRENT_BINARY_DIR}/rules_override_cache.cmake")
file(WRITE "${GR_RULES_TEST_CACHE}" "${GR_RULES_TEST_CACHE_TEXT}")

foreach(_arm hook plain)
  set(_hook "")
  if(_arm STREQUAL "hook")
    set(_hook "-DHOOK=${CMAKE_CURRENT_SOURCE_DIR}/rules_override/hook.cmake")
  endif()
  add_test(
    NAME qa_RulesOverride_${_arm}
    COMMAND
      ${CMAKE_COMMAND} "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/rules_override"
      "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/rules_override_${_arm}" "-DGENERATOR=${CMAKE_GENERATOR}"
      "-DINITIAL_CACHE=${GR_RULES_TEST_CACHE}" "-DEXPECTED_RELEASE=${GR_RULES_TEST_RELEASE}"
      "-DEXPECTED_RELWITHDEBINFO=${GR_RULES_TEST_RELWITHDEBINFO}" "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
      "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}" ${_hook} -P
      ${CMAKE_CURRENT_SOURCE_DIR}/RunRulesOverrideTest.cmake)
  set_tests_properties(qa_RulesOverride_${_arm} PROPERTIES TIMEOUT 60)
endforeach()
