cmake_minimum_required(VERSION 3.24)
if(NOT APPLE OR NOT IS_DIRECTORY "${APP}" OR NOT IS_DIRECTORY "${LIB_DIR}")
    message(FATAL_ERROR "Set APP to the staged macOS .app and LIB_DIR to dependency libraries")
endif()
include(BundleUtilities)
set(BU_CHMOD_BUNDLE_ITEMS ON)
# Copies non-system dependencies, rewrites install names, and verifies that all
# library references and symlinks stay in the bundle or macOS system directories.
fixup_bundle("${APP}" "" "${LIB_DIR}")
