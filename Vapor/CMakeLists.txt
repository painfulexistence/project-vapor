set(BUILD_SHARED_LIBS OFF)
option(SDL_TEST_LIBRARY OFF)
FetchContent_Declare(SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    # TODO: Upgrade to SDL3 when it comes out
    GIT_TAG release-2.30.3
    GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(SDL2)
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
target_link_libraries(main PRIVATE SDL2::SDL2 metal fmt::fmt glm::glm Vulkan::Vulkan)

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