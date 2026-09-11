# These are metaprogramming tests; the shared graph-oriented test precompiled header would add a large, unused include
# closure.
set(GR_QA_PCH OFF)

add_ut_test(qa_fixed_string)
add_ut_test(qa_reflection)
add_ut_test(qa_formatter)
add_ut_test(qa_traits)
add_ut_test(qa_type_name)
add_ut_test(qa_RangesHelper)
add_ut_test(qa_simd)
target_link_libraries(qa_formatter PRIVATE gnuradio-meta)
target_link_libraries(qa_traits PRIVATE gnuradio-meta)
target_link_libraries(qa_simd PRIVATE gnuradio-meta)
