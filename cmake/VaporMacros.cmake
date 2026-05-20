function(vapor_copy_assets TARGET_NAME)
    if(NOT DEFINED VAPOR_ASSETS_DIR)
        message(WARNING "VAPOR_ASSETS_DIR is not defined. Cannot copy Vapor assets.")
        return()
    endif()

    if(NOT EXISTS "${VAPOR_ASSETS_DIR}")
        message(WARNING "Vapor assets directory not found at ${VAPOR_ASSETS_DIR}")
        return()
    endif()

    # One copy target per CMakeLists.txt binary directory: all targets defined in
    # the same directory share the same runtime output directory, so one copy is
    # sufficient and avoids parallel-build races on the same destination.
    string(MD5 _hash "${CMAKE_CURRENT_BINARY_DIR}")
    set(_copy_target "copy_engine_assets_${_hash}")
    if(NOT TARGET ${_copy_target})
        add_custom_target(${_copy_target}
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${VAPOR_ASSETS_DIR}"
                    "$<TARGET_FILE_DIR:${TARGET_NAME}>/Res"
            COMMENT "Copying engine assets to $<TARGET_FILE_DIR:${TARGET_NAME}>/Res"
        )
    endif()

    add_dependencies(${TARGET_NAME} ${_copy_target})
endfunction()
