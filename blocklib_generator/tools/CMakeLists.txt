cmake_minimum_required(VERSION 3.16)
project(parseReg LANGUAGES CXX)

message(STATUS "Using ${CMAKE_CXX_COMPILER_ID} to compile the gnuradio_4_0_parse_registrations tool")

# use HOST compiler to build gnuradio_4_0_parse_registrations
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_BUILD_TYPE Release)

add_executable(gnuradio_4_0_parse_registrations parse_registrations.cpp)

# GR4_USE_LIBCXX is passed through from the parent build; like there, Clang uses the default standard library (usually
# libstdc++) unless libc++ is explicitly requested
if(GR4_USE_LIBCXX AND CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
  target_compile_options(gnuradio_4_0_parse_registrations PRIVATE -stdlib=libc++)
  target_link_options(
    gnuradio_4_0_parse_registrations
    PRIVATE
    -stdlib=libc++
    -lc++)
endif()
