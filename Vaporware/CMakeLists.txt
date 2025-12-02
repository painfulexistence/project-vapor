option(HOT_RELOAD "Enable hot reload (builds game as shared library)" ON)

set(GAME_SOURCES
    src/game.cpp
    src/camera_manager.cpp
)

if(HOT_RELOAD)
    # Hot reload mode: main.exe + Game.dll
    message(STATUS "Building with HOT_RELOAD enabled")

    # Build game as shared library
    add_library(Game SHARED
        ${GAME_SOURCES}
        src/game_module.cpp
    )
    target_include_directories(Game PRIVATE
        ${CMAKE_SOURCE_DIR}/Vapor/include
        ${CMAKE_SOURCE_DIR}/Vaporware/src
    )
    target_link_libraries(Game PRIVATE Vapor)
    if(NOT WIN32)
        target_compile_options(Game PRIVATE -fvisibility=hidden)
    endif()
    set_target_properties(Game PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    # Build main executable (loads DLL)
    add_executable(main src/main.cpp)
    target_compile_definitions(main PRIVATE HOT_RELOAD)
    target_link_libraries(main PRIVATE Vapor)
else()
    # Release mode: single executable
    message(STATUS "Building without HOT_RELOAD (release mode)")

    add_executable(main
        src/main.cpp
        ${GAME_SOURCES}
    )
    target_link_libraries(main PRIVATE Vapor)
endif()

# Shader compilation
find_program(GLSL_VALIDATOR "glslangValidator" REQUIRED)
file(GLOB_RECURSE SHADER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../Vapor/assets/shaders/*.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Vapor/assets/shaders/*.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Vapor/assets/shaders/*.comp"
)
foreach(GLSL ${SHADER_SOURCES})
    get_filename_component(SOURCE_NAME ${GLSL} NAME)
    set(SPIRV ${CMAKE_BINARY_DIR}/assets/shaders/${SOURCE_NAME}.spv)
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
