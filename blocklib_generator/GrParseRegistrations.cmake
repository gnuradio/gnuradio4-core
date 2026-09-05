# Generate the block-registration translation units for one header.
#
# Run as a script, once per header, while a block library configures:
#
# cmake -DHEADER=<header.hpp> -DOUT_DIR=<generation dir> [-DSPLIT=ON] [-DREGISTRY_HEADER=<include>]
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
# * default: each macro line => one registration .cpp file.
# * SPLIT: cartesian expansion -> each block-type combination gets its own registration .cpp.
#
# The registry options name where the registrations go:
#
# * REGISTRY_INSTANCE <expression>: the registry the generated units insert into, called as <expression>(); defaults to
#   gr::globalBlockRegistry.
# * REGISTRY_HEADER <include>: the header the generated units include for that instance; defaults to
#   gnuradio-4.0/BlockRegistry.hpp.

cmake_minimum_required(VERSION 3.28)

set(GR_PR_MACRO_NAME "GR_REGISTER_BLOCK")

# A value carries commas, so a list of values cannot be joined with one; this separates the values of one expansion
# combination and cannot occur in a C++ type name.
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
if(NOT DEFINED SPLIT)
  set(SPLIT OFF)
endif()

get_filename_component(GR_PR_STEM "${HEADER}" NAME_WE)
get_filename_component(GR_PR_MODULE "${OUT_DIR}" NAME)
file(MAKE_DIRECTORY "${OUT_DIR}")

if(SPLIT)
  set(GR_PR_SPLIT_REPORT "Yes")
else()
  set(GR_PR_SPLIT_REPORT "No")
endif()
message(STATUS "parsing header: '${HEADER}' -> '${OUT_DIR}'  split: ${GR_PR_SPLIT_REPORT}")

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

# Write one registration unit: the helpers that register each block of this unit, the initializer over them, and the
# declaration and call fragments the module integrator merges.
function(
  gr_pr_write_unit
  NAME_PREFIX
  TYPES
  NAMES
  DECLARATION_TYPE)
  set(_init "gr_blocklib_init_unit_${NAME_PREFIX}")
  set(_text "// auto-generated by ${CMAKE_CURRENT_LIST_FILE}, do not edit.\n")
  string(APPEND _text "#include <${REGISTRY_HEADER}>\n#include \"${HEADER}\" // for details: ${HEADER}:1\n\n")

  set(_functions "")
  set(_position 0)
  list(LENGTH TYPES _count)
  while(_position LESS _count)
    list(
      GET
      TYPES
      ${_position}
      _type)
    list(
      GET
      NAMES
      ${_position}
      _name)
    string(
      SUBSTRING "${_name}"
                1
                -1
                _name)
    set(_function "reg_${NAME_PREFIX}_${_position}")
    list(APPEND _functions "${_function}")
    string(
      APPEND
      _text
      "\n    namespace gr { class BlockRegistry; }\n    namespace {\n        bool ${_function}(gr::BlockRegistry& registry) {\n            return gr::registerBlock<${_type}, \"${_name}\">(registry); // for details: ${REGISTRY_INSTANCE}:${HEADER}\\n\"\n        }\n    }\n"
    )
    math(EXPR _position "${_position} + 1")
  endwhile()

  string(
    APPEND
    _text
    "\nextern \"C\" {\nGNURADIO_EXPORT std::size_t ${_init}(gr::BlockRegistry& registry) {\n"
    "    std::size_t result = 0UZ; \n")
  foreach(_function IN LISTS _functions)
    string(APPEND _text "    result += ( !${_function}(registry) ? 1UZ : 0UZ );\n")
  endforeach()
  string(APPEND _text "    return result;\n}\n}\n\n")
  string(APPEND _text "auto ${_init}_invoked = ${_init}(${REGISTRY_INSTANCE}());\n")
  string(APPEND _text "// To initialize, call ${_init}\n")
  string(APPEND _text "// end of auto-generated code\n")

  message(STATUS "\t=> Generating file: '${OUT_DIR}/${NAME_PREFIX}.cpp'")
  file(WRITE "${OUT_DIR}/${NAME_PREFIX}.cpp" "${_text}")
  file(
    WRITE "${OUT_DIR}/${NAME_PREFIX}_declarations.hpp.in"
    "#ifndef HEADER_GUARD_${_init}_HPP
#define HEADER_GUARD_${_init}_HPP
extern \"C\" { ${DECLARATION_TYPE} ${_init}(gr::BlockRegistry&); }
#endif // HEADER_GUARD_${_init}_HPP
")
  file(WRITE "${OUT_DIR}/${NAME_PREFIX}_raw_calls.hpp.in" "result += !${_init}(registry);\n")
  message(STATUS "\t=> To initialize, call ${_init}")
endfunction()

file(READ "${HEADER}" GR_PR_CONTENT)

set(GR_PR_LINE_NUMBER 0)
set(GR_PR_MACRO_COUNT 0)
set(GR_PR_FILE_COUNT 0)
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

  set(GR_PR_TYPES "")
  set(GR_PR_NAMES "")
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

    if(SPLIT)
      gr_pr_write_unit(
        "${GR_PR_STEM}_${GR_PR_MACRO_COUNT}_${GR_PR_COMBINATION_INDEX}"
        "${GR_PR_TYPE}"
        "|${GR_PR_FINAL_NAME}"
        "bool")
      math(EXPR GR_PR_FILE_COUNT "${GR_PR_FILE_COUNT} + 1")
    else()
      list(APPEND GR_PR_TYPES "${GR_PR_TYPE}")
      list(APPEND GR_PR_NAMES "|${GR_PR_FINAL_NAME}")
    endif()
    math(EXPR GR_PR_COMBINATION_INDEX "${GR_PR_COMBINATION_INDEX} + 1")
  endwhile()

  if(NOT SPLIT)
    gr_pr_write_unit(
      "${GR_PR_STEM}_${GR_PR_MACRO_COUNT}"
      "${GR_PR_TYPES}"
      "${GR_PR_NAMES}"
      "std::size_t")
    math(EXPR GR_PR_FILE_COUNT "${GR_PR_FILE_COUNT} + 1")
  endif()

  math(EXPR GR_PR_MACRO_COUNT "${GR_PR_MACRO_COUNT} + 1")
endwhile()

message(STATUS "GrParseRegistrations: wrote ${GR_PR_FILE_COUNT} file(s) for ${GR_PR_MACRO_COUNT} macro definition(s).")
