include("${CMAKE_CURRENT_LIST_DIR}/GnuRadio4PrecompileHeaders.cmake")

# Test translation units are dominated by the Block/Graph/Scheduler closure that the generated registration units
# already precompile. A precompiled header takes roughly as long to build as it saves across a handful of translation
# units, so one header per test target would largely pay for itself and no more. The test tier shares a single header
# instead, held by an anchor target that carries the compile options add_ut_test gives its tests.
option(GR_QA_PCH "Share one precompiled header across the test executables" ON)

# Names the anchor target that owns the shared precompiled header, creating it on first use. It is built lazily because
# this module is included before the option and library targets it has to mirror exist.
#
# The anchor compiles one trivial translation unit, so the header is its only real output. Its options mirror what
# add_ut_test gives a test target, because GCC refuses a precompiled header built under different settings -- the
# optimization level among them -- and CMake pairs the header with -Winvalid-pch, so under -Werror the refusal stops the
# build.
#
# The anchor is an executable rather than an object library for the same reason: this project sets
# CMAKE_POSITION_INDEPENDENT_CODE, under which CMake gives a library -fPIC and an executable -fPIE, and the tests
# reading this header are executables. It is left out of the default targets, so the link its main() allows is one
# nothing normally asks for.
function(gr_qa_pch_anchor RESULT_VAR)
  set(_anchor gr_qa_pch)
  set(${RESULT_VAR}
      ""
      PARENT_SCOPE)
  if(NOT GR_QA_PCH OR CMAKE_DISABLE_PRECOMPILE_HEADERS)
    return()
  endif()

  if(NOT TARGET ${_anchor})
    set(_stub "${CMAKE_CURRENT_BINARY_DIR}/${_anchor}.cpp")
    file(
      CONFIGURE
      OUTPUT
      "${_stub}"
      CONTENT
      "// Anchor for the shared test precompiled header; the header is the point, not this unit.\nint main() { return 0; }\n"
    )
    add_executable(${_anchor} EXCLUDE_FROM_ALL "${_stub}")
    if(GR_QA_OPTIMIZATION_LEVEL)
      set_target_properties(${_anchor} PROPERTIES GR_QA_REDUCED_OPTIMIZATION ON)
      target_compile_options(${_anchor} PRIVATE $<$<NOT:$<CONFIG:Debug>>:${GR_QA_OPTIMIZATION_LEVEL}>)
    endif()
    target_include_directories(${_anchor} PRIVATE ${CMAKE_BINARY_DIR}/include ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
    target_link_libraries(
      ${_anchor}
      PRIVATE gnuradio-options
              gnuradio-core
              gnuradio-blocklib-core
              ut
              ${GR_TEST_HELPER_LIBRARIES})
    gnuradio4_precompile_headers(${_anchor})
  endif()

  set(${RESULT_VAR}
      ${_anchor}
      PARENT_SCOPE)
endfunction()

# Gives the shared header to the tests of the directory that has just been read, skipping any whose compile options no
# longer match the ones it was built under. A test adjusts its own options after add_ut_test returns, so the check has
# to be deferred to the end of the directory to see them. The anchor is named literally rather than passed in, because
# cmake_language(DEFER CALL) evaluates its arguments when the deferred call runs, by which time a caller's local
# variable holding the name is out of scope. The check is worth making rather than leaving to the compiler, because
# CMake passes -Winvalid-pch and this project builds -Werror, so a mismatch stops the build instead of merely compiling
# slower. Options a target inherits from a linked library are not visible here, and would still reach the compiler.
function(gr_qa_attach_shared_pch)
  get_target_property(_anchorOptions gr_qa_pch COMPILE_OPTIONS)
  get_property(
    _candidates
    DIRECTORY
    PROPERTY GR_QA_PCH_CANDIDATES)
  foreach(_test IN LISTS _candidates)
    get_target_property(_testOptions ${_test} COMPILE_OPTIONS)
    if("${_testOptions}" STREQUAL "${_anchorOptions}")
      set_target_properties(${_test} PROPERTIES PRECOMPILE_HEADERS_REUSE_FROM gr_qa_pch)
    endif()
  endforeach()
endfunction()

function(setup_test_no_asan TEST_NAME)
  target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_BINARY_DIR}/include ${CMAKE_CURRENT_BINARY_DIR})
  target_link_libraries(
    ${TEST_NAME}
    PRIVATE gnuradio-options
            gnuradio-core
            gnuradio-blocklib-core
            ut
            ${GR_TEST_HELPER_LIBRARIES})
  # gnuradio-core's Graph/BlockModel/PluginLoader reference gr_plugin_base's out-of-line destructor, which is only
  # defined in gnuradio-plugin. Tests that reach that code need it on the link line.
  if(TARGET gnuradio-plugin)
    target_link_libraries(${TEST_NAME} PRIVATE gnuradio-plugin)
  endif()
  if(MSVC)
    # boost.ut nests a lambda per test inside the suite lambda, and /Od gives every local its own slot, so the frames
    # get large. Windows reserves 1 MiB by default where Linux gives 8 MiB -- match Linux.
    target_link_options(${TEST_NAME} PRIVATE /STACK:8388608)
  endif()
  add_test(NAME ${TEST_NAME} COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} ${CMAKE_CURRENT_BINARY_DIR}/${TEST_NAME})
  if(WIN32)
    # Windows has no RPATH: the loader locates the shared libraries the tests link against via PATH only.
    set(_gr_test_dll_dirs "PATH=path_list_prepend:$<TARGET_FILE_DIR:gnuradio-blocklib-core>")
    if(TARGET gnuradio-plugin)
      list(APPEND _gr_test_dll_dirs "PATH=path_list_prepend:$<TARGET_FILE_DIR:gnuradio-plugin>")
    endif()
    set_property(
      TEST ${TEST_NAME}
      APPEND
      PROPERTY ENVIRONMENT_MODIFICATION ${_gr_test_dll_dirs})
  endif()
endfunction()

function(setup_test TEST_NAME)
  if(PYTHON_AVAILABLE)
    target_include_directories(${TEST_NAME} PRIVATE ${Python3_INCLUDE_DIRS} ${NUMPY_INCLUDE_DIR})
    target_link_libraries(${TEST_NAME} PRIVATE ${Python3_LIBRARIES})
  endif()

  setup_test_no_asan(${TEST_NAME})
endfunction()

function(add_ut_test TEST_NAME)
  add_executable(${TEST_NAME} ${TEST_NAME}.cpp)
  if(GR_QA_OPTIMIZATION_LEVEL)
    # GCC's null-dereference analysis false-positives in libstdc++'s inlined string code below -O2; the warning battery
    # reads this property.
    set_target_properties(${TEST_NAME} PROPERTIES GR_QA_REDUCED_OPTIMIZATION ON)
    target_compile_options(${TEST_NAME} PRIVATE $<$<NOT:$<CONFIG:Debug>>:${GR_QA_OPTIMIZATION_LEVEL}>)
  endif()
  gr_qa_pch_anchor(_anchor)
  if(_anchor)
    get_property(
      _candidates
      DIRECTORY
      PROPERTY GR_QA_PCH_CANDIDATES)
    if(NOT _candidates)
      cmake_language(DEFER CALL gr_qa_attach_shared_pch)
    endif()
    set_property(
      DIRECTORY
      APPEND
      PROPERTY GR_QA_PCH_CANDIDATES ${TEST_NAME})
  endif()
  setup_test(${TEST_NAME})
  set_property(
    TEST ${TEST_NAME}
    APPEND
    PROPERTY ENVIRONMENT_MODIFICATION "GNURADIO4_PLUGIN_DIRECTORIES=set:${CMAKE_CURRENT_BINARY_DIR}/plugins")
  get_property(_env GLOBAL PROPERTY _GR_TEST_ENV)
  if(_env)
    set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT "${_env}")
  endif()
  target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
endfunction()
