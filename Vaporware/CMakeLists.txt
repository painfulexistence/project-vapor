set(SOURCES
    src/main.cpp
)

add_executable(main ${SOURCES})
target_link_libraries(main PRIVATE Vapor)

vapor_copy_assets(main)

add_custom_target(copy_assets COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/assets" $<TARGET_FILE_DIR:main>/assets
)
add_dependencies(main copy_assets)
