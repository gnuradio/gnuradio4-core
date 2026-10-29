cmake_minimum_required(VERSION 3.28)

project(gr4_core_only_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(gnuradio4 CONFIG REQUIRED)

# one program over the installed package that names only gnuradio4::gnuradio-core, builds a graph and runs it
add_executable(core_only_consumer src/consumer.cpp)
target_link_libraries(core_only_consumer PRIVATE gnuradio4::gnuradio-core)

# a link that lists no registry library fails on every linker. A link that lists it before the archive that calls it
# fails only on GNU ld with --as-needed, which keeps a shared library only when an earlier object uses it. Gold and lld
# link that order. The flag is set where the linker is GNU ld, so that failure shows too. Apple's linker has no such
# flag.
include(CheckLinkerFlag)
if(CMAKE_CXX_COMPILER_LINKER_ID)
  if(CMAKE_CXX_COMPILER_LINKER_ID STREQUAL "GNU")
    set(_core_only_as_needed ON)
  endif()
else()
  check_linker_flag(CXX "LINKER:--as-needed" _core_only_as_needed_supported)
  if(_core_only_as_needed_supported)
    set(_core_only_as_needed ON)
  endif()
endif()
if(_core_only_as_needed)
  target_link_options(core_only_consumer PRIVATE "LINKER:--as-needed")
endif()
