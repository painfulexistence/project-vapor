include(AssetPipeline)

# GLTFViewer: fly-camera viewer for glTF/GLB models through the SceneBlueprint
# import line (see main.cpp). Sibling of USDViewer.
add_executable(gltfviewer main.cpp)
target_link_libraries(gltfviewer PRIVATE Vapor)

vapor_copy_engine_assets(gltfviewer)
vapor_copy_game_assets(gltfviewer "${CMAKE_CURRENT_SOURCE_DIR}/assets")
# Publish the .metal shaders self-contained (flatten local #include) and
# compile-check them at build time. APPLE-only no-op elsewhere.
vapor_flatten_metal_shaders(gltfviewer)
