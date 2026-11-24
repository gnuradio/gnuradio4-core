#[[
Shared macOS installer configuration for GNU Radio 4.

Every GR project that ships a macOS installer (gr-core, gr-blocks, ... and the
combined superbuild installer) configures it through this module so that they
all agree on one install layout:

    /usr/local/gnuradio4                                runtime libraries and headers
    /Library/Application Support/.gnuradio-4/plugins/v<api>   plugins
    /Library/Application Support/.gnuradio-4/manifest/v<api>  the manifest describing both roots
    /Library/Application Support/.gnuradio-4/tools            the manifest maintenance script

This is the counterpart of GrInnoSetup.cmake and the manifest it maintains is
the same file in the same JSON format, so the two platforms stay describable by
one document. Four things do not carry over, and are deliberate:

    - a pkg's install location is fixed when it is built, so there is no install
      root to negotiate. The first installer does not pick a layout and later
      ones do not inherit it; the manifest only records where the files went,
      which is what third-party plugin installers read.
    - macOS Installer has no "install type" concept, so there is no [Types]
      equivalent. The superbuild turns module groups into per-choice defaults.
    - a pkg preinstall script has no UI, so a downgrade is refused rather than
      confirmed. See gr_preinstall.sh.in.
    - there is no uninstaller, so one is shipped: gr4-uninstall.

Terminology, because the manifest and CPack both use the word "component":

    package   a versioned GR product recorded in the manifest, such as
              gr-core=4.0.0. The manifest keeps packages in two sections,
              "components" and "plugins", which is the split between
              gr_configure_productbuild_installer() and
              gr_configure_productbuild_plugins() below.
    component a CPack install component, such as CORE or DEVELOP: a separately
              selectable part of one installer. One package normally spans
              several components, so the two are not interchangeable and neither
              implies the other.

Targets added to a module that calls gr_configure_productbuild_installer():

    gr4-component-pkgs   stages every CPack component and turns each into a flat
                         component package with pkgbuild, then writes a
                         descriptor listing them. The superbuild collects those
                         packages and composes them into one distribution
                         package with productbuild; a distribution package
                         cannot be nested inside another, which is why the flat
                         ones exist.
#]]

set(GR_PRODUCTBUILD_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}/../packaging/productbuild")
if(NOT EXISTS "${GR_PRODUCTBUILD_TEMPLATE_DIR}/gr_postinstall.sh.in")
  # installed layout: the templates sit next to this module
  set(GR_PRODUCTBUILD_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}/packaging/productbuild")
endif()

# The shared layout. Changing either of these splits an installation in two, so they are not cache variables: every GR
# package must agree on them.
set(GR_PRODUCTBUILD_INSTALL_ROOT "/usr/local/gnuradio4")
set(GR_PRODUCTBUILD_SHARED_ROOT "/Library/Application Support/.gnuradio-4")
set(GR_PRODUCTBUILD_IDENTIFIER_NAMESPACE "org.gnuradio.gr4")

set(GR4_MACOS_SIGN_IDENTITY
    ""
    CACHE STRING "Developer ID Installer identity for the macOS component packages; empty leaves them unsigned")

# Regenerates the installer scripts and CPack wiring from whatever has been declared so far. Both public macros call it,
# so they may be used in any order.
macro(_gr_productbuild_generate)
  if(NOT GR_PLUGIN_API_VERSION)
    message(FATAL_ERROR "gr_configure_productbuild_installer: GR_PLUGIN_API_VERSION is not set")
  endif()

  # the installer scripts read these as flat strings, in the same shapes the Inno Setup templates use
  string(
    JOIN
    "|"
    GR_INSTALLER_COMPONENT_PACKAGES
    ${GR_PRODUCTBUILD_COMPONENT_PACKAGES})
  string(
    JOIN
    "|"
    GR_INSTALLER_PLUGIN_PACKAGES
    ${GR_PRODUCTBUILD_PLUGIN_PACKAGES})
  string(
    JOIN
    ","
    GR_INSTALLER_PRESELECTED_COMPONENTS
    ${GR_PRODUCTBUILD_PRESELECTED_COMPONENTS})

  set(GR_PRODUCTBUILD_PLUGIN_ROOT "${GR_PRODUCTBUILD_SHARED_ROOT}/plugins/v${GR_PLUGIN_API_VERSION}")

  set(_gr_pb_dir "${CMAKE_CURRENT_BINARY_DIR}/packaging/productbuild")
  set(_gr_pb_scripts_dir "${_gr_pb_dir}/scripts")
  set(_gr_pb_out_dir "${_gr_pb_dir}/pkgs")
  set(_gr_pb_stage_dir "${_gr_pb_dir}/stage")

  set(_gr_pb_components ${CPACK_COMPONENTS_ALL})
  if(NOT _gr_pb_components)
    # without an explicit list CPack builds a monolithic package and the component selection this whole mechanism exists
    # for is lost
    message(WARNING "gr_configure_productbuild_installer: CPACK_COMPONENTS_ALL is not set, "
                    "the macOS installer will offer a single component")
    set(_gr_pb_components "Unspecified")
  endif()

  # A REGISTER_COMPONENT that is not built is the quiet failure mode here: the files install, nothing registers, and the
  # package is invisible to every other GR installer and to gr4-uninstall.
  if(NOT
     GR_PRODUCTBUILD_REGISTER_COMPONENT
     IN_LIST
     _gr_pb_components)
    message(
      FATAL_ERROR
        "gr_configure_productbuild_installer: REGISTER_COMPONENT is "
        "'${GR_PRODUCTBUILD_REGISTER_COMPONENT}', which is not in CPACK_COMPONENTS_ALL "
        "(${_gr_pb_components}). Nothing would record this package in the manifest.")
  endif()

  set(_gr_pb_script_permissions
      OWNER_READ
      OWNER_WRITE
      OWNER_EXECUTE
      GROUP_READ
      GROUP_EXECUTE
      WORLD_READ
      WORLD_EXECUTE)

  foreach(_gr_pb_component IN LISTS _gr_pb_components)
    set(_gr_pb_component_scripts "${_gr_pb_scripts_dir}/${_gr_pb_component}")

    # the downgrade guard belongs on every component: any one of them can be the first to write files over a newer
    # install. NEWLINE_STYLE LF on every generated script: a CRLF line ending leaves a stray carriage return on the #!
    # line, and a pkg script that cannot start fails the install with no useful message.
    configure_file("${GR_PRODUCTBUILD_TEMPLATE_DIR}/gr_preinstall.sh.in" "${_gr_pb_component_scripts}/preinstall" @ONLY
                   NEWLINE_STYLE LF FILE_PERMISSIONS ${_gr_pb_script_permissions})

    # pkgbuild --scripts takes a directory, so the manifest tool rides along beside the scripts that call it
    file(
      COPY "${GR_PRODUCTBUILD_TEMPLATE_DIR}/gr_manifest.sh"
      DESTINATION "${_gr_pb_component_scripts}"
      FILE_PERMISSIONS ${_gr_pb_script_permissions})

    # one registration per package, not per component: REGISTER_COMPONENT is REQUIRED, so it is installed whatever else
    # the user selected
    if(_gr_pb_component STREQUAL GR_PRODUCTBUILD_REGISTER_COMPONENT)
      configure_file("${GR_PRODUCTBUILD_TEMPLATE_DIR}/gr_postinstall.sh.in" "${_gr_pb_component_scripts}/postinstall"
                     @ONLY NEWLINE_STYLE LF FILE_PERMISSIONS ${_gr_pb_script_permissions})
    else()
      # a stale postinstall from an earlier configure would register twice
      file(REMOVE "${_gr_pb_component_scripts}/postinstall")
    endif()
  endforeach()

  # The arguments are written to a file rather than baked into the custom target so that a later
  # gr_configure_productbuild_plugins() call is picked up without having to redefine the target.
  set(_gr_pb_args "# Generated by gr-core cmake/GrProductBuild.cmake, do not edit.\n")
  string(APPEND _gr_pb_args "set(GR4_PB_BINARY_DIR [==[${CMAKE_BINARY_DIR}]==])\n")
  string(APPEND _gr_pb_args "set(GR4_PB_PACKAGE_NAME [==[${CPACK_PACKAGE_NAME}]==])\n")
  string(APPEND _gr_pb_args "set(GR4_PB_VERSION [==[${CPACK_PACKAGE_VERSION}]==])\n")
  string(APPEND _gr_pb_args "set(GR4_PB_SUMMARY [==[${CPACK_PACKAGE_DESCRIPTION_SUMMARY}]==])\n")
  string(APPEND _gr_pb_args "set(GR4_PB_IDENTIFIER [==[${GR_PRODUCTBUILD_IDENTIFIER}]==])\n")
  string(APPEND _gr_pb_args "set(GR4_PB_COMPONENTS [==[${_gr_pb_components}]==])\n")
  string(APPEND _gr_pb_args "set(GR4_PB_REGISTER_COMPONENT [==[${GR_PRODUCTBUILD_REGISTER_COMPONENT}]==])\n")
  string(APPEND _gr_pb_args "set(GR4_PB_PLUGIN_COMPONENTS [==[${GR_PRODUCTBUILD_PLUGIN_COMPONENTS}]==])\n")
  string(APPEND _gr_pb_args "set(GR4_PB_INSTALL_ROOT [==[${GR_PRODUCTBUILD_INSTALL_ROOT}]==])\n")
  string(APPEND _gr_pb_args "set(GR4_PB_PLUGIN_ROOT [==[${GR_PRODUCTBUILD_PLUGIN_ROOT}]==])\n")
  string(APPEND _gr_pb_args "set(GR4_PB_SCRIPTS_DIR [==[${_gr_pb_scripts_dir}]==])\n")
  string(APPEND _gr_pb_args "set(GR4_PB_OUT_DIR [==[${_gr_pb_out_dir}]==])\n")
  string(APPEND _gr_pb_args "set(GR4_PB_STAGE_DIR [==[${_gr_pb_stage_dir}]==])\n")
  string(APPEND _gr_pb_args "set(GR4_PB_SIGN_IDENTITY [==[${GR4_MACOS_SIGN_IDENTITY}]==])\n")
  foreach(_gr_pb_component IN LISTS _gr_pb_components)
    string(TOUPPER "${_gr_pb_component}" _gr_pb_component_upper)
    set(_gr_pb_display "${CPACK_COMPONENT_${_gr_pb_component_upper}_DISPLAY_NAME}")
    if(NOT _gr_pb_display)
      set(_gr_pb_display "${_gr_pb_component}")
    endif()
    string(APPEND _gr_pb_args "set(GR4_PB_DISPLAY_${_gr_pb_component} [==[${_gr_pb_display}]==])\n")
  endforeach()

  # file(WRITE), not file(CONFIGURE): everything above is already interpolated, and a lone '@' in an install path would
  # otherwise be taken as a substitution
  file(WRITE "${_gr_pb_dir}/args.cmake" "${_gr_pb_args}")

  # the uninstaller is payload, not a build-time script, so it is configured into the build tree and installed from
  # there
  configure_file("${GR_PRODUCTBUILD_TEMPLATE_DIR}/gr4-uninstall.in" "${_gr_pb_dir}/gr4-uninstall" @ONLY
                 NEWLINE_STYLE LF FILE_PERMISSIONS ${_gr_pb_script_permissions})

  set(CPACK_PACKAGING_INSTALL_PREFIX "${GR_PRODUCTBUILD_INSTALL_ROOT}")
  set(CPACK_PRODUCTBUILD_IDENTIFIER "${GR_PRODUCTBUILD_IDENTIFIER}")
  set(CPACK_PRODUCTBUILD_DOMAINS ON)
  set(CPACK_PRODUCTBUILD_DOMAINS_ANYWHERE OFF)
  set(CPACK_PRODUCTBUILD_DOMAINS_USER OFF)
  set(CPACK_PRODUCTBUILD_DOMAINS_ROOT ON)
  foreach(_gr_pb_component IN LISTS _gr_pb_components)
    if(EXISTS "${_gr_pb_scripts_dir}/${_gr_pb_component}/preinstall")
      set(CPACK_PREFLIGHT_${_gr_pb_component}_SCRIPT "${_gr_pb_scripts_dir}/${_gr_pb_component}/preinstall")
    endif()
    if(EXISTS "${_gr_pb_scripts_dir}/${_gr_pb_component}/postinstall")
      set(CPACK_POSTFLIGHT_${_gr_pb_component}_SCRIPT "${_gr_pb_scripts_dir}/${_gr_pb_component}/postinstall")
    endif()
  endforeach()

  if(NOT TARGET gr4-component-pkgs)
    add_custom_target(
      gr4-component-pkgs
      COMMAND "${CMAKE_COMMAND}" "-DARGS_FILE:FILEPATH=${_gr_pb_dir}/args.cmake" -P
              "${GR_PRODUCTBUILD_TEMPLATE_DIR}/BuildComponentPackages.cmake"
      USES_TERMINAL VERBATIM
      COMMENT "Building the ${CPACK_PACKAGE_NAME} component packages")
  endif()
endmacro()

#[[
gr_configure_productbuild_installer

Configures the installer itself: its macOS identity, the packages it records in
the manifest's "components" section and the components it pre-selects. Call it
once, before include(CPack), from every GR project that ships a macOS installer,
including plugin-only ones.

Args:
    IDENTIFIER          reverse-DNS package identifier; MUST be unique per
                        installer and constant across versions, otherwise
                        pkgutil treats every release as a separate product and
                        gr4-uninstall leaves the old receipts behind. This is the
                        counterpart of Inno Setup's AppId, without the brace
                        escaping
    PACKAGES            "name=version" entries recorded in the manifest's
                        "components" section, one per GR package this installer
                        ships. A superbuild installer lists all of them here
                        (default: "${CPACK_PACKAGE_NAME}=${CPACK_PACKAGE_VERSION}")
    REGISTER_COMPONENT  the CPack component whose postinstall script records the
                        packages in the manifest. Must be one that is always
                        installed, i.e. REQUIRED (default: CORE)
    PRESELECTED_COMPONENTS
                        CPack components ticked on a fresh install. CPack
                        pre-selects only components marked REQUIRED, so anything
                        else meant to be on by default belongs here
#]]
macro(gr_configure_productbuild_installer)
  cmake_parse_arguments(
    GR_PB
    ""
    "IDENTIFIER;REGISTER_COMPONENT"
    "PACKAGES;PRESELECTED_COMPONENTS"
    ${ARGN})

  if(NOT GR_PB_IDENTIFIER)
    message(
      FATAL_ERROR "gr_configure_productbuild_installer: IDENTIFIER is required and must be stable across releases")
  endif()

  set(GR_PRODUCTBUILD_IDENTIFIER "${GR_PB_IDENTIFIER}")
  set(GR_PRODUCTBUILD_PRESELECTED_COMPONENTS ${GR_PB_PRESELECTED_COMPONENTS})
  if(GR_PB_REGISTER_COMPONENT)
    set(GR_PRODUCTBUILD_REGISTER_COMPONENT "${GR_PB_REGISTER_COMPONENT}")
  else()
    set(GR_PRODUCTBUILD_REGISTER_COMPONENT "CORE")
  endif()
  if(GR_PB_PACKAGES)
    set(GR_PRODUCTBUILD_COMPONENT_PACKAGES ${GR_PB_PACKAGES})
  else()
    set(GR_PRODUCTBUILD_COMPONENT_PACKAGES "${CPACK_PACKAGE_NAME}=${CPACK_PACKAGE_VERSION}")
  endif()

  _gr_productbuild_generate()
endmacro()

#[[
gr_configure_productbuild_plugins

Adds plugin payload to the installer. Plugins are recorded in the manifest's
"plugins" section rather than its "components" section, and their files go to
the shared plugin tree rather than below the install root, which is why they are
declared separately. May be called before or after
gr_configure_productbuild_installer(); a project that ships no plugins never
calls it at all.

Args:
    PACKAGES   "name=version" entries recorded in the manifest's "plugins"
               section, one per plugin this installer ships
    COMPONENTS CPack components whose files belong in the shared plugin tree.
               Their install() DESTINATION must be "." because the component
               package is built with the plugin root as its install location
#]]
macro(gr_configure_productbuild_plugins)
  cmake_parse_arguments(
    GR_PB_PLUGIN
    ""
    ""
    "PACKAGES;COMPONENTS"
    ${ARGN})

  set(GR_PRODUCTBUILD_PLUGIN_PACKAGES ${GR_PB_PLUGIN_PACKAGES})
  set(GR_PRODUCTBUILD_PLUGIN_COMPONENTS ${GR_PB_PLUGIN_COMPONENTS})

  # nothing to generate until the installer identity is known
  if(GR_PRODUCTBUILD_IDENTIFIER)
    _gr_productbuild_generate()
  endif()
endmacro()

#[[
gr_install_productbuild_uninstaller

Installs gr4-uninstall into <install-root>/bin. Call it from the project that
owns the shared layout -- gr-core -- and only from there: one uninstaller serves
every GR 4 package, and it is removed when the last manifest entry goes.

Args:
    COMPONENT  the CPack component the uninstaller belongs to (default: CORE)
#]]
macro(gr_install_productbuild_uninstaller)
  cmake_parse_arguments(
    GR_PB_UNINSTALL
    ""
    "COMPONENT"
    ""
    ${ARGN})

  if(NOT GR_PB_UNINSTALL_COMPONENT)
    set(GR_PB_UNINSTALL_COMPONENT "CORE")
  endif()
  if(NOT GR_PRODUCTBUILD_IDENTIFIER)
    message(FATAL_ERROR "gr_install_productbuild_uninstaller: call gr_configure_productbuild_installer() first")
  endif()

  install(
    PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/packaging/productbuild/gr4-uninstall"
    DESTINATION bin
    COMPONENT ${GR_PB_UNINSTALL_COMPONENT})
endmacro()
