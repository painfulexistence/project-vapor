set(BUILD_SHARED_LIBS OFF)
option(SDL_TEST_LIBRARY OFF)
FetchContent_Declare(SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    # TODO: Upgrade to SDL3 when it comes out
    GIT_TAG release-2.30.3
    GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(SDL2)
find_package(fmt CONFIG REQUIRED)

add_subdirectory(metal)

set(SOURCES
    main.cpp
    helper.cpp
    renderer_metal.cpp
)

add_executable(main ${SOURCES})
target_compile_features(main PRIVATE cxx_std_20)
set_target_properties(main PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(main PRIVATE SDL2::SDL2 metal fmt::fmt)

add_custom_target(copy_assets COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets ${PROJECT_BINARY_DIR}/assets)
add_dependencies(main copy_assets)