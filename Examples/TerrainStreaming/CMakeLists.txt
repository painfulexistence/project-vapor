include(AssetPipeline)

# TerrainStreaming: 10km x 10km streamed heightfield terrain (see main.cpp).
# Everything is procedural — engine assets (shaders/fonts) only.
add_executable(terrainstreaming main.cpp)
target_link_libraries(terrainstreaming PRIVATE Vapor)

vapor_copy_engine_assets(terrainstreaming)
