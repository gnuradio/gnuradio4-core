# vir-simd installs a CMake package without a version file, so find_package(vir-simd <version>) rejects every install.
# This reads the version from the vir/simd_version.h that the found vir-simd::vir-simd target points at instead, sets
# vir-simd_VERSION in the caller's scope, and stops the configure when it is older than MINIMUM_VERSION. With
# REASON_VARIABLE it sets that variable to the problem, or to an empty string, and returns instead, so that a package
# config can report not-found rather than abort its consumer's configure.
function(gr_require_vir_simd_version MINIMUM_VERSION)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "REASON_VARIABLE" "")

  get_target_property(_include_dirs vir-simd::vir-simd INTERFACE_INCLUDE_DIRECTORIES)
  foreach(_dir IN LISTS _include_dirs)
    if(EXISTS "${_dir}/vir/simd_version.h")
      file(STRINGS "${_dir}/vir/simd_version.h" _version_define REGEX "^#define VIR_SIMD_VERSION 0x")
      break()
    endif()
  endforeach()

  # the header packs the version as one hexadecimal number, major * 0x10000 + minor * 0x100 + patch, written with digit
  # separators, e.g. 0x0'04'04 for 0.4.4
  string(REPLACE "'" "" _version_define "${_version_define}")
  if(NOT _version_define MATCHES "0x([0-9a-fA-F]+)$")
    set(_problem "vir-simd was found, but its version could not be read from vir/simd_version.h")
  else()
    math(EXPR _packed "0x${CMAKE_MATCH_1}")
    math(EXPR _major "${_packed} / 65536")
    math(EXPR _minor "(${_packed} % 65536) / 256")
    math(EXPR _patch "${_packed} % 256")
    set(_version "${_major}.${_minor}.${_patch}")
    set(vir-simd_VERSION
        "${_version}"
        PARENT_SCOPE)
    if(_version VERSION_LESS MINIMUM_VERSION)
      set(_problem "vir-simd ${_version} was found, ${MINIMUM_VERSION} or newer is required")
    endif()
  endif()

  if(ARG_REASON_VARIABLE)
    set(${ARG_REASON_VARIABLE}
        "${_problem}"
        PARENT_SCOPE)
  elseif(_problem)
    message(FATAL_ERROR "${_problem}")
  endif()
endfunction()
