set(SOURCES
    src/main.cpp
    src/camera_manager.cpp
)

add_executable(main ${SOURCES})
target_link_libraries(main PRIVATE Vapor)

# Link dl for dynamic loading on Linux
if(UNIX AND NOT APPLE)
    target_link_libraries(main PRIVATE dl)
endif()

# Build gameplay module
add_subdirectory(gameplay)

find_program(GLSL_VALIDATOR "glslangValidator" REQUIRED)
file(GLOB_RECURSE SHADER_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/../Vapor/assets/shaders/*.vert"
	"${CMAKE_CURRENT_SOURCE_DIR}/../Vapor/assets/shaders/*.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/../Vapor/assets/shaders/*.comp"
)
foreach(GLSL ${SHADER_SOURCES})
	get_filename_component(SOURCE_NAME ${GLSL} NAME)
	set(SPIRV ${CMAKE_CURRENT_BINARY_DIR}/assets/shaders/${SOURCE_NAME}.spv)
	add_custom_command(
		OUTPUT ${SPIRV}
		COMMAND ${GLSL_VALIDATOR} -V ${GLSL} -o ${SPIRV}
		DEPENDS ${GLSL}
	)
	list(APPEND SPIRV_BINARY_FILES ${SPIRV})
endforeach(GLSL)

add_custom_target(copy_assets COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/../Vapor/assets" $<TARGET_FILE_DIR:main>/assets
    DEPENDS ${SPIRV_BINARY_FILES}
)
add_dependencies(main copy_assets)