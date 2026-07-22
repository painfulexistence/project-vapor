include(AssetPipeline)

# MicroVoxel demo: raymarched sparse-brick voxel dioramas (see main.cpp).
add_executable(microvoxel main.cpp)
target_link_libraries(microvoxel PRIVATE Vapor)

# Engine assets (shaders/fonts) + the demo's declarative scenes
# (assets/scenes/main.json = three dioramas, big.json = the streaming world).
vapor_copy_engine_assets(microvoxel)
vapor_copy_game_assets(microvoxel "${CMAKE_CURRENT_SOURCE_DIR}/assets")
