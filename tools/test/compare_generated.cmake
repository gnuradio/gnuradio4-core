# Runs a document generator over a fixture and compares the result with the pinned expectation. Two steps in one script
# so that a single ctest entry covers both, and so that a generator that fails is reported as such rather than as an
# empty difference. READ_FROM is `file` or `stdin`; the two must yield the same document, which is why both compare
# against one expectation.
if(READ_FROM STREQUAL "stdin")
  execute_process(
    COMMAND "${TOOL}" --format "${FORMAT}" --title "${TITLE}" --output "${ACTUAL}" -
    INPUT_FILE "${INPUT}"
    RESULT_VARIABLE _generateStatus)
else()
  execute_process(COMMAND "${TOOL}" --format "${FORMAT}" --title "${TITLE}" --output "${ACTUAL}" "${INPUT}"
                  RESULT_VARIABLE _generateStatus)
endif()
if(NOT
   _generateStatus
   EQUAL
   0)
  message(FATAL_ERROR "${TOOL} exited ${_generateStatus} for ${INPUT}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${ACTUAL}" "${EXPECTED}" RESULT_VARIABLE _compareStatus)
if(NOT
   _compareStatus
   EQUAL
   0)
  message(FATAL_ERROR "${ACTUAL} differs from ${EXPECTED}")
endif()
