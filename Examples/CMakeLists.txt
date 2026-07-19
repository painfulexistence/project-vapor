# Standalone example apps, each a small main.cpp over the Vapor library —
# the counterpart of Atmospheric's Examples/ tree.
#
# Binaries land directly in the build root (build/microvoxel, not
# build/Examples/MicroVoxel/microvoxel) so they're trivial to find and run.
# This propagates into the subdirectories below; the engine-asset copy
# de-dupes by output dir (see AssetPipeline.cmake), so the three examples
# sharing the build root still stage Res/ exactly once, race-free.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_subdirectory(MicroVoxel)
add_subdirectory(USDViewer)
add_subdirectory(TerrainStreaming)
