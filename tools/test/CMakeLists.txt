set(GR_TOOLS_TEST_ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/assets)

add_ut_test(qa_GraphDoc)
target_link_libraries(qa_GraphDoc PRIVATE gnuradio-doctools)
# A refusal is the status the command exits with and the line it prints, neither of which is visible from inside the
# process, so the cases that cover one run the built program over a file they write in this directory.
add_dependencies(qa_GraphDoc graphdoc)
target_compile_definitions(
  qa_GraphDoc PRIVATE GR_TOOLS_TEST_ASSETS="${GR_TOOLS_TEST_ASSETS}" GR_TOOLS_GRAPHDOC="$<TARGET_FILE:graphdoc>"
                      GR_TOOLS_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}")

# A reader reviews the whole document, so the pinned documents are compared as files rather than asserted piecewise: a
# formatting change then shows up as a diff of the document itself. They carry the `.expected` extension because the
# document formatters that run over this repository would rewrite a file named `.md`, and a rewritten expectation pins
# nothing.
function(
  add_pinned_document_test
  TEST_NAME
  FORMAT
  READ_FROM)
  add_test(
    NAME ${TEST_NAME}
    COMMAND
      ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:graphdoc> -DINPUT=${GR_TOOLS_TEST_ASSETS}/nested_graph.yaml
      -DFORMAT=${FORMAT} "-DTITLE=Nested graph fixture" -DREAD_FROM=${READ_FROM}
      -DACTUAL=${CMAKE_CURRENT_BINARY_DIR}/${TEST_NAME}.actual
      -DEXPECTED=${GR_TOOLS_TEST_ASSETS}/nested_graph.${FORMAT}.expected -P
      ${CMAKE_CURRENT_SOURCE_DIR}/compare_generated.cmake)
  set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 60)
endfunction()

add_pinned_document_test(qa_GraphDocPinnedMarkdown md file)
add_pinned_document_test(qa_GraphDocPinnedHtml html file)
# a lone `-` names standard input, and the same graph read from there yields the same document
add_pinned_document_test(qa_GraphDocStandardInput md stdin)

# a tool asked for help says so and succeeds; a tool given an option it does not know refuses
function(add_option_handling_tests TOOL_TARGET)
  add_test(NAME qa_${TOOL_TARGET}_help COMMAND ${TOOL_TARGET} --help)
  add_test(NAME qa_${TOOL_TARGET}_unknown_option COMMAND ${TOOL_TARGET} --no-such-option)
  add_test(NAME qa_${TOOL_TARGET}_missing_value COMMAND ${TOOL_TARGET} --format)
  set_tests_properties(qa_${TOOL_TARGET}_unknown_option qa_${TOOL_TARGET}_missing_value PROPERTIES WILL_FAIL TRUE)
endfunction()

add_option_handling_tests(graphdoc)

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
    target_compile_definitions(
      qa_RunGraph PRIVATE GR_TOOLS_CORE_TEST_PLUGINS="${CMAKE_BINARY_DIR}/core/test/plugins"
                          GR_TOOLS_TEST_SCHEDULER_LIBRARY="${CMAKE_BINARY_DIR}/core/test/scheduler_library")
    # the cases read these files: GoodBasePlugin (good::fixed_source and good::cout_sink), GoodRegistryPlugin
    # (good::identity and good::start_recorder), good_math_plugin (a plugin's scheduler), bad_plugin (the plugin
    # --verbose reports as not loaded), scheduler_library (a scheduler from a shared object that is not a plugin), and
    # scheduler_library_v1 and scheduler_library_unversioned (the two builds of it that are refused)
    add_dependencies(
      qa_RunGraph
      GoodBasePlugin
      GoodRegistryPlugin
      good_math_plugin
      bad_plugin
      scheduler_library
      scheduler_library_v1
      scheduler_library_unversioned)

    # one plugin, in a directory of its own, whose block reports an error while the graph runs
    add_library(rungraph_fixture MODULE plugins/rungraph_fixture_plugin.cpp)
    target_include_directories(rungraph_fixture PRIVATE ${CMAKE_BINARY_DIR}/include)
    target_link_libraries(rungraph_fixture PRIVATE gnuradio-options gnuradio-core gnuradio-plugin)
    set_target_properties(rungraph_fixture PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fixture)
    add_dependencies(qa_RunGraph rungraph_fixture)
    target_compile_definitions(qa_RunGraph PRIVATE GR_TOOLS_FIXTURE_PLUGINS="${CMAKE_CURRENT_BINARY_DIR}/fixture")
  endif()
endif()

# grinfo is tested as the program a caller runs, for the same reason. It is given core's own test plugins and, beside
# them, the directory of shared objects that register blocks without being plugins, so that both kinds of file appear in
# one report. One case is given the plugin ABI test directory, which holds a plugin built at plugin ABI version 1.
if(TARGET grinfo)
  add_ut_test(qa_GrInfo)
  target_compile_definitions(qa_GrInfo PRIVATE GR_TOOLS_GRINFO="$<TARGET_FILE:grinfo>")
  add_dependencies(qa_GrInfo grinfo)
  set_tests_properties(qa_GrInfo PROPERTIES TIMEOUT 60)
  if(INTERNAL_ENABLE_BLOCK_PLUGINS)
    target_compile_definitions(
      qa_GrInfo
      PRIVATE GR_TOOLS_CORE_TEST_PLUGINS="${CMAKE_BINARY_DIR}/core/test/plugins"
              GR_TOOLS_TEST_BLOCK_LIBRARY="${CMAKE_BINARY_DIR}/core/test/block_library"
              GR_TOOLS_TEST_SCHEDULER_LIBRARY="${CMAKE_BINARY_DIR}/core/test/scheduler_library"
              GR_TOOLS_TEST_PLUGIN_ABI="${CMAKE_BINARY_DIR}/core/test/plugin_abi"
              GR_TOOLS_TEST_DOC_PROBE_LIBRARY="${CMAKE_BINARY_DIR}/core/test/doc_probe_library")
    add_dependencies(
      qa_GrInfo
      GoodBasePlugin
      good_math_plugin
      good_conversion_plugin
      bad_plugin
      block_library
      block_library_second
      scheduler_library
      scheduler_library_v1
      scheduler_library_unversioned
      abi_probe_plugin
      abi_probe_plugin_v1
      abi_probe_plugin_foreign_scheduler
      doc_probe_library)
    # the device cases read a block library and a plugin that register device listings of invented units
    target_compile_definitions(qa_GrInfo
                               PRIVATE GR_TOOLS_TEST_DEVICE_LISTING="${CMAKE_BINARY_DIR}/core/test/device_listing")
    add_dependencies(qa_GrInfo device_listing_library device_listing_plugin)
  endif()
endif()
