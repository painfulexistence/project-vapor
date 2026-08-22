cmake_minimum_required(VERSION 3.20)

# Overlay ports patch registry entries whose upstream-archive hashes rotted in
# the vcpkg submodule's snapshot. Must be set before project() runs the vcpkg
# toolchain; APPEND keeps any overlays passed on the command line.
list(APPEND VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg-overlay-ports")

project(vapor VERSION 0.0.1 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# /utf-8: sources are UTF-8 without BOM, but MSVC otherwise decodes them using
# the system ANSI codepage. That is harmless on CP1252 (every byte is a valid
# single character, so non-ASCII literals merely become mojibake) — which is why
# the windows-latest runner never sees it. On a double-byte codepage such as
# CP950 or CP932 a lead byte consumes the following byte, swallowing the closing
# quote of any literal containing non-ASCII text (C2001, then C1057).
# /bigobj: Debug builds of Vaporware/src/main.cpp exceed the COFF section limit.
if(MSVC)
    add_compile_options(/utf-8 /bigobj)
endif()

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