function(vapor_copy_assets TARGET_NAME)
    if(NOT DEFINED VAPOR_ASSETS_DIR)
        message(WARNING "VAPOR_ASSETS_DIR is not defined. Cannot copy Vapor assets.")
        return()
    endif()

    if(NOT EXISTS "${VAPOR_ASSETS_DIR}")
        message(WARNING "Vapor assets directory not found at ${VAPOR_ASSETS_DIR}")
        return()
    endif()

    # Resolve the destination at configure time so we can deduplicate.
    # Targets with an explicit RUNTIME_OUTPUT_DIRECTORY get their own copy target;
    # targets using the generator default share one per binary dir (and config).
    get_target_property(_explicit_dir ${TARGET_NAME} RUNTIME_OUTPUT_DIRECTORY)
    if(_explicit_dir)
        string(MD5 _hash "${_explicit_dir}")
        set(_dest "${_explicit_dir}/assets")
    elseif(CMAKE_CONFIGURATION_TYPES)
        # Multi-config generator (Ninja Multi-Config, Xcode, VS):
        # default runtime output is <binary_dir>/<config>. $<CONFIG> in COMMAND
        # is always valid and expands correctly per configuration.
        string(MD5 _hash "${CMAKE_BINARY_DIR}")
        set(_dest "${CMAKE_BINARY_DIR}/$<CONFIG>/assets")
    else()
        # Single-config generator: executables go directly to the binary dir.
        string(MD5 _hash "${CMAKE_BINARY_DIR}")
        set(_dest "${CMAKE_BINARY_DIR}/assets")
    endif()

    # Guard: only one copy target per resolved destination directory.
    # All targets sharing the same destination depend on this single target,
    # so parallel builds never race on the same asset directory.
    set(_copy_target "copy_engine_assets_${_hash}")
    if(NOT TARGET ${_copy_target})
        add_custom_target(${_copy_target}
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${VAPOR_ASSETS_DIR}" "${_dest}"
            COMMENT "Copying engine assets to ${_dest}"
        )
    endif()

    add_dependencies(${TARGET_NAME} ${_copy_target})
endfunction()
