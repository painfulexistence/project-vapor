include(AssetPipeline)

# TerrainStreaming: 10km x 10km streamed heightfield terrain (see main.cpp).
# Engine assets (shaders/fonts) + the declarative scene (assets/scenes/main.json).
add_executable(terrainstreaming main.cpp)
target_link_libraries(terrainstreaming PRIVATE Vapor)

vapor_copy_engine_assets(terrainstreaming)
vapor_copy_game_assets(terrainstreaming "${CMAKE_CURRENT_SOURCE_DIR}/assets")
