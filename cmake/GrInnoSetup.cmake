#[[
Shared Windows installer configuration for GNU Radio 4.

Every GR project that ships an Inno Setup installer (gr-core, gr-blocks, ...
and the combined superbuild installer) configures it through this module so
that they all agree on one install layout:

    %LOCALAPPDATA%\Programs\GNURadio4\v<api>   runtime libraries and headers (user-selectable)
    %LOCALAPPDATA%\.gnuradio-4\plugins\v<api>  plugins
    %LOCALAPPDATA%\.gnuradio-4\manifest\v<api> the manifest describing both roots
    %LOCALAPPDATA%\.gnuradio-4\tools           the manifest maintenance script

The first installer to run lets the user pick the install root and writes the
manifest; every later one reuses the recorded layout, skips the directory page
and only adds its own entries. Reinstalls and upgrades proceed silently,
downgrades ask for confirmation.

<api> is the plugin ABI version, and it keys the Add/Remove entry as well as
the paths above: installations built against different ABIs are separate
products that coexist, while releases sharing an ABI upgrade in place. The
release and debug builds of one release are a single product sharing an
install root and an uninstall entry, kept apart on disc by CMAKE_DEBUG_POSTFIX
and recorded separately in the manifest as that package's configurations.

Terminology, because the manifest and CPack both use the word "component":

    package   a versioned GR product recorded in the manifest, such as
              gr-core=4.0.0. The manifest keeps packages in two sections,
              "components" and "plugins", which is the split between
              gr_configure_innosetup_installer() and
              gr_configure_innosetup_plugins() below.
    component a CPack / Inno Setup install component, such as CORE or DEVELOP:
              a separately selectable part of one installer. One package
              normally spans several components, so the two are not
              interchangeable and neither implies the other.
#]]

set(GR_INNOSETUP_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}/../packaging/innosetup")
if(NOT EXISTS "${GR_INNOSETUP_TEMPLATE_DIR}/gr_install_flow.iss.in")
  # installed layout: the templates sit next to this module
  set(GR_INNOSETUP_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}/packaging/innosetup")
endif()

# Regenerates the installer scripts and CPack wiring from whatever has been
# declared so far. Both public macros call it, so they may be used in any order.
macro(_gr_innosetup_generate)
  if(NOT GR_PLUGIN_API_VERSION)
    message(FATAL_ERROR "gr_configure_innosetup_installer: GR_PLUGIN_API_VERSION is not set")
  endif()

  # the installer script reads these as flat strings
  string(JOIN "|" GR_INSTALLER_COMPONENT_PACKAGES ${GR_INNOSETUP_COMPONENT_PACKAGES})
  string(JOIN "|" GR_INSTALLER_PLUGIN_PACKAGES ${GR_INNOSETUP_PLUGIN_PACKAGES})
  string(JOIN "," GR_INSTALLER_PRESELECTED_COMPONENTS ${GR_INNOSETUP_PRESELECTED_COMPONENTS})

  # Recorded against every package this installer registers, so that anything
  # reading the manifest can tell a release install from a debug one. Once the
  # two share an install root this list is the only thing that does, so a wrong
  # answer is worse than a missing one.
  #
  # A single config generator settles this now. A multi config generator does
  # not choose until pack time, long after this script is written, so there is
  # nothing to read and GR_PACKAGE_CONFIGURATION has to say which configuration
  # is being packaged. Defaulting silently would register a Debug install as
  # Release, so the guess is at least announced.
  if(GR_PACKAGE_CONFIGURATION)
    set(GR_INSTALLER_CONFIGURATION "${GR_PACKAGE_CONFIGURATION}")
  elseif(CMAKE_BUILD_TYPE)
    set(GR_INSTALLER_CONFIGURATION "${CMAKE_BUILD_TYPE}")
  else()
    set(GR_INSTALLER_CONFIGURATION "Release")
    get_property(_gr_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_gr_multi_config)
      message(
        WARNING
          "GR4 installer: recording this package as a Release install, because CMAKE_BUILD_TYPE is empty "
          "under a multi-config generator. Set GR_PACKAGE_CONFIGURATION to the configuration you intend to "
          "package, or a Debug package will be registered as Release.")
    endif()
  endif()

  # One folder for every GR 4 package, keyed on the plugin ABI like everything
  # else and spelled exactly as GrBundleStartMenuLeaf() in gr_install_flow.iss.in
  # spells it, so a CPack generated shortcut and one the superbuild installer
  # creates land in the same place. Set here rather than in each project's
  # CPackConfig so the two cannot drift apart; GR_START_MENU still overrides it.
  if(NOT GR_START_MENU)
    set(GR_START_MENU "GNU Radio 4 (v${GR_PLUGIN_API_VERSION})")
  endif()
  set(CPACK_INNOSETUP_PROGRAM_MENU_FOLDER "${GR_START_MENU}")

  # Carried into CPackConfig.cmake, which is the only file the superbuild
  # installer reads back from a module's build tree. It needs the manifest
  # package names to ask whether a module's debug build is already installed,
  # and those are not CPACK_PACKAGE_NAME.
  set(CPACK_GR4_MANIFEST_PACKAGES "${GR_INSTALLER_COMPONENT_PACKAGES}")

  set(GR_MANIFEST_TOOL "${GR_INNOSETUP_TEMPLATE_DIR}/gr_manifest.ps1")
  set(_GR_INNO_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/packaging/innosetup")
  configure_file("${GR_INNOSETUP_TEMPLATE_DIR}/gr_install_flow.iss.in" "${_GR_INNO_OUTPUT_DIR}/gr_install_flow.iss"
                 @ONLY)
  configure_file("${GR_INNOSETUP_TEMPLATE_DIR}/gr_installer_files.iss.in" "${_GR_INNO_OUTPUT_DIR}/gr_installer_files.iss"
                 @ONLY)

  # CPack writes these values into CPackConfig.cmake verbatim; without this the
  # backslashes and braces of the Inno Setup constants below fail to re-parse
  set(CPACK_VERBATIM_VARIABLES ON)

  set(CPACK_INNOSETUP_EXTRA_SCRIPTS "${_GR_INNO_OUTPUT_DIR}/gr_installer_files.iss")
  set(CPACK_INNOSETUP_CODE_FILES "${_GR_INNO_OUTPUT_DIR}/gr_install_flow.iss")

  # The ABI version is appended here, once, rather than being baked into each
  # project's APP_ID: installations built against different plugin ABIs are
  # different products and need an Add/Remove entry each, while releases that
  # share an ABI must keep one entry between them and upgrade in place.
  set(CPACK_INNOSETUP_SETUP_AppId "${GR_INNOSETUP_APP_ID}_v${GR_PLUGIN_API_VERSION}")
  # the manifest, not Inno's own registry entry, decides where we install
  set(CPACK_INNOSETUP_SETUP_DefaultDirName "{code:GrGetInstallRoot}")
  set(CPACK_INNOSETUP_SETUP_UsePreviousAppDir "no")
  # per-user by default, so no UAC prompt and {autopf} resolves to %LOCALAPPDATA%\Programs
  set(CPACK_INNOSETUP_SETUP_PrivilegesRequired "lowest")
  # ...but the user, or the unified installer running this one silently, may
  # ask for a machine-wide install instead. Without this, /ALLUSERS is refused.
  set(CPACK_INNOSETUP_SETUP_PrivilegesRequiredOverridesAllowed "commandline dialog")
  set(CPACK_INNOSETUP_SETUP_AppVersion "${CPACK_PACKAGE_VERSION}")
  set(CPACK_INNOSETUP_SETUP_VersionInfoVersion "${CPACK_PACKAGE_VERSION}")
  set(CPACK_INNOSETUP_SETUP_UninstallDisplayName "${CPACK_PACKAGE_NAME} ${CPACK_PACKAGE_VERSION}")

  foreach(_gr_plugin_component IN LISTS GR_INNOSETUP_PLUGIN_COMPONENTS)
    string(TOUPPER "${_gr_plugin_component}" _gr_plugin_component)
    set(CPACK_INNOSETUP_${_gr_plugin_component}_INSTALL_DIRECTORY "{code:GrGetPluginRoot}")
  endforeach()
endmacro()

#[[
gr_configure_innosetup_installer

Configures the installer itself: its Windows identity, the packages it records
in the manifest's "components" section and the components it pre-selects. Call
it once, before include(CPack), from every GR project that ships a Windows
installer, including plugin-only ones.

Args:
    APP_ID                 Inno Setup AppId; MUST be a GUID that is unique per
                           installer and constant across versions, otherwise
                           Windows treats every release as a separate product.
                           The plugin ABI version is appended to it above, so
                           do not add a version of any kind here
    PACKAGES               "name=version" entries recorded in the manifest's
                           "components" section, one per GR package this
                           installer ships. A superbuild installer lists all of
                           them here
                           (default: "${CPACK_PACKAGE_NAME}=${CPACK_PACKAGE_VERSION}")
    PRESELECTED_COMPONENTS CPack components ticked on a fresh install. CPack
                           pre-selects only components marked REQUIRED, so
                           anything else meant to be on by default belongs here
#]]
macro(gr_configure_innosetup_installer)
  cmake_parse_arguments(GR_INNO "" "APP_ID" "PACKAGES;PRESELECTED_COMPONENTS" ${ARGN})

  if(NOT GR_INNO_APP_ID)
    message(FATAL_ERROR "gr_configure_innosetup_installer: APP_ID is required and must be stable across releases")
  endif()

  set(GR_INNOSETUP_APP_ID "${GR_INNO_APP_ID}")
  set(GR_INNOSETUP_PRESELECTED_COMPONENTS ${GR_INNO_PRESELECTED_COMPONENTS})
  if(GR_INNO_PACKAGES)
    set(GR_INNOSETUP_COMPONENT_PACKAGES ${GR_INNO_PACKAGES})
  else()
    set(GR_INNOSETUP_COMPONENT_PACKAGES "${CPACK_PACKAGE_NAME}=${CPACK_PACKAGE_VERSION}")
  endif()

  _gr_innosetup_generate()
endmacro()

#[[
gr_configure_innosetup_plugins

Adds plugin payload to the installer. Plugins are recorded in the manifest's
"plugins" section rather than its "components" section, and their files go to
the shared plugin tree rather than below the install root, which is why they
are declared separately. May be called before or after
gr_configure_innosetup_installer(); a project that ships no plugins never calls
it at all.

Args:
    PACKAGES   "name=version" entries recorded in the manifest's "plugins"
               section, one per plugin this installer ships
    COMPONENTS CPack components whose files belong in the shared plugin tree.
               Their install() DESTINATION must be "." because CPack appends
               the destination to the component's install directory
#]]
macro(gr_configure_innosetup_plugins)
  cmake_parse_arguments(GR_INNO_PLUGIN "" "" "PACKAGES;COMPONENTS" ${ARGN})

  set(GR_INNOSETUP_PLUGIN_PACKAGES ${GR_INNO_PLUGIN_PACKAGES})
  set(GR_INNOSETUP_PLUGIN_COMPONENTS ${GR_INNO_PLUGIN_COMPONENTS})

  # nothing to generate until the installer identity is known
  if(GR_INNOSETUP_APP_ID)
    _gr_innosetup_generate()
  endif()
endmacro()
