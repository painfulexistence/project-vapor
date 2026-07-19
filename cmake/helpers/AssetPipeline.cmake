# AssetPipeline.cmake
# Functions for compiling shaders and copying assets to build output.
#
# vapor_compile_glsl_shaders(TARGET SHADER_DIR)
#   Compile all GLSL shaders under SHADER_DIR to SPIR-V and add as a
#   dependency of TARGET.
#
# vapor_copy_engine_assets(TARGET)
#   Copy the engine's built-in assets (shaders, fonts, etc.) to TARGET's
#   output directory under Res/. Requires VAPOR_ASSETS_DIR to be set.
#
# vapor_copy_game_assets(TARGET ASSETS_DIR)
#   Copy a game's own assets to TARGET's output directory under Res/.

function(vapor_compile_glsl_shaders TARGET SHADER_DIR)
    find_program(GLSL_VALIDATOR "glslangValidator" REQUIRED)

    # CONFIGURE_DEPENDS re-globs at build time and auto-reconfigures when the
    # set of shader sources changes. Without it, a shader added after the last
    # `cmake` configure is silently never compiled to .spv (and the renderer
    # then aborts at load with "Asset not found: shaders/<x>.spv").
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS
        "${SHADER_DIR}/*.vert"
        "${SHADER_DIR}/*.frag"
        "${SHADER_DIR}/*.comp"
        "${SHADER_DIR}/*.task"
        "${SHADER_DIR}/*.mesh"
    )

    set(_spirv_outputs)
    foreach(_glsl ${_sources})
        get_filename_component(_name ${_glsl} NAME)
        set(_spirv "${SHADER_DIR}/${_name}.spv")
        # Task/mesh shaders (GL_EXT_mesh_shader) need SPIR-V >= 1.4; target the
        # Vulkan 1.3 env for them (the device is created with API version 1.3).
        # Other stages keep the default target for maximal compatibility.
        set(_stage_flags "")
        if(_name MATCHES "\\.(task|mesh)$")
            set(_stage_flags --target-env vulkan1.3)
        endif()
        add_custom_command(
            OUTPUT  ${_spirv}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/assets/shaders"
            COMMAND ${GLSL_VALIDATOR} -V ${_stage_flags} ${_glsl} -o ${_spirv}
            DEPENDS ${_glsl}
            COMMENT "Compiling ${_name} to SPIR-V"
        )
        list(APPEND _spirv_outputs ${_spirv})

        # Bindless MDI variant: RHIMain.frag is compiled a second time with
        # -DBINDLESS (material textures from the set-3 runtime descriptor array
        # instead of per-draw set-2 slots). Needs SPIR-V 1.4+ for
        # GL_EXT_nonuniform_qualifier's descriptor-indexing capabilities.
        if(_name STREQUAL "RHIMain.frag")
            set(_bindless_spirv "${SHADER_DIR}/RHIMainBindless.frag.spv")
            add_custom_command(
                OUTPUT  ${_bindless_spirv}
                COMMAND ${GLSL_VALIDATOR} -V --target-env vulkan1.3 -DBINDLESS ${_glsl} -o ${_bindless_spirv}
                DEPENDS ${_glsl}
                COMMENT "Compiling ${_name} (BINDLESS) to SPIR-V"
            )
            list(APPEND _spirv_outputs ${_bindless_spirv})
        endif()
    endforeach()

    if(_spirv_outputs)
        string(MD5 _hash "${TARGET}${SHADER_DIR}")
        set(_shader_target "compile_shaders_${_hash}")
        add_custom_target(${_shader_target} ALL DEPENDS ${_spirv_outputs})
        add_dependencies(${TARGET} ${_shader_target})
        # Record for the asset-copy targets: they copy the .spv out of the
        # source tree, so they MUST run after compilation. Without this edge the
        # copy (a sibling dependency of the executable) can race ahead of
        # glslangValidator and publish a Res/ dir missing the freshly-added
        # .spv — the renderer then aborts with "Asset not found: shaders/<x>".
        set_property(GLOBAL APPEND PROPERTY VAPOR_SHADER_COMPILE_TARGETS ${_shader_target})
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

    # One copy target per OUTPUT directory avoids parallel-build races: the
    # destination is $<TARGET_FILE_DIR>/Res, so keying on the target's output
    # dir (not its source dir) de-dupes correctly even when targets in
    # different source subdirectories share a RUNTIME_OUTPUT_DIRECTORY (e.g.
    # the Examples all build into the build root). Falls back to the source
    # binary dir when the target sets no output dir.
    get_target_property(_outdir ${TARGET} RUNTIME_OUTPUT_DIRECTORY)
    if(NOT _outdir)
        set(_outdir "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    string(MD5 _hash "${_outdir}")
    set(_copy_target "copy_engine_assets_${_hash}")
    if(NOT TARGET ${_copy_target})
        add_custom_target(${_copy_target}
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${VAPOR_ASSETS_DIR}"
                    "$<TARGET_FILE_DIR:${TARGET}>/Res"
            COMMENT "Copying engine assets to $<TARGET_FILE_DIR:${TARGET}>/Res"
        )
        # Copy only after shaders are compiled (the .spv live in the asset tree).
        get_property(_shader_targets GLOBAL PROPERTY VAPOR_SHADER_COMPILE_TARGETS)
        if(_shader_targets)
            add_dependencies(${_copy_target} ${_shader_targets})
        endif()
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
        # Same ordering guard as engine assets: never publish Res/ before the
        # shaders it references have been compiled.
        get_property(_shader_targets GLOBAL PROPERTY VAPOR_SHADER_COMPILE_TARGETS)
        if(_shader_targets)
            add_dependencies(${_copy_target} ${_shader_targets})
        endif()
    endif()

    add_dependencies(${TARGET} ${_copy_target})
endfunction()
