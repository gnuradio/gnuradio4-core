# Script mode: stages every CPack component of one module and turns each into a
# flat component package with pkgbuild, then writes the descriptor the
# superbuild's bundle generator reads.
#
#   cmake -DARGS_FILE=<build>/packaging/productbuild/args.cmake
#         -P BuildComponentPackages.cmake
#
# All the arguments live in args.cmake, written by cmake/GrProductBuild.cmake at
# configure time, so that adding one does not mean editing the custom target
# that invokes this script.
#
# Flat component packages exist because a productbuild distribution package
# cannot be nested inside another one: the superbuild needs real component
# packages to reference from its own distribution. CPack's productbuild
# generator builds equivalent ones internally but leaves them under
# _CPack_Packages/, an internal path nothing outside CPack may depend on.

if(NOT DEFINED ARGS_FILE OR NOT EXISTS "${ARGS_FILE}")
  message(FATAL_ERROR "BuildComponentPackages.cmake requires -DARGS_FILE=<path to args.cmake>")
endif()
include("${ARGS_FILE}")

foreach(variable GR4_PB_BINARY_DIR GR4_PB_PACKAGE_NAME GR4_PB_VERSION GR4_PB_IDENTIFIER GR4_PB_COMPONENTS
                 GR4_PB_INSTALL_ROOT GR4_PB_PLUGIN_ROOT GR4_PB_SCRIPTS_DIR GR4_PB_OUT_DIR GR4_PB_STAGE_DIR)
  if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
    message(FATAL_ERROR "${ARGS_FILE} does not set ${variable}")
  endif()
endforeach()

find_program(GR4_PB_PKGBUILD pkgbuild REQUIRED)

# A component set that shrank between builds must not leave last build's
# packages behind for the bundle to pick up.
file(REMOVE_RECURSE "${GR4_PB_STAGE_DIR}" "${GR4_PB_OUT_DIR}")
file(MAKE_DIRECTORY "${GR4_PB_OUT_DIR}")

set(descriptor_entries "")
set(collected_components "")

foreach(component IN LISTS GR4_PB_COMPONENTS)
  set(stage "${GR4_PB_STAGE_DIR}/${component}")

  if(component IN_LIST GR4_PB_PLUGIN_COMPONENTS)
    # plugin components install to "." so their files land at the stage root and
    # the package carries the plugin tree as its install location
    set(prefix "${stage}")
    set(install_location "${GR4_PB_PLUGIN_ROOT}")
  else()
    # everything else is staged under the real install root inside the payload,
    # so the package installs relative to "/"
    set(prefix "${stage}${GR4_PB_INSTALL_ROOT}")
    set(install_location "/")
  endif()

  file(MAKE_DIRECTORY "${prefix}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${GR4_PB_BINARY_DIR}" --component "${component}" --prefix "${prefix}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_output)
  if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "Staging component ${component} failed:\n${install_output}")
  endif()

  # A component nothing installs into would become an empty checkbox in the
  # installer, so it is dropped rather than packaged.
  file(GLOB_RECURSE staged_files LIST_DIRECTORIES FALSE "${stage}/*")
  if(NOT staged_files)
    # Dropping the component that carries the postinstall script would install
    # the files and register nothing, leaving the package invisible to every
    # other GR installer and to gr4-uninstall.
    if(component STREQUAL GR4_PB_REGISTER_COMPONENT)
      message(
        FATAL_ERROR
          "Component '${component}' records ${GR4_PB_PACKAGE_NAME} in the install manifest, "
          "but it installs no files. Point REGISTER_COMPONENT at a component that does.")
    endif()
    message(STATUS "${component}: installs no files, leaving it out of the bundle")
    continue()
  endif()

  # reverse-DNS segments are lowercase and alphanumeric; CPack component names
  # are neither
  string(TOLOWER "${component}" component_id)
  string(REGEX REPLACE "[^a-z0-9]+" "-" component_id "${component_id}")
  string(REGEX REPLACE "^-+|-+$" "" component_id "${component_id}")
  set(identifier "${GR4_PB_IDENTIFIER}.${component_id}")
  set(package_file "${GR4_PB_PACKAGE_NAME}-${component}.pkg")

  set(pkgbuild_arguments
      --root "${stage}"
      --install-location "${install_location}"
      --identifier "${identifier}"
      --version "${GR4_PB_VERSION}")
  if(EXISTS "${GR4_PB_SCRIPTS_DIR}/${component}")
    list(APPEND pkgbuild_arguments --scripts "${GR4_PB_SCRIPTS_DIR}/${component}")
  endif()
  if(GR4_PB_SIGN_IDENTITY)
    # optional: the signature that matters for Gatekeeper is the one on the
    # distribution package the superbuild builds around these
    list(APPEND pkgbuild_arguments --sign "${GR4_PB_SIGN_IDENTITY}")
  endif()

  execute_process(COMMAND "${GR4_PB_PKGBUILD}" ${pkgbuild_arguments} "${GR4_PB_OUT_DIR}/${package_file}"
                  COMMAND_ERROR_IS_FATAL ANY)

  set(display "${GR4_PB_DISPLAY_${component}}")
  if(NOT display)
    set(display "${component}")
  endif()

  list(APPEND collected_components "${component}")
  string(APPEND descriptor_entries "    [==[${component}|${package_file}|${identifier}|${display}]==]\n")
  message(STATUS "Built ${GR4_PB_OUT_DIR}/${package_file} (${identifier})")
endforeach()

if(NOT collected_components)
  message(FATAL_ERROR "No component of ${GR4_PB_PACKAGE_NAME} installs any files, so no package was built")
endif()

file(
  WRITE "${GR4_PB_OUT_DIR}/components.cmake"
  "# Generated by gr-core packaging/productbuild/BuildComponentPackages.cmake, do not edit.
set(GR4_COMPONENT_PACKAGE_NAME [==[${GR4_PB_PACKAGE_NAME}]==])
set(GR4_COMPONENT_VERSION [==[${GR4_PB_VERSION}]==])
set(GR4_COMPONENT_SUMMARY [==[${GR4_PB_SUMMARY}]==])
set(GR4_COMPONENT_CPACK_COMPONENTS [==[${collected_components}]==])
# one \"COMPONENT|file|identifier|display name\" entry per flat component package
set(GR4_COMPONENT_PKGS
${descriptor_entries})
")

message(STATUS "Wrote ${GR4_PB_OUT_DIR}/components.cmake")
