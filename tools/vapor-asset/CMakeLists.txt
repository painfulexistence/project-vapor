cmake_minimum_required(VERSION 3.20)

project(vapor-asset VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies
find_package(args CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(cereal CONFIG REQUIRED)
find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h")

# meshoptimizer - fetch from GitHub
include(FetchContent)
FetchContent_Declare(
    meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG v0.21
)
FetchContent_MakeAvailable(meshoptimizer)

# Source files
set(SOURCES
    src/main.cpp
    src/lod_generator.cpp
    src/gltf_loader.cpp
)

add_executable(vapor-asset ${SOURCES})

target_include_directories(vapor-asset PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${TINYGLTF_INCLUDE_DIRS}
)

target_link_libraries(vapor-asset PRIVATE
    taywee::args
    fmt::fmt
    glm::glm
    cereal::cereal
    meshoptimizer
)

# Install
install(TARGETS vapor-asset
    RUNTIME DESTINATION bin
)
