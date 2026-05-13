function(vapor_copy_assets TARGET_NAME)
    if(NOT DEFINED VAPOR_ASSETS_DIR)
        message(WARNING "VAPOR_ASSETS_DIR is not defined. Cannot copy Vapor assets.")
        return()
    endif()

    if(NOT EXISTS "${VAPOR_ASSETS_DIR}")
        message(WARNING "Vapor assets directory not found at ${VAPOR_ASSETS_DIR}")
        return()
    endif()

    # Resolve output dir at configure time so we can use it in add_custom_command OUTPUT.
    # Prefer an explicit RUNTIME_OUTPUT_DIRECTORY if the target already has one set;
    # otherwise fall back to CMAKE_CURRENT_BINARY_DIR (CMake's default).
    get_target_property(_explicit_dir ${TARGET_NAME} RUNTIME_OUTPUT_DIRECTORY)
    if(_explicit_dir)
        set(_out_dir "${_explicit_dir}")
    else()
        set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}")
    endif()

    # Deduplicate: if another target in the same output dir already registered a
    # copy command, reuse that target instead of racing with a second copy_directory.
    string(MD5 _hash "${_out_dir}")
    set(_copy_target "copy_engine_assets_${_hash}")
    set(_stamp "${_out_dir}/assets/.engine_assets_stamp")

    if(NOT TARGET ${_copy_target})
        add_custom_command(
            OUTPUT  "${_stamp}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${VAPOR_ASSETS_DIR}" "${_out_dir}/assets"
            COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
            DEPENDS "${VAPOR_ASSETS_DIR}"
            COMMENT "Copying engine assets to ${_out_dir}/assets"
        )
        add_custom_target(${_copy_target} DEPENDS "${_stamp}")
    endif()

    add_dependencies(${TARGET_NAME} ${_copy_target})
endfunction()
