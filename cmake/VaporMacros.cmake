function(vapor_copy_assets TARGET_NAME)
    if(NOT DEFINED VAPOR_ASSETS_DIR)
        message(WARNING "VAPOR_ASSETS_DIR is not defined. Cannot copy Vapor assets.")
        return()
    endif()

    if(NOT EXISTS "${VAPOR_ASSETS_DIR}")
        message(WARNING "Vapor assets directory not found at ${VAPOR_ASSETS_DIR}")
        return()
    endif()

    add_custom_target(copy_engine_assets_for_${TARGET_NAME}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${VAPOR_ASSETS_DIR}"
        "$<TARGET_FILE_DIR:${TARGET_NAME}>/assets"
        COMMENT "Copying engine assets to TARGET asset directory"
    )
    add_dependencies(${TARGET_NAME} copy_engine_assets_for_${TARGET_NAME})
endfunction()


