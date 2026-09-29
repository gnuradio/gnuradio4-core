# Sets the initial per-build-type compiler flags. project() reads this file once it has identified the compiler, then
# creates the cache entries for the built-in build types and the build type named at that point from their _INIT values.
if(NOT MSVC)
  # optional:  -fno-inline-functions -fvar-tracking-assignments
  set(CMAKE_CXX_FLAGS_DEBUG_INIT "-Og -g1 -gz -DDEBUG -fno-omit-frame-pointer -ffunction-sections -fdata-sections")
  set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O2 -g0 -DNDEBUG -ffunction-sections -fdata-sections")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO_INIT "-O2 -g1 -gz -DNDEBUG -ffunction-sections -fdata-sections")
  set(CMAKE_CXX_FLAGS_RELWITHASSERT_INIT "-O2 -DASSERT_ENABLED -ffunction-sections -fdata-sections")
  # '-s' strips the symbol table from linked binaries. EMBEDDED disables or reduces code features for embedded systems
  # (code size, console output).
  set(CMAKE_CXX_FLAGS_MINSIZEREL_INIT "-Os -g0 -DNDEBUG -DEMBEDDED -ffunction-sections -fdata-sections")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    string(APPEND CMAKE_CXX_FLAGS_MINSIZEREL_INIT " -s")
  endif()
else()
  set(CMAKE_CXX_FLAGS_DEBUG_INIT "/Od /Zi /Zf /DDEBUG")
  set(CMAKE_CXX_FLAGS_RELEASE_INIT "/O2 /GL /DNDEBUG")
  set(CMAKE_CXX_FLAGS_RELWITHASSERT_INIT "/O2 /GL /DASSERT_ENABLED")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO_INIT "/O2 /GL /Zi /Zf /DNDEBUG")
  set(CMAKE_CXX_FLAGS_MINSIZEREL_INIT "/O1 /Os /GL /DNDEBUG")
endif()
