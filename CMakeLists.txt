cmake_minimum_required(VERSION 3.20)

# Overlay ports patch registry entries whose upstream-archive hashes rotted in
# the vcpkg submodule's snapshot. Must be set before project() runs the vcpkg
# toolchain; APPEND keeps any overlays passed on the command line.
list(APPEND VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg-overlay-ports")

project(vapor VERSION 0.0.1 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/helpers")

add_subdirectory(Vapor)

# Vaporware (demo app), examples, tests, and benchmarks are only built when
# Vapor is the root project. Downstream games that
# add_subdirectory(project-vapor) skip these automatically.
if(PROJECT_IS_TOP_LEVEL)
    add_subdirectory(Vaporware)
    add_subdirectory(Examples)
    enable_testing()
    add_subdirectory(tests)
    add_subdirectory(benchmarks)
endif()

# install(FILES LICENSE DESTINATION "share/${PROJECT_NAME}")