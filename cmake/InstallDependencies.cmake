# InstallDependencies.cmake
# Include this BEFORE project() in your downstream CMakeLists.txt to
# automatically configure vcpkg with Vapor's bundled dependencies.
#
# Usage:
#   include(project-vapor/cmake/InstallDependencies.cmake)
#   project(MyGame LANGUAGES CXX)
#   add_subdirectory(project-vapor)
#
# The downstream project does not need its own vcpkg.json or overlay ports.

set(_VAPOR_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

# Auto-set vcpkg toolchain if not already configured.
# Must be set before project() for the toolchain to take effect.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${_VAPOR_ROOT}/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "[Vapor] vcpkg toolchain")
    message(STATUS "[Vapor] Using bundled vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
endif()

# Point vcpkg at Vapor's vcpkg.json so downstream games don't need their own.
# VCPKG_MANIFEST_DIR overrides CMAKE_SOURCE_DIR as the manifest search root.
if(NOT DEFINED VCPKG_MANIFEST_DIR)
    set(VCPKG_MANIFEST_DIR "${_VAPOR_ROOT}"
        CACHE STRING "[Vapor] vcpkg manifest directory")
    message(STATUS "[Vapor] Using engine vcpkg manifest at: ${VCPKG_MANIFEST_DIR}/vcpkg.json")
endif()

# Install packages inside the engine tree so they are shared across downstream
# builds and don't have to be re-downloaded per game project.
if(NOT DEFINED VCPKG_INSTALLED_DIR)
    set(VCPKG_INSTALLED_DIR "${_VAPOR_ROOT}/vcpkg_installed"
        CACHE STRING "[Vapor] vcpkg installed packages directory")
endif()
