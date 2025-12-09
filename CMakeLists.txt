cmake_minimum_required(VERSION 3.20)

project(vapor VERSION 0.0.1 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL3 CONFIG REQUIRED)

add_subdirectory(Vapor)
add_subdirectory(Vaporware)

# Tools (optional, enable with -DBUILD_TOOLS=ON)
option(BUILD_TOOLS "Build asset processing tools" OFF)
if(BUILD_TOOLS)
    add_subdirectory(tools/vapor-asset)
endif()

# install(FILES LICENSE DESTINATION "share/${PROJECT_NAME}")