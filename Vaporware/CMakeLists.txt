include(AssetPipeline)

add_executable(main src/main.cpp)
target_link_libraries(main PRIVATE Vapor)

vapor_copy_engine_assets(main)
vapor_copy_game_assets(main "${CMAKE_CURRENT_SOURCE_DIR}/assets")
# Rewrite the published .metal shaders self-contained (flatten local #include),
# so GPU-trace replay can recompile them without the app's CWD. APPLE-only no-op
# elsewhere. Runs POST_BUILD, i.e. after the asset copies above.
vapor_flatten_metal_shaders(main)
