set(GR_TOOLS_TEST_ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/assets)

# rungraph is tested as the program a caller runs, because its subject is the exit status and the output of a run. The
# graph it is given names blocks core's own test plugins provide, so the cases that run one are built only where those
# plugins are.
if(TARGET rungraph)
  add_ut_test(qa_RunGraph)
  target_compile_definitions(qa_RunGraph PRIVATE GR_TOOLS_TEST_ASSETS="${GR_TOOLS_TEST_ASSETS}"
                                                 GR_TOOLS_RUNGRAPH="$<TARGET_FILE:rungraph>")
  add_dependencies(qa_RunGraph rungraph)
  set_tests_properties(qa_RunGraph PROPERTIES TIMEOUT 60)
  if(INTERNAL_ENABLE_BLOCK_PLUGINS)
    target_compile_definitions(qa_RunGraph PRIVATE GR_TOOLS_CORE_TEST_PLUGINS="${CMAKE_BINARY_DIR}/core/test/plugins")
    # the fusion cases run a chain of good::identity, which GoodRegistryPlugin supplies
    add_dependencies(qa_RunGraph GoodBasePlugin GoodRegistryPlugin)
  endif()
endif()

# grinfo is tested as the program a caller runs, for the same reason. It is given core's own test plugins and, beside
# them, the directory of shared objects that register blocks without being plugins, so that both kinds of file appear in
# one report.
if(TARGET grinfo)
  add_ut_test(qa_GrInfo)
  target_compile_definitions(qa_GrInfo PRIVATE GR_TOOLS_GRINFO="$<TARGET_FILE:grinfo>")
  add_dependencies(qa_GrInfo grinfo)
  set_tests_properties(qa_GrInfo PROPERTIES TIMEOUT 60)
  if(INTERNAL_ENABLE_BLOCK_PLUGINS)
    target_compile_definitions(
      qa_GrInfo PRIVATE GR_TOOLS_CORE_TEST_PLUGINS="${CMAKE_BINARY_DIR}/core/test/plugins"
                        GR_TOOLS_TEST_BLOCK_LIBRARY="${CMAKE_BINARY_DIR}/core/test/block_library")
    add_dependencies(
      qa_GrInfo
      GoodBasePlugin
      good_math_plugin
      good_conversion_plugin
      bad_plugin
      block_library
      block_library_second)
  endif()
endif()
