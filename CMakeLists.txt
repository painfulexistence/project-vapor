cmake_minimum_required(VERSION 3.20)

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