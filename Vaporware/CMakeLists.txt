include("${CMAKE_SOURCE_DIR}/cmake/helpers/AssetPipeline.cmake")

add_executable(main src/main.cpp)
target_link_libraries(main PRIVATE Vapor)

vapor_copy_engine_assets(main)
vapor_copy_game_assets(main "${CMAKE_CURRENT_SOURCE_DIR}/assets")
