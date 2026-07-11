# Vendored third-party headers

## clusterlod.h
Single-header cluster-LOD hierarchy builder from meshoptimizer's `demo/`.
- Source: https://github.com/zeux/meshoptimizer/blob/master/demo/clusterlod.h
- Pinned to meshoptimizer commit `ce321f27536cdbc61ef86622d8ef11fedf5492a8`.
- Requires bleeding-edge meshoptimizer APIs (meshopt_extractMeshletIndices,
  meshopt_buildMeshletsSpatial/Flex, meshopt_partitionClusters,
  meshopt_simplifyWithAttributes, meshopt_computeSphereBounds,
  meshopt_spatialClusterPoints) that are NOT in released/vcpkg versions yet.
  Because of that, meshoptimizer is built from this exact commit via CMake
  FetchContent in Vapor/CMakeLists.txt (not vcpkg). Keep the FetchContent
  GIT_TAG in sync with this header's pin when updating either one.
- Include exactly one .cpp with `#define CLUSTERLOD_IMPLEMENTATION` before the include.
