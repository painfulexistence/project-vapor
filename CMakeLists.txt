cmake_minimum_required(VERSION 3.20)

project(vapor VERSION 0.0.1 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL3 CONFIG REQUIRED)

add_subdirectory(Vapor)
add_subdirectory(Vaporware)

enable_testing()
add_subdirectory(tests)

# install(FILES LICENSE DESTINATION "share/${PROJECT_NAME}")