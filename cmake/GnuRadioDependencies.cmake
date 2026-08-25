
set(FETCH OFF)
if(EMSCRIPTEN OR GR_USE_FETCHCONTENT_DEPS)
  set(FETCH ON)
  include(FetchContent)
endif()

function(ObtainOrFindUT)
  if(FETCH)
    FetchContent_Declare(
      ut
      GIT_REPOSITORY https://github.com/boost-ext/ut.git
      GIT_TAG 53e17f25119598c6458d30351b260193096ba67e # latest tag as of 2023-04-02
      EXCLUDE_FROM_ALL)
      set(GR_FETCH_MAKE_AVAILABLE_DEPS ${GR_FETCH_MAKE_AVAILABLE_DEPS} ut PARENT_SCOPE)
  else()
    find_package(ut CONFIG QUIET)
    if(TARGET Boost::ut AND NOT TARGET ut)
      add_library(ut ALIAS Boost::ut)
    elseif(TARGET boost::ut AND NOT TARGET ut)
      add_library(ut ALIAS boost::ut)
    endif()
    if(NOT TARGET ut)
      find_path(GR_BOOST_UT_INCLUDE_DIR boost/ut.hpp)
      if(NOT GR_BOOST_UT_INCLUDE_DIR)
        message(FATAL_ERROR "Boost.UT was not found. Install libboost-ext-ut-dev, ut-devel, or a source-built Boost.UT.")
      endif()
      add_library(ut INTERFACE)
      target_include_directories(ut INTERFACE ${GR_BOOST_UT_INCLUDE_DIR})
    endif()
  endif()
  set(BOOST_UT_DISABLE_MODULE
    ON
    CACHE BOOL "Disable UT Module Support" FORCE)
  add_compile_definitions(BOOST_UT_DISABLE_MODULE)
endfunction()


function(ObtainOrFindVirSIMD)
  if(FETCH)
    FetchContent_Declare(
      vir-simd
      GIT_REPOSITORY https://github.com/mattkretz/vir-simd.git
      GIT_TAG v0.4.4
      EXCLUDE_FROM_ALL)
      set(GR_FETCH_MAKE_AVAILABLE_DEPS ${GR_FETCH_MAKE_AVAILABLE_DEPS} vir-simd PARENT_SCOPE)
  else()
    find_path(vir-simd_SOURCE_DIR vir/simd.h)
    if(NOT vir-simd_SOURCE_DIR)
      message(
        FATAL_ERROR
          "vir-simd was not found. Install the vir headers under an include path, for example /usr/local/include/vir.")
    endif()
  endif()
endfunction()

# must run after FetchContent_MakeAvailable(): a fetched vir-simd_SOURCE_DIR does not exist until then, and a file set
# whose files fall outside its base directories is a hard generate-step error rather than a warning.
function(DefineVirTarget)
  add_library(vir INTERFACE)
  add_library(gnuradio4::vir ALIAS vir)
  if(NOT FETCH)
    # the headers belong to whatever provides them, so carry only a build-time include path and stage nothing. an
    # interface file set is deliberately not used here: CMake refuses to export a target whose interface file sets are
    # not installed, and installing these would copy a second copy of somebody else's headers into our prefix.
    target_include_directories(vir SYSTEM INTERFACE $<BUILD_INTERFACE:${vir-simd_SOURCE_DIR}>)
    return()
  endif()
  # transitive quoted-include closure of <vir/simd.h> and <vir/simdize.h>, the only two vir headers GR4 includes
  # directly. an installed prefix needs the whole closure, not just the two entry points.
  set(_vir_headers
      simd.h
      simdize.h
      simd_version.h
      simd_concepts.h
      simd_permute.h
      detail.h
      constexpr_wrapper.h
      struct_reflect.h)
  list(TRANSFORM _vir_headers PREPEND "${vir-simd_SOURCE_DIR}/vir/")
  target_sources(vir
    INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${vir-simd_SOURCE_DIR}
    FILES ${_vir_headers})
  # BASE_DIRS is the whole fetched source tree, which would otherwise reach every consumer of gnuradio4::vir as a plain
  # -I under -Werror / /WX.
  set_target_properties(vir PROPERTIES SYSTEM ON)
endfunction()

function(ObtainOrFindHttpLib)
  if(FETCH)
    FetchContent_Declare(
      cpp-httplib
      GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
      GIT_TAG v0.18.1
      EXCLUDE_FROM_ALL)
    set(GR_FETCH_MAKE_AVAILABLE_DEPS ${GR_FETCH_MAKE_AVAILABLE_DEPS} cpp-httplib PARENT_SCOPE)
  else()
    find_package(httplib CONFIG QUIET)
    if(NOT TARGET httplib::httplib)
      if(NOT PkgConfig_FOUND)
        message(
          FATAL_ERROR "cpp-httplib was not found and pkg-config is unavailable. Install cpp-httplib development files.")
      endif()

      pkg_search_module(
        CPP_HTTPLIB
        QUIET
        IMPORTED_TARGET
        cpp-httplib
        httplib)
      if(NOT CPP_HTTPLIB_FOUND)
        message(FATAL_ERROR "cpp-httplib was not found. Install libcpp-httplib-dev, cpp-httplib-devel, or cpp-httplib.")
      endif()
      add_library(
        httplib::httplib
        INTERFACE
        IMPORTED
        GLOBAL)
      target_link_libraries(httplib::httplib INTERFACE PkgConfig::CPP_HTTPLIB)
    endif()
  endif()
endfunction()

set(GR_HTTP_ENABLED FALSE)

set(GR_FETCH_MAKE_AVAILABLE_DEPS)
ObtainOrFindVirSIMD()
ObtainOrFindHttpLib()
if(ENABLE_BENCH OR ENABLE_TESTING)
  ObtainOrFindUT()
endif()

if(GR_FETCH_MAKE_AVAILABLE_DEPS)
  FetchContent_MakeAvailable(${GR_FETCH_MAKE_AVAILABLE_DEPS})
endif()

DefineVirTarget()
