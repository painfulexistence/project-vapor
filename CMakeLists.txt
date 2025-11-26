cmake_minimum_required(VERSION 3.20)

project(vapor LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL3 CONFIG REQUIRED)

add_subdirectory(Vapor)

add_executable(vapor_main src/main.cpp)
target_link_libraries(vapor_main PRIVATE Vapor)
target_include_directories(vapor_main PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Vapor/include/Vapor")

find_program(GLSL_VALIDATOR "glslangValidator" REQUIRED)
file(GLOB_RECURSE SHADER_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_DIR}/Vapor/assets/shaders/*.vert"
	"${CMAKE_CURRENT_DIR}/Vapor/assets/shaders/*.frag"
	"${CMAKE_CURRENT_DIR}/Vapor/assets/shaders/*.comp"
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
    "${CMAKE_CURRENT_DIR}/Vapor/assets" $<TARGET_FILE_DIR:vapor_main>/assets 
    DEPENDS ${SPIRV_BINARY_FILES}
)
add_dependencies(vapor_main copy_assets)


# -------------------------------------------------------------------
# 處理 CMake 套件的匯出與安裝
# -------------------------------------------------------------------
# Skip EXPORT and Config generation as per request

# 安裝 LICENSE
install(FILES LICENSE DESTINATION "share/${PROJECT_NAME}")