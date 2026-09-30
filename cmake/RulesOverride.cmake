# project() and each try_compile project read this file, through the rules file CMakeLists.txt writes into the build
# tree, once they have identified the compiler. A rules file the caller names in CMAKE_USER_MAKE_RULES_OVERRIDE runs
# first and sees CMake's defaults. The project's defaults from FlagsOverride.cmake then replace each per-build-type
# _INIT value the caller's file leaves unchanged.
set(_grBuildTypes
    DEBUG
    RELEASE
    RELWITHDEBINFO
    RELWITHASSERT
    MINSIZEREL)
foreach(_config IN LISTS _grBuildTypes)
  set(_grCMakeDefault_${_config} "${CMAKE_CXX_FLAGS_${_config}_INIT}")
endforeach()
if(_GR_CALLER_RULES_OVERRIDE)
  include(${_GR_CALLER_RULES_OVERRIDE})
endif()
foreach(_config IN LISTS _grBuildTypes)
  set(_grCaller_${_config} "${CMAKE_CXX_FLAGS_${_config}_INIT}")
endforeach()
include(${CMAKE_CURRENT_LIST_DIR}/FlagsOverride.cmake)
foreach(_config IN LISTS _grBuildTypes)
  set(_default "${_grCMakeDefault_${_config}}")
  set(_caller "${_grCaller_${_config}}")
  if(_caller STREQUAL _default)
    continue()
  endif()
  set(CMAKE_CXX_FLAGS_${_config}_INIT "${_caller}")
endforeach()
