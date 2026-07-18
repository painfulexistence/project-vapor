include(AssetPipeline)

# MicroVoxel demo: raymarched sparse-brick voxel dioramas (see main.cpp).
add_executable(microvoxel main.cpp)
target_link_libraries(microvoxel PRIVATE Vapor)

# Engine assets only (shaders/fonts); this example has no assets of its own —
# the whole scene is procedural.
vapor_copy_engine_assets(microvoxel)
