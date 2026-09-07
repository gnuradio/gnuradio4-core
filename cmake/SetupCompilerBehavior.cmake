

# apply known LSAN suppressions to all tests
if(ADDRESS_SANITIZER OR UB_SANITIZER)
  set(ASAN_SUPPRESSION_FILE "${CMAKE_SOURCE_DIR}/cmake/asan_suppressions.supp")
  if(EXISTS "${ASAN_SUPPRESSION_FILE}")
    set(ENV_ASAN_OPTIONS "ASAN_OPTIONS=suppressions=${ASAN_SUPPRESSION_FILE}")
    message(STATUS "LeakSanitizer suppression enabled: ${ASAN_SUPPRESSION_FILE}")
  else()
    message(WARNING "AddressSanitizer suppression file not found: ${ASAN_SUPPRESSION_FILE}")
  endif()
  set(LSAN_SUPPRESSION_FILE "${CMAKE_SOURCE_DIR}/cmake/lsan_suppressions.supp")
  if(EXISTS "${LSAN_SUPPRESSION_FILE}")
    set(ENV_LSAN_OPTIONS "LSAN_OPTIONS=suppressions=${LSAN_SUPPRESSION_FILE}")
    message(STATUS "LeakSanitizer suppression enabled: ${LSAN_SUPPRESSION_FILE}")
  else()
    message(WARNING "LeakSanitizer suppression file not found: ${LSAN_SUPPRESSION_FILE}")
  endif()
  set_property(GLOBAL PROPERTY _GR_TEST_ENV "${ENV_ASAN_OPTIONS};${ENV_LSAN_OPTIONS}")
endif()

# default explicit compiler and linker flag optimisation per build type
if(NOT MSVC)
  # optional:  -fno-inline-functions -fvar-tracking-assignments
  set(CMAKE_CXX_FLAGS_DEBUG "-Og -g1 -gz -DDEBUG -fno-omit-frame-pointer -ffunction-sections -fdata-sections")
  set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g0 -DNDEBUG -ffunction-sections -fdata-sections")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g1 -gz -DNDEBUG -ffunction-sections -fdata-sections")
  set(CMAKE_CXX_FLAGS_RELWITHASSERT "-O2 -DASSERT_ENABLED -ffunction-sections -fdata-sections")
  # '-s': strip debug symbols, 'EMBEDDED is used to disable/minimise code features for embedded systems (e.g. code-size,
  # console printouts, etc.)
  set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -g0 -DNDEBUG -DEMBEDDED -ffunction-sections -fdata-sections")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} -s")
  endif()

  if(GR4_USE_THIN_ARCHIVES)
    set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> crT <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_CXX_ARCHIVE_APPEND "<CMAKE_AR> rT <TARGET> <LINK_FLAGS> <OBJECTS>")
  else()
    set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> cr <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_CXX_ARCHIVE_APPEND "<CMAKE_AR> r <TARGET> <LINK_FLAGS> <OBJECTS>")
  endif()

  if(APPLE)
    # macOS ld uses -dead_strip instead of --gc-sections; -ffunction-sections/-fdata-sections are still valid for Clang
    set(_LD_GC_SECTIONS "-Wl,-dead_strip -ffunction-sections -fdata-sections")
  else()
    set(_LD_GC_SECTIONS "-Wl,--gc-sections -ffunction-sections -fdata-sections")
  endif()

  if(APPLE)
    # macOS Mach-O two-level namespace duplicates singletons (e.g. globalBlockRegistry()) when a static library is
    # linked into both the executable and shared plugins; -flat_namespace forces ELF-like single-level lookup ensuring
    # one instance. -stack_size 8 MiB matches the Linux default — macOS ARM64 non-main threads default to 512 KiB.
    string(APPEND _LD_GC_SECTIONS " -Wl,-flat_namespace")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-stack_size,0x800000")
  endif()

  # prefer mold > lld > system linker (Linux/non-Emscripten only) --icf=safe (Identical Code Folding) is disabled under
  # sanitizers to avoid merging functions that sanitiser instrumentation needs to track separately
  set(_USE_ICF TRUE)
  if(ADDRESS_SANITIZER
     OR UB_SANITIZER
     OR THREAD_SANITIZER)
    set(_USE_ICF FALSE)
  endif()

  if(NOT
     CMAKE_SYSTEM_NAME
     STREQUAL
     "Emscripten"
     AND NOT APPLE)
    set(_mold_usable FALSE)
    if(USE_MOLD_LINKER)
      find_program(MOLD_BIN ld.mold)
      if(MOLD_BIN)
        # mold < 3.0 does not parse the AS_NEEDED() directive used by some modern libatomic stubs (observed with GCC
        # 16's libatomic_asneeded.so; affects any compiler toolchain whose runtime ships an AS_NEEDED linker-script
        # stub). Auto-fall-through to lld / default in that case.
        execute_process(
          COMMAND "${MOLD_BIN}" --version
          OUTPUT_VARIABLE _mold_version_out
          OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_mold_version_out MATCHES "mold ([0-9]+)\\.([0-9]+)")
          if(CMAKE_MATCH_1 VERSION_LESS 3)
            message(
              STATUS "Skipping mold ${CMAKE_MATCH_1}.${CMAKE_MATCH_2} (AS_NEEDED() support requires mold >= 3.0).")
          else()
            set(_mold_usable TRUE)
          endif()
        else()
          # unknown format — assume usable, user can override via -DUSE_MOLD_LINKER=OFF
          set(_mold_usable TRUE)
        endif()
      endif()
    endif()

    if(_mold_usable)
      set(_LD_GC_SECTIONS "${_LD_GC_SECTIONS} -fuse-ld=mold")
      if(_USE_ICF)
        string(APPEND _LD_GC_SECTIONS " -Wl,--icf=safe")
      endif()
      message(STATUS "Using mold @ ${MOLD_BIN} as the linker.")
    else()
      find_program(LLD_BIN ld.lld)
      if(LLD_BIN)
        set(_LD_GC_SECTIONS "${_LD_GC_SECTIONS} -fuse-ld=lld")
        if(_USE_ICF)
          string(APPEND _LD_GC_SECTIONS " -Wl,--icf=safe")
        endif()
        message(STATUS "Using lld @ ${LLD_BIN} as the linker.")
      else()
        message(STATUS "Using default system linker (mold and lld unavailable).")
      endif()
    endif()
  endif()
  set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${_LD_GC_SECTIONS}")
  set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "${_LD_GC_SECTIONS}")
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${_LD_GC_SECTIONS}")
  set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${_LD_GC_SECTIONS}")
  set(CMAKE_EXE_LINKER_FLAGS_MINSIZEREL "${_LD_GC_SECTIONS}")
  set(CMAKE_SHARED_LINKER_FLAGS_MINSIZEREL "${_LD_GC_SECTIONS}")
  if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # we need to set -Wa,-mbig-obj or build fails with gcc
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wa,-mbig-obj")
    # on windows gcc requires stdc++exp to link for <format> and <print> functions
    set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} -lstdc++exp")
  endif()
elseif(MSVC)
  set(CMAKE_CXX_FLAGS_DEBUG "/Od /Zi /Zf /DDEBUG")
  set(CMAKE_CXX_FLAGS_RELEASE "/O2 /GL /DNDEBUG")
  set(CMAKE_CXX_FLAGS_RELWITHASSERT "/O2 /GL /DASSERT_ENABLED")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/O2 /GL /Zi /Zf /DNDEBUG")
  set(CMAKE_CXX_FLAGS_MINSIZEREL "/O1 /Os /GL /DNDEBUG")

  set(LINK_RELEASE "/LTCG /OPT:REF /OPT:ICF")
  set(LINK_RELASSERT "${LINK_RELEASE}")
  set(LINK_RELDEBINFO "${LINK_RELEASE}")
  set(LINK_MINSIZE "${LINK_RELEASE} /DEBUG:NONE") # strip PDB
endif()

# clang-scan-deps in required by ccache
if(CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
  string(
    REGEX MATCH
          "^[0-9]+"
          CLANG_MAJOR
          "${CMAKE_CXX_COMPILER_VERSION}")
  find_program(CLANG_SCAN_DEPS_EXEC NAMES clang-scan-deps-${CLANG_MAJOR} clang-scan-deps)
  if(CLANG_SCAN_DEPS_EXEC)
    set(CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS
        "${CLANG_SCAN_DEPS_EXEC}"
        CACHE FILEPATH "" FORCE)
  else()
    message(WARNING "clang‑scan‑deps not found; module scanning will be disabled")
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
  endif()
endif()

# Use ccache if found and enabled
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM AND USE_CCACHE)
  message(STATUS "ccache found and will be used")
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
else()
  message(STATUS "ccache will not be used")
endif()

# Prefer forced colored compiler output if there's a chance we'd otherwise get none at all. Ninja and ccache "consume"
# the compiler output, breaking the terminal detection of compilers. ccache tries to solve the problem, but can only do
# so if it determines that it's calling GCC or Clang. It uses a very lightweight heuristic, which breaks easily.
if((CMAKE_GENERATOR STREQUAL "Ninja" OR (CCACHE_PROGRAM AND USE_CCACHE)) AND NOT DEFINED CMAKE_COLOR_DIAGNOSTICS)
  message(
    STATUS
      "Forcing compiler color output due to the use of Ninja and/or ccache. Use -DCMAKE_COLOR_DIAGNOSTICS=OFF to turn it off."
  )
  set(CMAKE_COLOR_DIAGNOSTICS ON)
endif()

set(CMAKE_EXT_DEP_WARNING_GUARD "")
if(DISABLE_EXTERNAL_DEPS_WARNINGS) # enable warnings for external dependencies
  set(CMAKE_EXT_DEP_WARNING_GUARD SYSTEM)
endif()

# Initialize a variable to hold all the compiler flags -> exported into global config.h(.in)
if(CMAKE_BUILD_TYPE MATCHES Debug)
  set(ALL_COMPILER_FLAGS "${CMAKE_CXX_FLAGS_DEBUG} ${CMAKE_CXX_FLAGS}")
elseif(CMAKE_BUILD_TYPE MATCHES Release)
  set(ALL_COMPILER_FLAGS "${CMAKE_CXX_FLAGS_RELEASE} ${CMAKE_CXX_FLAGS}")
elseif(CMAKE_BUILD_TYPE MATCHES RelWithAssert)
  set(ALL_COMPILER_FLAGS "${CMAKE_CXX_FLAGS_RELWITHASSERT} ${CMAKE_CXX_FLAGS}")
elseif(CMAKE_BUILD_TYPE MATCHES RelWithDebInfo)
  set(ALL_COMPILER_FLAGS "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} ${CMAKE_CXX_FLAGS}")
elseif(CMAKE_BUILD_TYPE MATCHES MinSizeRel)
  set(ALL_COMPILER_FLAGS "${CMAKE_CXX_FLAGS_MINSIZEREL} ${CMAKE_CXX_FLAGS}")
endif()
# Replace ; with space
string(
  REPLACE ";"
          " "
          ALL_COMPILER_FLAGS
          "${ALL_COMPILER_FLAGS}")

if(CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
  if(NOT GR_USE_ADAPTIVE_CPP AND GR4_USE_LIBCXX)
    message(STATUS "Using plain Clang – enabling libc++ (GR4_USE_LIBCXX=ON)")
    add_compile_options(-stdlib=libc++ -Wno-unused-command-line-argument)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -stdlib=libc++")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -stdlib=libc++")
  elseif(NOT GR_USE_ADAPTIVE_CPP)
    message(STATUS "Using plain Clang – using default C++ standard library")
  else()
    message(STATUS "AdaptiveCpp detected – skipping libc++ settings")
  endif()
  if(TIMETRACE)
    add_compile_options(-ftime-trace)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -ftime-trace")
    message(STATUS "Enable TIMETRACE: ${TIMETRACE}")
  endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "(Clang|GNU)")
  if(ENABLE_TBB)
    find_package(TBB REQUIRED)
    target_link_libraries(gnuradio-options INTERFACE TBB::tbb)
    string(APPEND GR4_PKGCONFIG_EXTRA_LIBS " -ltbb")
  else()
    target_compile_definitions(gnuradio-options INTERFACE _GLIBCXX_USE_TBB_PAR_BACKEND=0)
  endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "(Clang|GNU)")
  if(ADDRESS_SANITIZER)
    set(SANITIZER_FLAGS
        -fsanitize=address
        -fsanitize-address-use-after-scope
        -fsanitize=leak
        -fno-omit-frame-pointer
        -fstack-protector-strong)
  elseif(UB_SANITIZER)
    set(SANITIZER_FLAGS
        -fsanitize=undefined
        -fsanitize-address-use-after-scope
        -fsanitize=leak
        -fno-omit-frame-pointer
        -fstack-protector-strong)
  elseif(THREAD_SANITIZER)
    set(SANITIZER_FLAGS -fsanitize=thread -fsanitize=thread)
  endif()

  if(DEFINED SANITIZER_FLAGS)
    add_compile_options(${SANITIZER_FLAGS} -fno-omit-frame-pointer -fstack-protector-strong)
    if(NOT EMSCRIPTEN)
      add_compile_options(${SANITIZER_FLAGS} -fstack-clash-protection)
    endif()
    add_link_options(${SANITIZER_FLAGS})
    message(STATUS "Enabled sanitizer flags: ${SANITIZER_FLAGS}")
  endif()
endif()

# Include What You Use tooling: https://github.com/include-what-you-use/include-what-you-use
find_program(INCLUDE_WHAT_YOU_USE_TOOL_PATH NAMES include-what-you-use iwyu)
if(INCLUDE_WHAT_YOU_USE_TOOL_PATH)
  message(" using 'Include What You Use' path: (${INCLUDE_WHAT_YOU_USE_TOOL_PATH})")
  set_property(GLOBAL PROPERTY CMAKE_CXX_INCLUDE_WHAT_YOU_USE ${INCLUDE_WHAT_YOU_USE_TOOL_PATH})
endif()
