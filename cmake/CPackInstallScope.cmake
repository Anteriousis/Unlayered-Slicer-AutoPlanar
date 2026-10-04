# Per-user vs per-machine installer selection.
#
# The DEFAULT is per-user: the app installs under %LOCALAPPDATA%\Programs, with
# no UAC prompt. That is not a preference, it is the prerequisite for the slicer
# updating itself silently -- a copy living in Program Files can only be
# replaced by an elevated process, so every update would prompt.
#
# The per-machine installer is still available for shared or centrally-managed
# workstations, where one install has to serve every account:
#
#   cpack -G NSIS -C Release -D CPACK_INSTALL_SCOPE=machine
#
# The flag is deliberately NOT set in CMakeLists.txt, so that `cpack -D` reaches
# this file. CPack includes this file for every generator AFTER the generated
# CPackConfig.cmake, and that ordering is the entire point: variables assigned
# with set(CPACK_...) in CMakeLists.txt are written into CPackConfig.cmake as
# unconditional SET commands which run *after* any `cpack -D` argument, silently
# discarding it. An override of such a variable does not fail -- it produces the
# wrong installer and says nothing. A project config file runs later still, so
# assignments made here actually stick.
#
# See the CPack section of CMakeLists.txt for the caveat about the two flavours
# coexisting on one machine.
#
# These variables only do anything because cmake/modules/NSIS.template.in is
# picked up in place of CMake's built-in NSIS template. That override has to sit
# FLAT on the module path: cmCPackNSISGenerator asks FindTemplate() for
# "NSIS.template.in" first and only falls back to the builtin
# "Internal/CPack/NSIS.template.in" if that is not found. Reproducing CMake's
# own Internal/CPack/ directory layout looks more correct and does not work.
# CMAKE_MODULE_PATH reaches cpack on its own -- CPack.cmake copies it into
# CPACK_MODULE_PATH -- so nothing extra is needed here to make it findable.
#
# When the template is not found, nothing warns: the variables below are
# substituted into a template that never references them, and every flavour
# silently comes out per-machine. Verify by grepping RequestExecutionLevel in
# the generated _CPack_Packages/*/NSIS/project.nsi.

if("${CPACK_INSTALL_SCOPE}" STREQUAL "machine")
  # Shared-workstation build: Program Files, elevated, machine-wide shortcuts.
  # Renamed so it cannot be confused with the default download. CPACK_NSIS_
  # INSTALL_ROOT stays unset so CPack keeps picking $PROGRAMFILES64 itself.
  set(CPACK_NSIS_EXECUTION_LEVEL "admin")
  set(CPACK_NSIS_REGISTRY_ROOT "HKLM")
  set(CPACK_NSIS_SHELL_VAR_CONTEXT "all")
  set(CPACK_NSIS_JUSTME_INSTALL_ROOT "$DOCUMENTS")

  # Derived from the name CMakeLists.txt already computed rather than rebuilt
  # from version parts, so it keeps any suffix such as -dev and cannot drift.
  string(REPLACE "_Windows_Installer_" "_Windows_SystemSetup_"
         CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}")
else()
  # Default. The artifact keeps the existing Unlayered_Slicer_Windows_Installer_
  # name so release.yml and the update manifest's asset matching are unaffected.
  set(CPACK_NSIS_EXECUTION_LEVEL "user")
  set(CPACK_NSIS_REGISTRY_ROOT "HKCU")
  set(CPACK_NSIS_SHELL_VAR_CONTEXT "current")
  set(CPACK_NSIS_INSTALL_ROOT "$LOCALAPPDATA\\Programs")
  # Must match CPACK_NSIS_INSTALL_ROOT. The template's .onInit picks this root
  # for a "JustMe" install, which is the branch every non-admin account takes;
  # leaving it at $DOCUMENTS puts standard users somewhere different from
  # admins, and the difference is invisible on a developer machine.
  set(CPACK_NSIS_JUSTME_INSTALL_ROOT "$LOCALAPPDATA\\Programs")
endif()
