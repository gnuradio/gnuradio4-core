# The shared test precompiled header carries Block, Graph and Scheduler, and CMake delivers it with -include, so every
# translation unit here would parse a graph closure none of these tests builds. That is not merely wasted work: the
# closure brings gr::test into scope, which makes the unqualified test:: in qa_UncertainValue ambiguous against
# Boost.UT's own. These are metaprogramming tests; the header belongs to the tier that builds graphs.
set(GR_QA_PCH OFF)

add_ut_test(qa_fixed_string)
add_ut_test(qa_reflection)
add_ut_test(qa_formatter)
add_ut_test(qa_traits)
add_ut_test(qa_type_name)
add_ut_test(qa_UncertainValue)
add_ut_test(qa_RangesHelper)
add_ut_test(qa_simd)
target_link_libraries(qa_formatter PRIVATE gnuradio-meta)
target_link_libraries(qa_traits PRIVATE gnuradio-meta)
target_link_libraries(qa_UncertainValue PRIVATE gnuradio-meta)
target_link_libraries(qa_simd PRIVATE gnuradio-meta)
