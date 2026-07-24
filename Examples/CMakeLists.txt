# Standalone example apps, each a small main.cpp over the Vapor library —
# the counterpart of Atmospheric's Examples/ tree.
#
# Drop the intermediate Examples/ path component from the build tree so each
# app lands at build/<Name>/ (e.g. build/MicroVoxel/microvoxel) instead of
# build/Examples/MicroVoxel/microvoxel. The value is inherited by each
# subdirectory below at add_subdirectory time; every example keeps its own
# output directory, so its Res/ (engine + game assets) stays self-contained
# exactly as before.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/MicroVoxel")
add_subdirectory(MicroVoxel)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/USDViewer")
add_subdirectory(USDViewer)
