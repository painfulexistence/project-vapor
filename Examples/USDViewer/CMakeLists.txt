include(AssetPipeline)

# USDViewer: fly-camera viewer for USD scenes through the SceneBlueprint
# import line (see main.cpp).
add_executable(usdviewer main.cpp)
target_link_libraries(usdviewer PRIVATE Vapor)

vapor_copy_engine_assets(usdviewer)
vapor_copy_game_assets(usdviewer "${CMAKE_CURRENT_SOURCE_DIR}/assets")
