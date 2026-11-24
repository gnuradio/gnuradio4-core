# Regexes that keep libraries out of the runtime dependencies a GR 4 package bundles:
#
# * the platform's own libraries. The Windows API sets resolve to no file at all and have to be dropped before
#   resolution is tried.
# * the MSVC debug runtime. It resolves from the toolchain's redist tree rather than from System32, may not be
#   redistributed, and would otherwise ride along in a Debug package. PRE_EXCLUDE_REGEXES match the unresolved name, so
#   the names are matched here and the debug_nonredist directory in the POST list. The msvcp pattern anchors the 'd' to
#   the version digits, catching msvcp140d_atomic_wait.dll but not msvcp140_codecvt_ids.dll.
# * Homebrew, where a macOS build gets its LLVM runtime. Copying those dylibs would not rewrite any install name, so the
#   binaries would still load the originals. A macOS package currently expects Homebrew LLVM.
# * GR 4's own libraries. Every GR 4 package installs into one shared root, so a downstream package that bundled
#   gnuradio-*.dll or a block library would claim files another installer owns. On macOS they are named through
#   @rpath/, which the patterns allow for: unexcluded, such a name either fails to resolve or copies GR 4 a second time.
function(_gr_runtime_dependency_excludes OUT_VAR)
  set(${OUT_VAR}
      PRE_EXCLUDE_REGEXES
      "api-ms-win-.*"
      "ext-ms-.*"
      "^/lib"
      "^/usr/lib"
      "^/usr/local/lib"
      "^/System/Library"
      "^/usr/lib/libSystem"
      "^/opt/homebrew"
      "^/usr/local/opt"
      "^[Cc]:[\\/][Ww][Ii][Nn][Dd][Oo][Ww][Ss]"
      "[Uu][Cc][Rr][Tt][Bb][Aa][Ss][Ee][Dd]\\.[Dd][Ll][Ll]"
      "[Vv][Cc][Rr][Uu][Nn][Tt][Ii][Mm][Ee].*[Dd]\\.[Dd][Ll][Ll]"
      "[Mm][Ss][Vv][Cc][Pp][0-9]+[Dd](_[A-Za-z0-9_]+)?\\.[Dd][Ll][Ll]"
      "^(@rpath/)?(lib)?gnuradio-"
      "^(@rpath/)?(lib)?Gr[A-Za-z0-9]*Blocks"
      POST_EXCLUDE_REGEXES
      ".*[Ss][Yy][Ss][Tt][Ee][Mm]32.*"
      ".*[Ww][Ii][Nn][Ss][Xx][Ss].*"
      ".*[Dd][Ee][Bb][Uu][Gg]_[Nn][Oo][Nn][Rr][Ee][Dd][Ii][Ss][Tt].*"
      PARENT_SCOPE)
endfunction()

#[[
gr_install_runtime_dependencies

Installs the third-party libraries a downstream project needs alongside its own binaries, using the same exclusion rules
GR 4 packages itself with.

This is an install-time step on purpose. Copying DLLs beside a target at build time leaves stale copies behind whenever
a dependency moves or is rebuilt, and hides which library is actually loaded. While developing, use the GNU Radio 4
shell, which puts the install prefix on PATH; copy libraries only when installing or shipping something.

Add RUNTIME_DEPENDENCY_SET to your own install(TARGETS) call, then name the same set here:

    install(TARGETS my_app RUNTIME_DEPENDENCY_SET my_app_deps
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    gr_install_runtime_dependencies(SET my_app_deps)

Args:
    SET          the RUNTIME_DEPENDENCY_SET to resolve, required
    DESTINATION  where the resolved libraries go (default: CMAKE_INSTALL_BINDIR on Windows, where a DLL must sit beside
                 its executable; CMAKE_INSTALL_LIBDIR elsewhere)
    COMPONENT    install component for the resolved libraries
]]
function(gr_install_runtime_dependencies)
  cmake_parse_arguments(
    "GR_DEPS"
    ""
    "SET;DESTINATION;COMPONENT"
    ""
    ${ARGN})

  if(NOT GR_DEPS_SET)
    message(FATAL_ERROR "gr_install_runtime_dependencies: SET is required")
  endif()
  if(GR_DEPS_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "gr_install_runtime_dependencies: unknown arguments: ${GR_DEPS_UNPARSED_ARGUMENTS}")
  endif()

  if(NOT GR_DEPS_DESTINATION)
    if(WIN32)
      set(GR_DEPS_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    else()
      set(GR_DEPS_DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    endif()
  endif()

  set(COMPONENT_ARGS)
  if(GR_DEPS_COMPONENT)
    set(COMPONENT_ARGS COMPONENT ${GR_DEPS_COMPONENT})
  endif()

  _gr_runtime_dependency_excludes(RUNTIME_DEPENDENCY_EXCLUDES)
  install(
    RUNTIME_DEPENDENCY_SET
    ${GR_DEPS_SET}
    ${RUNTIME_DEPENDENCY_EXCLUDES}
    DESTINATION
    ${GR_DEPS_DESTINATION}
    ${COMPONENT_ARGS})
endfunction()

# Gives every forwarded FILE_SET / PUBLIC_HEADER / PRIVATE_HEADER group its own COMPONENT. It cannot be left to the
# caller: an exported target must install its file sets in the same install() command, and this function consumes the
# COMPONENT keyword, so without this the headers would fall into "Unspecified".
function(_gr_apply_headers_component OUT_VAR COMPONENT_NAME)
  set(RESULT)
  set(NEXT_IS_FILE_SET_NAME FALSE)
  foreach(ARG IN LISTS ARGN)
    list(APPEND RESULT "${ARG}")
    if(NEXT_IS_FILE_SET_NAME)
      set(NEXT_IS_FILE_SET_NAME FALSE)
      list(
        APPEND
        RESULT
        COMPONENT
        "${COMPONENT_NAME}")
    elseif(ARG STREQUAL "FILE_SET")
      set(NEXT_IS_FILE_SET_NAME TRUE)
    elseif(ARG MATCHES "^(PUBLIC_HEADER|PRIVATE_HEADER)$")
      list(
        APPEND
        RESULT
        COMPONENT
        "${COMPONENT_NAME}")
    endif()
  endforeach()
  set(${OUT_VAR}
      "${RESULT}"
      PARENT_SCOPE)
endfunction()

#[[
gr_install_artifact

The install entry point for everything GR puts on disc. Takes any install() signature and adds what the GR installers
rely on, so no project has to remember a follow-up call.

Args:
    COMPONENT         - the install component to associate the artifact with, default is "CORE"
    HEADERS_COMPONENT - the install component for FILE_SET, PUBLIC_HEADER and PRIVATE_HEADER groups, default is
                        COMPONENT. Use it to put an exported library's binaries and its headers in different components,
                        which has to happen in one call because CMake requires an exported target to install its file
                        sets alongside the export
    ARCHIVE_COMPONENT - the install component for static libraries and, on Windows, import libraries, default is
                        COMPONENT. Both are only needed to build against GR, so they usually belong with the headers
                        rather than with the runtime

Every other argument is forwarded to install() exactly as provided by the caller, so TARGETS, FILES, PROGRAMS,
DIRECTORY, EXPORT, SCRIPT and FILE_SET all behave as usual. PRE_EXCLUDE_REGEXES and POST_EXCLUDE_REGEXES given for
TARGETS add to the ones GR packages with.

For TARGETS the runtime, library and archive destinations are supplied here so that all GR artifacts share one layout:
on Windows the DLLs sit next to the executables, elsewhere they go to CMAKE_INSTALL_LIBDIR and the binaries get an RPATH
that finds them, unless the target already sets its own. With GR_PACKAGING, executables, shared libraries and module
libraries also pull in the third-party libraries they load at runtime. Those are resolved by this same install()
command, which is what guarantees they land in the same destination and component as the artifact needing them.
Everything else - static, object and interface libraries, files, directories, exports - has no runtime dependencies and
skips that step. With MSVC each loadable target's PDB goes to the SYMBOLS component.
#]]
function(gr_install_artifact)
  cmake_parse_arguments(
    "GR_INSTALL"
    ""
    "COMPONENT;HEADERS_COMPONENT;ARCHIVE_COMPONENT;EXPORT"
    "TARGETS;DIRECTORY"
    ${ARGN})
  if(NOT GR_INSTALL_COMPONENT)
    set(GR_INSTALL_COMPONENT "CORE")
  endif()
  if(NOT GR_INSTALL_HEADERS_COMPONENT)
    set(GR_INSTALL_HEADERS_COMPONENT "${GR_INSTALL_COMPONENT}")
  endif()
  if(NOT GR_INSTALL_ARCHIVE_COMPONENT)
    set(GR_INSTALL_ARCHIVE_COMPONENT "${GR_INSTALL_COMPONENT}")
  endif()
  if(POLICY CMP0207)
    # match the runtime dependency exclude regexes against normalised paths
    cmake_policy(SET CMP0207 NEW)
  endif()

  if(GR_INSTALL_DIRECTORY)
    # COMPONENT has to precede any FILES_MATCHING/PATTERN rules; appended, it is a hard error
    install(
      DIRECTORY ${GR_INSTALL_DIRECTORY}
      COMPONENT ${GR_INSTALL_COMPONENT}
      ${GR_INSTALL_UNPARSED_ARGUMENTS})
    return()
  endif()

  set(EXPORT_ARGS)
  if(GR_INSTALL_EXPORT)
    set(EXPORT_ARGS EXPORT ${GR_INSTALL_EXPORT})
  endif()

  if(NOT GR_INSTALL_TARGETS)
    # FILES, PROGRAMS, EXPORT, SCRIPT/CODE: no artifact kinds to place
    install(
      ${EXPORT_ARGS}
      ${GR_INSTALL_UNPARSED_ARGUMENTS}
      COMPONENT
      ${GR_INSTALL_COMPONENT})
    return()
  endif()

  set(BIN_DEST "${CMAKE_INSTALL_BINDIR}")
  if(WIN32)
    set(LIB_DEST "${CMAKE_INSTALL_BINDIR}")
  else()
    set(LIB_DEST "${CMAKE_INSTALL_LIBDIR}")
    # @loader_path, not @executable_path: @executable_path resolves against whatever process loaded a .dylib, so a
    # plugin opened by a host outside the prefix would look for its siblings next to that host.
    file(
      RELATIVE_PATH
      BIN_TO_LIB
      "${CMAKE_INSTALL_FULL_BINDIR}"
      "${CMAKE_INSTALL_FULL_LIBDIR}")
    if(APPLE)
      set(INSTALL_RPATH "@loader_path/${BIN_TO_LIB}")
    else()
      set(INSTALL_RPATH "$ORIGIN/${BIN_TO_LIB}")
    endif()
    list(APPEND INSTALL_RPATH ${CMAKE_INSTALL_RPATH})
  endif()

  # only loadable artifacts have runtime dependencies, an RPATH or a PDB to speak of
  set(RUNTIME_DEPENDENCY_ARGS)
  foreach(TARGET_NAME IN LISTS GR_INSTALL_TARGETS)
    get_target_property(TARGET_TYPE ${TARGET_NAME} TYPE)
    if(NOT
       TARGET_TYPE
       MATCHES
       "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
      continue()
    endif()
    get_target_property(EXISTING_RPATH ${TARGET_NAME} INSTALL_RPATH)
    if(NOT WIN32 AND NOT EXISTING_RPATH)
      set_target_properties(${TARGET_NAME} PROPERTIES INSTALL_RPATH "${INSTALL_RPATH}")
    endif()
    if(GR_PACKAGING)
      _gr_runtime_dependency_excludes(RUNTIME_DEPENDENCY_EXCLUDES)
      set(RUNTIME_DEPENDENCY_ARGS RUNTIME_DEPENDENCIES ${RUNTIME_DEPENDENCY_EXCLUDES})
    endif()
    if(MSVC)
      # OPTIONAL because a target built without /Zi has no PDB
      install(
        FILES "$<TARGET_PDB_FILE:${TARGET_NAME}>"
        DESTINATION ${BIN_DEST}
        COMPONENT SYMBOLS
        OPTIONAL)
    endif()
  endforeach()

  _gr_apply_headers_component(FORWARDED_ARGS "${GR_INSTALL_HEADERS_COMPONENT}" ${GR_INSTALL_UNPARSED_ARGUMENTS})
  install(
    TARGETS ${GR_INSTALL_TARGETS}
            ${EXPORT_ARGS}
            ${RUNTIME_DEPENDENCY_ARGS}
            ${FORWARDED_ARGS}
    RUNTIME DESTINATION ${BIN_DEST} COMPONENT ${GR_INSTALL_COMPONENT}
    LIBRARY DESTINATION ${LIB_DEST} COMPONENT ${GR_INSTALL_COMPONENT}
    # Apple requires a FRAMEWORK destination whenever RUNTIME_DEPENDENCIES is given, because a resolved dependency may
    # itself be a framework. No GR target is a framework, so this never creates an install rule of its own.
    FRAMEWORK DESTINATION ${LIB_DEST} COMPONENT ${GR_INSTALL_COMPONENT}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT ${GR_INSTALL_ARCHIVE_COMPONENT})
endfunction()
