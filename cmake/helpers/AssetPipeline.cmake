# AssetPipeline.cmake
# Functions for compiling shaders and copying assets to build output.
#
# vapor_compile_shaders(TARGET SHADER_DIR)
#   Compile all GLSL shaders under SHADER_DIR to SPIR-V and add as a
#   dependency of TARGET.
#
# vapor_copy_engine_assets(TARGET)
#   Copy the engine's built-in assets (shaders, fonts, etc.) to TARGET's
#   output directory under Res/. Requires VAPOR_ASSETS_DIR to be set.
#
# vapor_copy_game_assets(TARGET ASSETS_DIR)
#   Copy a game's own assets to TARGET's output directory under Res/.

function(vapor_compile_shaders TARGET SHADER_DIR)
    find_program(GLSL_VALIDATOR "glslangValidator" REQUIRED)

    file(GLOB_RECURSE _sources
        "${SHADER_DIR}/*.vert"
        "${SHADER_DIR}/*.frag"
        "${SHADER_DIR}/*.comp"
    )

    set(_spirv_outputs)
    foreach(_glsl ${_sources})
        get_filename_component(_name ${_glsl} NAME)
        set(_spirv "${SHADER_DIR}/${_name}.spv")
        add_custom_command(
            OUTPUT  ${_spirv}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/assets/shaders"
            COMMAND ${GLSL_VALIDATOR} -V ${_glsl} -o ${_spirv}
            DEPENDS ${_glsl}
            COMMENT "Compiling ${_name} to SPIR-V"
        )
        list(APPEND _spirv_outputs ${_spirv})
    endforeach()

    if(_spirv_outputs)
        string(MD5 _hash "${TARGET}${SHADER_DIR}")
        set(_shader_target "compile_shaders_${_hash}")
        add_custom_target(${_shader_target} ALL DEPENDS ${_spirv_outputs})
        add_dependencies(${TARGET} ${_shader_target})
    endif()
endfunction()

function(vapor_copy_engine_assets TARGET)
    if(NOT DEFINED VAPOR_ASSETS_DIR)
        message(WARNING "[Vapor] VAPOR_ASSETS_DIR is not defined — engine assets not copied.")
        return()
    endif()
    if(NOT EXISTS "${VAPOR_ASSETS_DIR}")
        message(WARNING "[Vapor] Engine assets directory not found: ${VAPOR_ASSETS_DIR}")
        return()
    endif()

    # One copy target per binary directory avoids parallel-build races when
    # multiple targets in the same directory all depend on this.
    string(MD5 _hash "${CMAKE_CURRENT_BINARY_DIR}")
    set(_copy_target "copy_engine_assets_${_hash}")
    if(NOT TARGET ${_copy_target})
        add_custom_target(${_copy_target}
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${VAPOR_ASSETS_DIR}"
                    "$<TARGET_FILE_DIR:${TARGET}>/Res"
            COMMENT "Copying engine assets to $<TARGET_FILE_DIR:${TARGET}>/Res"
        )
    endif()

    add_dependencies(${TARGET} ${_copy_target})
endfunction()

function(vapor_copy_game_assets TARGET ASSETS_DIR)
    if(NOT EXISTS "${ASSETS_DIR}")
        message(WARNING "[Vapor] Game assets directory not found: ${ASSETS_DIR}")
        return()
    endif()

    string(MD5 _hash "${CMAKE_CURRENT_BINARY_DIR}${ASSETS_DIR}")
    set(_copy_target "copy_game_assets_${_hash}")
    if(NOT TARGET ${_copy_target})
        add_custom_target(${_copy_target}
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${ASSETS_DIR}"
                    "$<TARGET_FILE_DIR:${TARGET}>/Res"
            COMMENT "Copying game assets to $<TARGET_FILE_DIR:${TARGET}>/Res"
        )
    endif()

    add_dependencies(${TARGET} ${_copy_target})
endfunction()
