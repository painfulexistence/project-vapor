include(AssetPipeline)

# GLTFViewer: fly-camera viewer for glTF/GLB models through the SceneBlueprint
# import line (see main.cpp). Sibling of USDViewer.
add_executable(gltfviewer main.cpp)
target_link_libraries(gltfviewer PRIVATE Vapor)

vapor_copy_engine_assets(gltfviewer)
vapor_copy_game_assets(gltfviewer "${CMAKE_CURRENT_SOURCE_DIR}/assets")
