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
        # Copy only after shaders are compiled (the .spv live in the asset tree).
        get_property(_shader_targets GLOBAL PROPERTY VAPOR_SHADER_COMPILE_TARGETS)
        if(_shader_targets)
            add_dependencies(${_copy_target} ${_shader_targets})
        endif()
    endif()

    add_dependencies(${TARGET} ${_copy_target})
endfunction()

# vapor_flatten_metal_shaders(TARGET)
#   Flatten local `#include "Res/shaders/X.metal"` directives in the Metal
#   shaders published to TARGET's Res/ dir, so each .metal is self-contained.
#
#   Why: the renderer compiles .metal from source at runtime (newLibraryWithSource),
#   and Metal resolves a relative `#include` against the process CWD. That works
#   for the running app, but a GPU-trace *replay* tool has a different CWD and
#   cannot find the included file — the captured program_source keeps the
#   unresolved `#include` and replay fails with "file not found". Flattening at
#   build time removes the relative include entirely.
#
#   APPLE-only (only the Metal backend reads .metal). Implemented as its own
#   always-run custom target ordered AFTER the asset copy, so it also re-flattens
#   when only a shader changed (a POST_BUILD on the app would be skipped when the
#   app doesn't relink, leaving the copy's raw .metal — and replay broken again).
function(vapor_flatten_metal_shaders TARGET)
    if(NOT APPLE)
        return()
    endif()
    if(NOT DEFINED VAPOR_ASSETS_DIR)
        return()
    endif()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_FOUND)
        message(WARNING "[Vapor] Python3 not found — Metal shaders not flattened; "
                        "GPU-trace replay may fail on relative #include.")
        return()
    endif()
    # project-vapor's root (set by project(vapor)) — robust whether this repo is
    # the top project or add_subdirectory'd by a downstream game, and independent
    # of CMAKE_CURRENT_LIST_DIR (which inside a function is the CALLER's dir).
    set(_flatten_script "${vapor_SOURCE_DIR}/scripts/flatten_metal_includes.py")
    file(GLOB _metal_sources CONFIGURE_DEPENDS "${VAPOR_ASSETS_DIR}/shaders/*.metal")
    set(_res_shaders "$<TARGET_FILE_DIR:${TARGET}>/Res/shaders")

    string(MD5 _hash "${CMAKE_CURRENT_BINARY_DIR}${TARGET}")
    set(_flatten_target "flatten_metal_${_hash}")
    add_custom_target(${_flatten_target}
        COMMENT "Flattening Metal #include directives in Res/shaders")
    foreach(_metal ${_metal_sources})
        get_filename_component(_name ${_metal} NAME)
        add_custom_command(TARGET ${_flatten_target} POST_BUILD
            COMMAND ${Python3_EXECUTABLE} "${_flatten_script}"
                    "${_metal}" "${VAPOR_ASSETS_DIR}/shaders" "${_res_shaders}/${_name}"
            VERBATIM)
    endforeach()
    # Order after the engine asset copy (same binary dir → same hash) so the raw
    # .metal is published first, then rewritten in place.
    string(MD5 _copy_hash "${CMAKE_CURRENT_BINARY_DIR}")
    if(TARGET copy_engine_assets_${_copy_hash})
        add_dependencies(${_flatten_target} copy_engine_assets_${_copy_hash})
    endif()
    add_dependencies(${TARGET} ${_flatten_target})

    # ── Build-time validation: compile each shader with `xcrun metal` so MSL
    # errors (undeclared identifiers, buffer collisions, …) fail the BUILD instead
    # of only surfacing at the user's createRenderer. OUTPUT-tracked (per-shader
    # .air), so only changed shaders recompile — not all ~70 every build. Compiles
    # the FLATTENED shader (source `#include "Res/shaders/X"` can't be resolved by
    # `-I`, since the path is written with the Res/shaders/ prefix). Fatal: a
    # compile error stops the build.
    #
    # Escape hatch: -DVAPOR_VALIDATE_METAL=OFF. Uses the default `xcrun metal`
    # language version (matches the runtime newLibrary, which passes no options);
    # if a shader needs a specific -std/target on your toolchain, add it to the
    # `xcrun metal` command below.
    option(VAPOR_VALIDATE_METAL "Compile-check Metal shaders at build via xcrun metal" ON)
    if(NOT VAPOR_VALIDATE_METAL)
        return()
    endif()
    set(_scratch "${CMAKE_CURRENT_BINARY_DIR}/metal_validate")
    set(_common "${VAPOR_ASSETS_DIR}/shaders/3d_common.metal")
    set(_air_outputs)
    foreach(_metal ${_metal_sources})
        get_filename_component(_name ${_metal} NAME)
        add_custom_command(
            OUTPUT  "${_scratch}/${_name}.air"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_scratch}"
            COMMAND ${Python3_EXECUTABLE} "${_flatten_script}"
                    "${_metal}" "${VAPOR_ASSETS_DIR}/shaders" "${_scratch}/${_name}"
            COMMAND xcrun metal -c "${_scratch}/${_name}" -o "${_scratch}/${_name}.air"
            # Re-validate when the shader OR the shared include changes.
            DEPENDS "${_metal}" "${_common}" "${_flatten_script}"
            COMMENT "Validating (xcrun metal) ${_name}"
            VERBATIM
        )
        list(APPEND _air_outputs "${_scratch}/${_name}.air")
    endforeach()
    if(_air_outputs)
        add_custom_target(validate_metal_${_hash} ALL DEPENDS ${_air_outputs})
        add_dependencies(${TARGET} validate_metal_${_hash})
    endif()
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
