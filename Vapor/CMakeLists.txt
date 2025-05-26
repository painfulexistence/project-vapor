set(BUILD_SHARED_LIBS OFF)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_TESTS_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.14
    GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(SDL3)
set(ARGS_BUILD_EXAMPLE OFF)
set(ARGS_BUILD_UNITTESTS OFF)
FetchContent_Declare(args
    GIT_REPOSITORY https://github.com/Taywee/args
    GIT_TAG 6.4.6
    GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(args)
find_package(fmt CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(Vulkan REQUIRED)

add_subdirectory(metal)

set(SOURCES
    main.cpp
    helper.cpp
    renderer_metal.cpp
    renderer_vulkan.cpp
    graphics.cpp
	asset_manager.cpp
)

add_executable(main ${SOURCES})
target_compile_features(main PRIVATE cxx_std_20)
set_target_properties(main PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(main PRIVATE SDL3::SDL3 metal fmt::fmt glm::glm Vulkan::Vulkan args)

find_program(GLSL_VALIDATOR "glslangValidator" REQUIRED)
file(GLOB_RECURSE SHADER_SOURCES CONFIGURE_DEPENDS
	${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/*.vert
	${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/*.frag
	${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/*.comp
)
foreach(GLSL ${SHADER_SOURCES})
	get_filename_component(SOURCE_NAME ${GLSL} NAME)
	set(SPIRV ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/${SOURCE_NAME}.spv)
	add_custom_command(
		OUTPUT ${SPIRV}
		COMMAND ${GLSL_VALIDATOR} -V ${GLSL} -o ${SPIRV}
		DEPENDS ${GLSL}
	)
	list(APPEND SPIRV_BINARY_FILES ${SPIRV})
endforeach(GLSL)

add_custom_target(copy_assets COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:main>/assets DEPENDS ${SPIRV_BINARY_FILES})
add_dependencies(main copy_assets)