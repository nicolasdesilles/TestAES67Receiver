# Convenience toolchain wrapper.
#
# Usage:
#   cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain.cmake -DVCPKG_TARGET_TRIPLET=linux-arm64
#
# This wraps the vcpkg toolchain shipped as a submodule of ravennakit and sets the overlay triplets path.

cmake_minimum_required(VERSION 3.22)

set(_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
set(_VCPKG_DIR "${_ROOT_DIR}/external/ravennakit/submodules/vcpkg")

if (NOT EXISTS "${_VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake")
  message(FATAL_ERROR "vcpkg toolchain not found at: ${_VCPKG_DIR}. Did you init submodules?")
endif ()

set(VCPKG_OVERLAY_TRIPLETS "${_ROOT_DIR}/external/ravennakit/triplets" CACHE STRING "" FORCE)

# Important: our project root does not have a vcpkg.json, but ravennakit does.
# Point vcpkg manifest mode at ravennakit so dependencies like fmt/boost/portaudio get installed.
set(VCPKG_MANIFEST_DIR "${_ROOT_DIR}/external/ravennakit" CACHE STRING "" FORCE)

# For older vcpkg versions, manifests may require an explicit feature flag (harmless on newer versions).
set(VCPKG_FEATURE_FLAGS "manifests" CACHE STRING "" FORCE)

include("${_VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake")


