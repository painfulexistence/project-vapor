function(vapor_copy_assets TARGET_NAME)
    if(NOT DEFINED VAPOR_ASSETS_DIR)
        message(WARNING "VAPOR_ASSETS_DIR is not defined. Cannot copy Vapor assets.")
        return()
    endif()

    if(NOT EXISTS "${VAPOR_ASSETS_DIR}")
        message(WARNING "Vapor assets directory not found at ${VAPOR_ASSETS_DIR}")
        return()
    endif()

    # Each target gets a unique stamp so CMake doesn't complain about duplicate
    # OUTPUT paths. Concurrent same-content writes to the shared asset directory
    # are benign (identical bytes, typical asset sizes are written atomically).
    set(_stamp "$<TARGET_FILE_DIR:${TARGET_NAME}>/assets/.stamp_${TARGET_NAME}")

    add_custom_command(
        OUTPUT  "${_stamp}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${VAPOR_ASSETS_DIR}"
                "$<TARGET_FILE_DIR:${TARGET_NAME}>/assets"
        COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
        DEPENDS "${VAPOR_ASSETS_DIR}"
        COMMENT "Copying engine assets for ${TARGET_NAME}"
    )
    add_custom_target(copy_engine_assets_for_${TARGET_NAME} DEPENDS "${_stamp}")
    add_dependencies(${TARGET_NAME} copy_engine_assets_for_${TARGET_NAME})
endfunction()
