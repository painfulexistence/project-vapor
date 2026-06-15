list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(VaporMacros)

set(SOURCES
    src/main.cpp
)

add_executable(main ${SOURCES})
target_link_libraries(main PRIVATE Vapor)

vapor_copy_assets(main)

add_custom_target(copy_assets COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/assets" $<TARGET_FILE_DIR:main>/Res
)
add_dependencies(main copy_assets)

# Whiteout Survival ad-game — a focused ECS minigame on the same engine.
# Headers live alongside src/survival/main.cpp and include each other by bare
# name; src/survival is on the include path so they always resolve to the
# survival headers (never the demo's same-named components.hpp/systems.hpp).
add_executable(survival src/survival/main.cpp)
target_link_libraries(survival PRIVATE Vapor)
target_include_directories(survival PRIVATE src/survival)

vapor_copy_assets(survival)

add_custom_target(copy_assets_survival COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/assets" $<TARGET_FILE_DIR:survival>/Res
)
add_dependencies(survival copy_assets_survival)
