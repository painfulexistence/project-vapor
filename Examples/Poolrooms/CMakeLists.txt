include(AssetPipeline)

# Poolrooms: procedural liminal pool-hall walking demo and the showcase for
# the RHI water pass (see main.cpp). Scene assembly lives in the header-only
# poolgen helper next to main.cpp; the mesh and texture primitives it builds
# on are the engine's Vapor::procgen / Vapor::proctex toolkits.
add_executable(poolrooms main.cpp)
target_link_libraries(poolrooms PRIVATE Vapor)

vapor_copy_engine_assets(poolrooms)
vapor_copy_game_assets(poolrooms "${CMAKE_CURRENT_SOURCE_DIR}/assets")
# Publish the .metal shaders self-contained (flatten local #include) and
# compile-check them at build time. APPLE-only no-op elsewhere.
vapor_flatten_metal_shaders(poolrooms)
