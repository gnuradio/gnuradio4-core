# gnuradio4_precompile_headers(<target> [EXTRA_HEADERS <header>...])
#
# Precompiles the GNU Radio 4 umbrella headers for <target> as a PRIVATE precompiled header: Block.hpp, Graph.hpp and
# Scheduler.hpp — the closure every translation unit that builds and runs a flowgraph pays for. Precompiling it once per
# target reduces compile time (measured on a typical graph+scheduler TU: -10..20% with GCC; with Clang the function
# additionally passes -fpch-instantiate-templates, which also captures template instantiation — roughly -55% time and
# -50% peak memory per TU).
#
# EXTRA_HEADERS appends headers used widely across the target's sources, e.g. block-library headers:
#
# find_package(gnuradio4 REQUIRED) add_executable(myapp graph.cpp control.cpp) target_link_libraries(myapp PRIVATE
# gnuradio4::gnuradio-core) gnuradio4_precompile_headers(myapp EXTRA_HEADERS <gnuradio-4.0/basic/SignalGenerator.hpp>)
#
# Notes: - the usual PCH rules apply: all sources of <target> must share compile flags, and the PCH rebuilds when any
# header in the closure changes - independent of PCH: consumers must use the same compiler family the gnuradio4 install
# was built with — libgnuradio-core provides explicit instantiations of constrained templates (gr::pmt::Value et al.)
# whose mangled names differ between GCC and Clang, so e.g. a Clang consumer of a GCC-built install fails to link - set
# CMAKE_DISABLE_PRECOMPILE_HEADERS=ON to turn this into a no-op globally - for several targets with identical flags,
# CMake's PRECOMPILE_HEADERS_REUSE_FROM target property can share one PCH between them

function(gnuradio4_precompile_headers TARGET)
  if(NOT TARGET ${TARGET})
    message(FATAL_ERROR "gnuradio4_precompile_headers: '${TARGET}' is not a target")
  endif()

  set(multiValueArgs EXTRA_HEADERS)
  cmake_parse_arguments(
    ARG
    ""
    ""
    "${multiValueArgs}"
    ${ARGN})

  target_precompile_headers(
    ${TARGET}
    PRIVATE
    "<gnuradio-4.0/Block.hpp>"
    "<gnuradio-4.0/Graph.hpp>"
    "<gnuradio-4.0/Scheduler.hpp>"
    ${ARG_EXTRA_HEADERS})

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${TARGET} PRIVATE -fpch-instantiate-templates)
  endif()
endfunction()
