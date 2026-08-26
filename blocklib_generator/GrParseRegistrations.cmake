# Generate the block-registration translation units for one header.
#
# Run as a script, once per header, while a block library configures:
#
# cmake -DHEADER=<header.hpp> -DOUT_DIR=<generation dir> [-DSPLIT=ON] [-DMAX_PER_TU=<n>] [-DREGISTRY_HEADER=<include>]
# [-DREGISTRY_INSTANCE=<expression>] -P GrParseRegistrations.cmake
#
# The script scans the header for lines carrying
#
# GR_REGISTER_BLOCK("OptionalQuotedName", MyTemplate, (paramPack?), [ expansions ]...)
#
# for example:
#
# * GR_REGISTER_BLOCK("MyBlockName", gr::basic::Block1, ([T], [U]), [ float, double ], [int])
# * GR_REGISTER_BLOCK(gr::basic::Block0)
# * GR_REGISTER_BLOCK("blockN.hpp", gr::basic::BlockN, ([T],[U],3UZ,SomeAlgo<[T]>), [ short, int], [double])
#
# Each marker must be on one line; there is no multi-line support. A marker the script cannot parse ends the configure,
# naming the header and the line.
#
# Each registered (block, type) produces two translation units:
#
# * a DEFINITION unit, <stem>_block_<n>.cpp, which names the block type: it defines the factory that builds a
#   BlockWrapper<TBlock> and exports the registry key and alias the typed insert<TBlock>() path would derive. This is
#   the only unit that parses the block header, and the only one that materializes the framework's per-type machinery.
# * a share of a REGISTRATION unit, which declares the definition unit's exported symbol and registers it through
#   gr::insertBlockFactory(). A registration unit includes gnuradio-4.0/BlockRegistration.hpp alone and instantiates
#   nothing.
#
# Every generated symbol is qualified by the module -- the generation directory's name, which is the block library's
# name: gr_blocklib_factory_<module>_<stem>_<n> and gr_blocklib_registration_<module>_<stem>_<n> in a definition unit,
# gr_blocklib_init_unit_<module>_<unit> in a registration unit, and gr_blocklib_init_module_<module> in the integrator.
# Header basename and index alone would collide between two libraries that each carry a Gain.hpp, and the dynamic linker
# would resolve both to the definitions it saw first.
#
# Both generated initializers -- gr_blocklib_init_unit_<module>_<unit> per registration unit, and
# gr_blocklib_init_module_<module> over all of them -- return the number of blocks the call newly entered into the
# registry they are given. Registering a block the registry already holds is a no-op and not a failure, so calling an
# initializer on a registry the definition units' own static initializers have already populated returns zero and
# changes nothing. That makes initialization idempotent and safe to repeat, and still leaves a caller building its own
# registry a usable count.
#
# With GR_ENABLE_BLOCK_REGISTRY off both initializers are still defined and still link -- the registry-off configuration
# of the installed core reaches an out-of-tree block library through gnuradio-4.0/config.hpp -- and both return zero,
# having entered nothing. The library's blocks remain usable by naming their type; only lookup by registry key is gone.
#
# The switches group the registration units; definition units are always one per (block, type).
#
# * default: each macro line => one registration .cpp file.
# * SPLIT: cartesian expansion -> each block-type combination gets its own registration .cpp.
# * MAX_PER_TU <N>: balanced chunking -> all registrations of the header (across macro lines) are packed into
#   registration .cpp files of at most N entries each.
#
# The registry options name where the registrations go:
#
# * REGISTRY_INSTANCE <expression>: the registry the registration units insert into, called as <expression>(); defaults
#   to gr::globalBlockRegistry.
# * REGISTRY_HEADER <include>: the header the definition units include in place of gnuradio-4.0/BlockRegistry.hpp.

cmake_minimum_required(VERSION 3.28)

set(GR_PR_MACRO_NAME "GR_REGISTER_BLOCK")

# A value carries commas, so a list of values cannot be joined with one; these two separate the fields of a record and
# the values of one expansion combination, and cannot occur in a C++ type name.
set(GR_PR_FIELD "@GR_PR_FIELD@")
set(GR_PR_VALUE "@GR_PR_VALUE@")

# The placeholders a parameter pack may carry, in the order the expansion groups fill them.
set(GR_PR_PLACEHOLDERS
    "[T]"
    "[U]"
    "[A]"
    "[B]"
    "[X]"
    "[Y]"
    "[Z]"
    "[S]")

# Split at the commas that are not inside brackets, angle brackets or a string literal. CMake's regular expressions do
# not nest, so the depth is counted here one character at a time.
function(gr_pr_split_top_level INPUT OUT_VAR)
  string(STRIP "${INPUT}" INPUT)
  string(LENGTH "${INPUT}" _length)
  set(_tokens "")
  set(_depth 0)
  set(_in_string OFF)
  set(_token "")
  set(_index 0)
  while(_index LESS _length)
    string(
      SUBSTRING "${INPUT}"
                ${_index}
                1
                _char)
    math(EXPR _index "${_index} + 1")
    if(_in_string)
      string(APPEND _token "${_char}")
      if(_char STREQUAL "\"")
        set(_in_string OFF)
      endif()
      continue()
    endif()
    if(_char STREQUAL "\"")
      set(_in_string ON)
    elseif(
      _char STREQUAL "("
      OR _char STREQUAL "["
      OR _char STREQUAL "<")
      math(EXPR _depth "${_depth} + 1")
    elseif(
      _char STREQUAL ")"
      OR _char STREQUAL "]"
      OR _char STREQUAL ">")
      math(EXPR _depth "${_depth} - 1")
      if(_depth LESS 0)
        set(${OUT_VAR}
            "GR_PR_UNBALANCED"
            PARENT_SCOPE)
        return()
      endif()
    elseif(_char STREQUAL "," AND _depth EQUAL 0)
      string(STRIP "${_token}" _token)
      list(APPEND _tokens "${_token}")
      set(_token "")
      continue()
    endif()
    string(APPEND _token "${_char}")
  endwhile()
  if(NOT
     _depth
     EQUAL
     0
     OR _in_string)
    set(${OUT_VAR}
        "GR_PR_UNBALANCED"
        PARENT_SCOPE)
    return()
  endif()
  string(STRIP "${_token}" _token)
  list(APPEND _tokens "${_token}")
  set(${OUT_VAR}
      "${_tokens}"
      PARENT_SCOPE)
endfunction()

# Substitute the expansion values for the placeholders of a parameter pack, whose outer parentheses are dropped.
function(
  gr_pr_replace_placeholders
  PARAM_PACK
  VALUES
  OUT_VAR)
  string(
    REGEX
    REPLACE "^[( ]+"
            ""
            _param
            "${PARAM_PACK}")
  string(
    REGEX
    REPLACE "[) ]+$"
            ""
            _param
            "${_param}")
  set(_position 0)
  foreach(_value IN LISTS VALUES)
    list(LENGTH GR_PR_PLACEHOLDERS _placeholder_count)
    if(_position GREATER_EQUAL _placeholder_count)
      break()
    endif()
    list(
      GET
      GR_PR_PLACEHOLDERS
      ${_position}
      _placeholder)
    string(
      REPLACE "${_placeholder}"
              "${_value}"
              _param
              "${_param}")
    math(EXPR _position "${_position} + 1")
  endforeach()
  set(${OUT_VAR}
      "${_param}"
      PARENT_SCOPE)
endfunction()

function(
  gr_pr_registration_symbol
  STEM
  INDEX
  OUT_VAR)
  set(${OUT_VAR}
      "gr_blocklib_registration_${GR_PR_MODULE}_${STEM}_${INDEX}"
      PARENT_SCOPE)
endfunction()

function(
  gr_pr_factory_symbol
  STEM
  INDEX
  OUT_VAR)
  set(${OUT_VAR}
      "gr_blocklib_factory_${GR_PR_MODULE}_${STEM}_${INDEX}"
      PARENT_SCOPE)
endfunction()

if(NOT DEFINED HEADER OR NOT DEFINED OUT_DIR)
  message(FATAL_ERROR "GrParseRegistrations: HEADER and OUT_DIR are required")
endif()
if(NOT EXISTS "${HEADER}")
  message(FATAL_ERROR "GrParseRegistrations: '${HEADER}' not found")
endif()
if(NOT DEFINED REGISTRY_HEADER)
  set(REGISTRY_HEADER "gnuradio-4.0/BlockRegistry.hpp")
endif()
if(NOT DEFINED REGISTRY_INSTANCE)
  set(REGISTRY_INSTANCE "gr::globalBlockRegistry")
endif()
if(NOT DEFINED MAX_PER_TU)
  set(MAX_PER_TU 0)
endif()
if(NOT DEFINED SPLIT)
  set(SPLIT OFF)
endif()
if(SPLIT AND MAX_PER_TU GREATER 0)
  message(FATAL_ERROR "GrParseRegistrations: SPLIT and MAX_PER_TU are mutually exclusive")
endif()

get_filename_component(GR_PR_STEM "${HEADER}" NAME_WE)
get_filename_component(GR_PR_MODULE "${OUT_DIR}" NAME)
file(MAKE_DIRECTORY "${OUT_DIR}")

if(SPLIT)
  set(GR_PR_SPLIT_REPORT "Yes")
else()
  set(GR_PR_SPLIT_REPORT "No")
endif()
message(STATUS "parsing header: '${HEADER}' -> '${OUT_DIR}'  split: ${GR_PR_SPLIT_REPORT}  max-per-tu: ${MAX_PER_TU}")

set(GR_PR_INTEGRATOR_SOURCE "${OUT_DIR}/integrator.cpp")
if(NOT EXISTS "${GR_PR_INTEGRATOR_SOURCE}")
  message(STATUS "\t=> Generating file: '${GR_PR_INTEGRATOR_SOURCE}'")
  file(
    WRITE "${GR_PR_INTEGRATOR_SOURCE}"
    "
            #include <gnuradio-4.0/BlockRegistry.hpp>

            #include \"declarations.hpp\"

            extern \"C\" {
                GNURADIO_EXPORT
                std::size_t gr_blocklib_init_module_${GR_PR_MODULE}(gr::BlockRegistry& registry) {
                    std::size_t result = 0UZ;
                    #include \"raw_calls.hpp\"
                    return result;
                }
            }
")
endif()

set(GR_PR_INTEGRATOR_HEADER "${OUT_DIR}/${GR_PR_MODULE}.hpp")
if(NOT EXISTS "${GR_PR_INTEGRATOR_HEADER}")
  message(STATUS "\t=> Generating file: '${GR_PR_INTEGRATOR_HEADER}'")
  file(
    WRITE "${GR_PR_INTEGRATOR_HEADER}"
    "
            #ifndef GR_BLOCKLIB_INIT_MODULE_${GR_PR_MODULE}
            #define GR_BLOCKLIB_INIT_MODULE_${GR_PR_MODULE}
            namespace gr { class BlockRegistry; }

            extern \"C\" {
                GNURADIO_EXPORT
                std::size_t gr_blocklib_init_module_${GR_PR_MODULE}(gr::BlockRegistry& registry);
            }

            namespace gr::blocklib {
                inline
                std::size_t init${GR_PR_MODULE}(gr::BlockRegistry& registry) {
                    return gr_blocklib_init_module_${GR_PR_MODULE}(registry);
                }
            }
            #endif
")
endif()

# Read the header and cut it into lines by hand. A CMake list is not usable here: its separator is escaped by an
# unbalanced '[' or ']' anywhere in the text, which C++ source carries freely, and the lines after such a character
# would merge into one and shift every line number reported below.
file(READ "${HEADER}" GR_PR_CONTENT)

# every registration of this header, in source order, as <type><FIELD><name><FIELD><line><FIELD><macro index>
set(GR_PR_PENDING "")
set(GR_PR_LINE_NUMBER 0)
set(GR_PR_MACRO_COUNT 0)
set(GR_PR_AT_END OFF)

while(NOT GR_PR_AT_END)
  string(FIND "${GR_PR_CONTENT}" "\n" GR_PR_NEWLINE)
  if(GR_PR_NEWLINE LESS 0)
    set(GR_PR_LINE "${GR_PR_CONTENT}")
    set(GR_PR_AT_END ON)
  else()
    string(
      SUBSTRING "${GR_PR_CONTENT}"
                0
                ${GR_PR_NEWLINE}
                GR_PR_LINE)
    math(EXPR GR_PR_NEWLINE "${GR_PR_NEWLINE} + 1")
    string(
      SUBSTRING "${GR_PR_CONTENT}"
                ${GR_PR_NEWLINE}
                -1
                GR_PR_CONTENT)
  endif()
  math(EXPR GR_PR_LINE_NUMBER "${GR_PR_LINE_NUMBER} + 1")
  string(
    REGEX
    REPLACE "\r$"
            ""
            GR_PR_LINE
            "${GR_PR_LINE}")
  string(STRIP "${GR_PR_LINE}" GR_PR_TRIMMED)
  if(GR_PR_TRIMMED STREQUAL ""
     OR GR_PR_TRIMMED MATCHES "^//"
     OR NOT
        GR_PR_TRIMMED
        MATCHES
        "${GR_PR_MACRO_NAME}")
    continue()
  endif()

  message(STATUS "\tfound macro on line ${GR_PR_LINE_NUMBER}: '${GR_PR_TRIMMED}'")

  string(FIND "${GR_PR_TRIMMED}" "${GR_PR_MACRO_NAME}" GR_PR_MACRO_POSITION)
  string(LENGTH "${GR_PR_MACRO_NAME}" GR_PR_MACRO_LENGTH)
  math(EXPR GR_PR_AFTER_MACRO "${GR_PR_MACRO_POSITION} + ${GR_PR_MACRO_LENGTH}")
  string(
    SUBSTRING "${GR_PR_TRIMMED}"
              ${GR_PR_AFTER_MACRO}
              -1
              GR_PR_REST)
  string(FIND "${GR_PR_REST}" "(" GR_PR_OPEN)
  if(GR_PR_OPEN LESS 0)
    message(FATAL_ERROR "${HEADER}:${GR_PR_LINE_NUMBER}: missing '(' after ${GR_PR_MACRO_NAME}")
  endif()
  string(FIND "${GR_PR_TRIMMED}" ")" GR_PR_CLOSE REVERSE)
  math(EXPR GR_PR_CONTENT_START "${GR_PR_AFTER_MACRO} + ${GR_PR_OPEN} + 1")
  if(GR_PR_CLOSE LESS_EQUAL GR_PR_CONTENT_START)
    message(FATAL_ERROR "${HEADER}:${GR_PR_LINE_NUMBER}: missing ')' after ${GR_PR_MACRO_NAME}")
  endif()
  math(EXPR GR_PR_CONTENT_LENGTH "${GR_PR_CLOSE} - ${GR_PR_CONTENT_START}")
  string(
    SUBSTRING "${GR_PR_TRIMMED}"
              ${GR_PR_CONTENT_START}
              ${GR_PR_CONTENT_LENGTH}
              GR_PR_BODY)

  gr_pr_split_top_level("${GR_PR_BODY}" GR_PR_PARTS)
  if(GR_PR_PARTS STREQUAL "GR_PR_UNBALANCED")
    message(FATAL_ERROR "${HEADER}:${GR_PR_LINE_NUMBER}: mismatched bracket in the macro body")
  endif()

  list(LENGTH GR_PR_PARTS GR_PR_PART_COUNT)
  set(GR_PR_PART_INDEX 0)
  set(GR_PR_BASE_NAME "")
  list(
    GET
    GR_PR_PARTS
    0
    GR_PR_FIRST)
  if(GR_PR_FIRST MATCHES "^\".*\"$")
    string(LENGTH "${GR_PR_FIRST}" GR_PR_FIRST_LENGTH)
    math(EXPR GR_PR_FIRST_LENGTH "${GR_PR_FIRST_LENGTH} - 2")
    string(
      SUBSTRING "${GR_PR_FIRST}"
                1
                ${GR_PR_FIRST_LENGTH}
                GR_PR_BASE_NAME)
    set(GR_PR_PART_INDEX 1)
  endif()

  if(GR_PR_PART_INDEX GREATER_EQUAL GR_PR_PART_COUNT)
    message(FATAL_ERROR "${HEADER}:${GR_PR_LINE_NUMBER}: missing the block type argument")
  endif()
  list(
    GET
    GR_PR_PARTS
    ${GR_PR_PART_INDEX}
    GR_PR_TEMPLATE_NAME)
  math(EXPR GR_PR_PART_INDEX "${GR_PR_PART_INDEX} + 1")

  set(GR_PR_PARAM_PACK "")
  if(GR_PR_PART_INDEX LESS GR_PR_PART_COUNT)
    list(
      GET
      GR_PR_PARTS
      ${GR_PR_PART_INDEX}
      GR_PR_PARAM_PACK)
    math(EXPR GR_PR_PART_INDEX "${GR_PR_PART_INDEX} + 1")
  endif()

  # the remainder are the expansion groups, each a bracketed list of concrete types
  set(GR_PR_GROUPS "")
  while(GR_PR_PART_INDEX LESS GR_PR_PART_COUNT)
    list(
      GET
      GR_PR_PARTS
      ${GR_PR_PART_INDEX}
      GR_PR_CHUNK)
    math(EXPR GR_PR_PART_INDEX "${GR_PR_PART_INDEX} + 1")
    if(GR_PR_CHUNK MATCHES "^\\[.*\\]$")
      string(LENGTH "${GR_PR_CHUNK}" GR_PR_CHUNK_LENGTH)
      math(EXPR GR_PR_CHUNK_LENGTH "${GR_PR_CHUNK_LENGTH} - 2")
      string(
        SUBSTRING "${GR_PR_CHUNK}"
                  1
                  ${GR_PR_CHUNK_LENGTH}
                  GR_PR_CHUNK)
      string(STRIP "${GR_PR_CHUNK}" GR_PR_CHUNK)
    endif()
    gr_pr_split_top_level("${GR_PR_CHUNK}" GR_PR_VALUES)
    if(GR_PR_VALUES STREQUAL "GR_PR_UNBALANCED")
      message(FATAL_ERROR "${HEADER}:${GR_PR_LINE_NUMBER}: mismatched bracket in '${GR_PR_CHUNK}'")
    endif()
    set(GR_PR_GROUP "")
    foreach(GR_PR_VALUE_ITEM IN LISTS GR_PR_VALUES)
      if(NOT
         GR_PR_VALUE_ITEM
         STREQUAL
         "")
        if(GR_PR_GROUP STREQUAL "")
          set(GR_PR_GROUP "${GR_PR_VALUE_ITEM}")
        else()
          set(GR_PR_GROUP "${GR_PR_GROUP}${GR_PR_VALUE}${GR_PR_VALUE_ITEM}")
        endif()
      endif()
    endforeach()
    if(NOT
       GR_PR_GROUP
       STREQUAL
       "")
      list(APPEND GR_PR_GROUPS "${GR_PR_GROUP}")
    endif()
  endwhile()

  # cartesian product over the groups, the last group varying fastest
  set(GR_PR_COMBINATIONS "")
  set(GR_PR_HAVE_GROUPS OFF)
  foreach(GR_PR_GROUP IN LISTS GR_PR_GROUPS)
    string(
      REPLACE "${GR_PR_VALUE}"
              ";"
              GR_PR_VALUES
              "${GR_PR_GROUP}")
    set(GR_PR_NEXT "")
    if(NOT GR_PR_HAVE_GROUPS)
      foreach(GR_PR_VALUE_ITEM IN LISTS GR_PR_VALUES)
        list(APPEND GR_PR_NEXT "${GR_PR_VALUE_ITEM}")
      endforeach()
      set(GR_PR_HAVE_GROUPS ON)
    else()
      foreach(GR_PR_PREFIX IN LISTS GR_PR_COMBINATIONS)
        foreach(GR_PR_VALUE_ITEM IN LISTS GR_PR_VALUES)
          list(APPEND GR_PR_NEXT "${GR_PR_PREFIX}${GR_PR_VALUE}${GR_PR_VALUE_ITEM}")
        endforeach()
      endforeach()
    endif()
    set(GR_PR_COMBINATIONS "${GR_PR_NEXT}")
  endforeach()
  if(NOT GR_PR_HAVE_GROUPS)
    set(GR_PR_COMBINATIONS "")
    set(GR_PR_COMBINATION_COUNT 1)
  else()
    list(LENGTH GR_PR_COMBINATIONS GR_PR_COMBINATION_COUNT)
  endif()

  set(GR_PR_COMBINATION_INDEX 0)
  while(GR_PR_COMBINATION_INDEX LESS GR_PR_COMBINATION_COUNT)
    if(GR_PR_HAVE_GROUPS)
      list(
        GET
        GR_PR_COMBINATIONS
        ${GR_PR_COMBINATION_INDEX}
        GR_PR_COMBINATION)
      string(
        REPLACE "${GR_PR_VALUE}"
                ";"
                GR_PR_VARS
                "${GR_PR_COMBINATION}")
    else()
      set(GR_PR_VARS "")
    endif()
    math(EXPR GR_PR_COMBINATION_INDEX "${GR_PR_COMBINATION_INDEX} + 1")

    gr_pr_replace_placeholders("${GR_PR_PARAM_PACK}" "${GR_PR_VARS}" GR_PR_REPLACED)
    if(GR_PR_BASE_NAME STREQUAL "" OR GR_PR_REPLACED STREQUAL "")
      set(GR_PR_FINAL_NAME "${GR_PR_BASE_NAME}")
    else()
      set(GR_PR_FINAL_NAME "${GR_PR_BASE_NAME}<${GR_PR_REPLACED}>")
    endif()
    if(GR_PR_REPLACED STREQUAL "")
      set(GR_PR_TYPE "${GR_PR_TEMPLATE_NAME}")
    else()
      set(GR_PR_TYPE "${GR_PR_TEMPLATE_NAME}<${GR_PR_REPLACED}>")
    endif()
    list(
      APPEND
      GR_PR_PENDING
      "${GR_PR_TYPE}${GR_PR_FIELD}${GR_PR_FINAL_NAME}${GR_PR_FIELD}${GR_PR_LINE_NUMBER}${GR_PR_FIELD}${GR_PR_MACRO_COUNT}"
    )
  endwhile()

  math(EXPR GR_PR_MACRO_COUNT "${GR_PR_MACRO_COUNT} + 1")
endwhile()

list(LENGTH GR_PR_PENDING GR_PR_TOTAL)
set(GR_PR_FILE_COUNT 0)

# the only unit that names the block type, and so the only one that materializes it
set(GR_PR_INDEX 0)
while(GR_PR_INDEX LESS GR_PR_TOTAL)
  list(
    GET
    GR_PR_PENDING
    ${GR_PR_INDEX}
    GR_PR_RECORD)
  string(
    REPLACE "${GR_PR_FIELD}"
            ";"
            GR_PR_RECORD
            "${GR_PR_RECORD}")
  list(
    GET
    GR_PR_RECORD
    0
    GR_PR_TYPE)
  list(
    GET
    GR_PR_RECORD
    1
    GR_PR_FINAL_NAME)
  list(
    GET
    GR_PR_RECORD
    2
    GR_PR_RECORD_LINE)

  gr_pr_factory_symbol("${GR_PR_STEM}" ${GR_PR_INDEX} GR_PR_FACTORY)
  gr_pr_registration_symbol("${GR_PR_STEM}" ${GR_PR_INDEX} GR_PR_REGISTRATION)
  set(GR_PR_DEFINITION_FILE "${OUT_DIR}/${GR_PR_STEM}_block_${GR_PR_INDEX}.cpp")
  message(STATUS "\t=> Generating file: '${GR_PR_DEFINITION_FILE}'")
  file(
    WRITE "${GR_PR_DEFINITION_FILE}"
    "// auto-generated by ${CMAKE_CURRENT_LIST_FILE}, do not edit.
#include <${REGISTRY_HEADER}>
#include \"${HEADER}\" // for details: ${HEADER}:${GR_PR_RECORD_LINE}

#ifdef GR_ENABLE_BLOCK_REGISTRY

std::unique_ptr<gr::BlockModel> ${GR_PR_FACTORY}(gr::property_map params) { return std::make_unique<gr::BlockWrapper<${GR_PR_TYPE}>>(std::move(params)); }

const gr::BlockRegistration& ${GR_PR_REGISTRATION}() {
    static const gr::BlockRegistration registration = gr::makeBlockRegistration<${GR_PR_TYPE}, \"${GR_PR_FINAL_NAME}\">(&${GR_PR_FACTORY});
    return registration;
}

#endif // GR_ENABLE_BLOCK_REGISTRY
// end of auto-generated code
")
  math(EXPR GR_PR_FILE_COUNT "${GR_PR_FILE_COUNT} + 1")
  math(EXPR GR_PR_INDEX "${GR_PR_INDEX} + 1")
endwhile()

# Group the registrations into declaration-only units: balanced chunks when MAX_PER_TU is set, one unit per registration
# when SPLIT is set, one unit per macro line otherwise.
set(GR_PR_UNIT_NAMES "")
set(GR_PR_UNIT_MEMBERS "")
if(MAX_PER_TU GREATER 0 AND GR_PR_TOTAL GREATER 0)
  math(EXPR GR_PR_CHUNK_COUNT "(${GR_PR_TOTAL} + ${MAX_PER_TU} - 1) / ${MAX_PER_TU}")
  set(GR_PR_NEXT_INDEX 0)
  set(GR_PR_CHUNK 0)
  while(GR_PR_CHUNK LESS GR_PR_CHUNK_COUNT)
    math(EXPR GR_PR_CHUNK_SIZE "${GR_PR_TOTAL} / ${GR_PR_CHUNK_COUNT}")
    math(EXPR GR_PR_REMAINDER "${GR_PR_TOTAL} % ${GR_PR_CHUNK_COUNT}")
    if(GR_PR_CHUNK LESS GR_PR_REMAINDER)
      math(EXPR GR_PR_CHUNK_SIZE "${GR_PR_CHUNK_SIZE} + 1")
    endif()
    set(GR_PR_MEMBERS "")
    set(GR_PR_TAKEN 0)
    while(GR_PR_TAKEN LESS GR_PR_CHUNK_SIZE)
      list(APPEND GR_PR_MEMBERS "${GR_PR_NEXT_INDEX}")
      math(EXPR GR_PR_NEXT_INDEX "${GR_PR_NEXT_INDEX} + 1")
      math(EXPR GR_PR_TAKEN "${GR_PR_TAKEN} + 1")
    endwhile()
    list(APPEND GR_PR_UNIT_NAMES "${GR_PR_STEM}_${GR_PR_CHUNK}")
    string(
      REPLACE ";"
              "${GR_PR_VALUE}"
              GR_PR_MEMBERS
              "${GR_PR_MEMBERS}")
    list(APPEND GR_PR_UNIT_MEMBERS "${GR_PR_MEMBERS}")
    math(EXPR GR_PR_CHUNK "${GR_PR_CHUNK} + 1")
  endwhile()
else()
  set(GR_PR_LOCAL_INDEX 0)
  set(GR_PR_PREVIOUS_MACRO "")
  set(GR_PR_INDEX 0)
  while(GR_PR_INDEX LESS GR_PR_TOTAL)
    list(
      GET
      GR_PR_PENDING
      ${GR_PR_INDEX}
      GR_PR_RECORD)
    string(
      REPLACE "${GR_PR_FIELD}"
              ";"
              GR_PR_RECORD
              "${GR_PR_RECORD}")
    list(
      GET
      GR_PR_RECORD
      3
      GR_PR_MACRO_INDEX)
    if(GR_PR_INDEX GREATER 0 AND GR_PR_PREVIOUS_MACRO STREQUAL GR_PR_MACRO_INDEX)
      math(EXPR GR_PR_LOCAL_INDEX "${GR_PR_LOCAL_INDEX} + 1")
    else()
      set(GR_PR_LOCAL_INDEX 0)
    endif()
    set(GR_PR_PREVIOUS_MACRO "${GR_PR_MACRO_INDEX}")

    if(SPLIT)
      list(APPEND GR_PR_UNIT_NAMES "${GR_PR_STEM}_${GR_PR_MACRO_INDEX}_${GR_PR_LOCAL_INDEX}")
      list(APPEND GR_PR_UNIT_MEMBERS "${GR_PR_INDEX}")
    elseif(GR_PR_LOCAL_INDEX EQUAL 0)
      list(APPEND GR_PR_UNIT_NAMES "${GR_PR_STEM}_${GR_PR_MACRO_INDEX}")
      list(APPEND GR_PR_UNIT_MEMBERS "${GR_PR_INDEX}")
    else()
      list(POP_BACK GR_PR_UNIT_MEMBERS GR_PR_MEMBERS)
      list(APPEND GR_PR_UNIT_MEMBERS "${GR_PR_MEMBERS}${GR_PR_VALUE}${GR_PR_INDEX}")
    endif()
    math(EXPR GR_PR_INDEX "${GR_PR_INDEX} + 1")
  endwhile()
endif()

# declaration-only: a registration unit names no block type, so it instantiates nothing
list(LENGTH GR_PR_UNIT_NAMES GR_PR_UNIT_COUNT)
set(GR_PR_UNIT 0)
while(GR_PR_UNIT LESS GR_PR_UNIT_COUNT)
  list(
    GET
    GR_PR_UNIT_NAMES
    ${GR_PR_UNIT}
    GR_PR_UNIT_NAME)
  list(
    GET
    GR_PR_UNIT_MEMBERS
    ${GR_PR_UNIT}
    GR_PR_MEMBERS)
  string(
    REPLACE "${GR_PR_VALUE}"
            ";"
            GR_PR_MEMBERS
            "${GR_PR_MEMBERS}")
  set(GR_PR_INIT "gr_blocklib_init_unit_${GR_PR_MODULE}_${GR_PR_UNIT_NAME}")

  set(GR_PR_TEXT "// auto-generated by ${CMAKE_CURRENT_LIST_FILE}, do not edit.\n")
  string(APPEND GR_PR_TEXT "#include <gnuradio-4.0/BlockRegistration.hpp>\n\n#include <cstddef>\n\n")
  string(APPEND GR_PR_TEXT "#ifdef GR_ENABLE_BLOCK_REGISTRY\n")
  foreach(GR_PR_MEMBER IN LISTS GR_PR_MEMBERS)
    list(
      GET
      GR_PR_PENDING
      ${GR_PR_MEMBER}
      GR_PR_RECORD)
    string(
      REPLACE "${GR_PR_FIELD}"
              ";"
              GR_PR_RECORD
              "${GR_PR_RECORD}")
    list(
      GET
      GR_PR_RECORD
      0
      GR_PR_TYPE)
    list(
      GET
      GR_PR_RECORD
      2
      GR_PR_RECORD_LINE)
    gr_pr_registration_symbol("${GR_PR_STEM}" ${GR_PR_MEMBER} GR_PR_REGISTRATION)
    string(
      APPEND
      GR_PR_TEXT
      "extern const gr::BlockRegistration& ${GR_PR_REGISTRATION}(); // ${GR_PR_TYPE} -- for details: "
      "${HEADER}:${GR_PR_RECORD_LINE}\n")
  endforeach()
  string(APPEND GR_PR_TEXT "#endif // GR_ENABLE_BLOCK_REGISTRY\n\n")

  # The initializer is defined in every configuration: the module integrator calls it unconditionally, and a consumer
  # that builds this library against an installed core with the registry off must still link. With the registry off
  # there is nothing to insert, so it enters no block and returns zero, which is what the contract promises for a
  # registry that already holds every entry.
  string(
    APPEND
    GR_PR_TEXT
    "extern \"C\" {\nGNURADIO_EXPORT std::size_t ${GR_PR_INIT}([[maybe_unused]] "
    "gr::BlockRegistry& registry) {\n    std::size_t result = 0UZ;\n#ifdef GR_ENABLE_BLOCK_REGISTRY\n")
  foreach(GR_PR_MEMBER IN LISTS GR_PR_MEMBERS)
    gr_pr_registration_symbol("${GR_PR_STEM}" ${GR_PR_MEMBER} GR_PR_REGISTRATION)
    string(APPEND GR_PR_TEXT
           "    result += ( gr::insertBlockFactory(registry, ${GR_PR_REGISTRATION}()) ? 1UZ : 0UZ );\n")
  endforeach()
  string(APPEND GR_PR_TEXT "#endif // GR_ENABLE_BLOCK_REGISTRY\n    return result;\n}\n}\n\n")
  string(APPEND GR_PR_TEXT "#ifdef GR_ENABLE_BLOCK_REGISTRY\n")
  string(APPEND GR_PR_TEXT "auto ${GR_PR_INIT}_invoked = ${GR_PR_INIT}(${REGISTRY_INSTANCE}());\n")
  string(APPEND GR_PR_TEXT "#endif // GR_ENABLE_BLOCK_REGISTRY\n\n")
  string(APPEND GR_PR_TEXT "// To initialize, call ${GR_PR_INIT}\n")
  string(APPEND GR_PR_TEXT "// end of auto-generated code\n")

  message(STATUS "\t=> Generating file: '${OUT_DIR}/${GR_PR_UNIT_NAME}.cpp'")
  file(WRITE "${OUT_DIR}/${GR_PR_UNIT_NAME}.cpp" "${GR_PR_TEXT}")
  file(
    WRITE "${OUT_DIR}/${GR_PR_UNIT_NAME}_declarations.hpp.in"
    "#ifndef HEADER_GUARD_${GR_PR_INIT}_HPP
#define HEADER_GUARD_${GR_PR_INIT}_HPP
extern \"C\" { std::size_t ${GR_PR_INIT}(gr::BlockRegistry&); }
#endif // HEADER_GUARD_${GR_PR_INIT}_HPP
")
  file(WRITE "${OUT_DIR}/${GR_PR_UNIT_NAME}_raw_calls.hpp.in" "result += ${GR_PR_INIT}(registry);\n")
  message(STATUS "\t=> To initialize, call ${GR_PR_INIT}")
  math(EXPR GR_PR_FILE_COUNT "${GR_PR_FILE_COUNT} + 1")
  math(EXPR GR_PR_UNIT "${GR_PR_UNIT} + 1")
endwhile()

message(STATUS "GrParseRegistrations: wrote ${GR_PR_FILE_COUNT} file(s) for ${GR_PR_MACRO_COUNT} macro definition(s).")
