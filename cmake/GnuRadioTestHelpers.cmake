function(setup_test_no_asan TEST_NAME)
  target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_BINARY_DIR}/include ${CMAKE_CURRENT_BINARY_DIR})
  target_link_libraries(
    ${TEST_NAME}
    PRIVATE gnuradio-options
            gnuradio-core
            gnuradio-blocklib-core
            ut
            ${GR_TEST_HELPER_LIBRARIES})
  # gnuradio-core's Graph/BlockModel/PluginLoader reference gr_plugin_base's out-of-line destructor,
  # which is only defined in gnuradio-plugin. Tests that reach that code need it on the link line.
  if(TARGET gnuradio-plugin)
    target_link_libraries(${TEST_NAME} PRIVATE gnuradio-plugin)
  endif()
  if(MSVC)
    # boost.ut nests a lambda per test inside the suite lambda, and /Od gives every local its own slot,
    # so the frames get large. Windows reserves 1 MiB by default where Linux gives 8 MiB -- match Linux.
    target_link_options(${TEST_NAME} PRIVATE /STACK:8388608)
  endif()
  add_test(NAME ${TEST_NAME} COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} ${CMAKE_CURRENT_BINARY_DIR}/${TEST_NAME})
  if(WIN32)
    # Windows has no RPATH: the loader locates the shared libraries the tests link against via PATH only.
    set(_gr_test_dll_dirs "PATH=path_list_prepend:$<TARGET_FILE_DIR:gnuradio-blocklib-core>")
    if(TARGET gnuradio-plugin)
      list(APPEND _gr_test_dll_dirs "PATH=path_list_prepend:$<TARGET_FILE_DIR:gnuradio-plugin>")
    endif()
    set_property(TEST ${TEST_NAME} APPEND PROPERTY ENVIRONMENT_MODIFICATION ${_gr_test_dll_dirs})
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
  setup_test(${TEST_NAME})
  set_property(TEST ${TEST_NAME} APPEND PROPERTY ENVIRONMENT_MODIFICATION
                                          "GNURADIO4_PLUGIN_DIRECTORIES=set:${CMAKE_CURRENT_BINARY_DIR}/plugins")
  get_property(_env GLOBAL PROPERTY _GR_TEST_ENV)
  if(_env)
    set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT "${_env}")
  endif()
  target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
endfunction()
