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
    add_dependencies(qa_RunGraph GoodBasePlugin)
  endif()
endif()
