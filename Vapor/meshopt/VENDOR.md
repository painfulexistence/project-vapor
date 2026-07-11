# Vendored third-party headers

## clusterlod.h
Single-header cluster-LOD hierarchy builder from meshoptimizer's `demo/`.
- Source: https://github.com/zeux/meshoptimizer/blob/master/demo/clusterlod.h
- Pinned to meshoptimizer commit `ce321f2` (post-v1.0).
- Requires a meshoptimizer recent enough to provide meshopt_buildMeshletsSpatial,
  meshopt_buildMeshletsFlex, meshopt_partitionClusters, meshopt_simplifyWithAttributes,
  meshopt_computeSphereBounds, meshopt_spatialClusterPoints. If the vcpkg
  meshoptimizer baseline is older, bump it (vcpkg.json builtin-baseline / overrides).
- Include exactly one .cpp with `#define CLUSTERLOD_IMPLEMENTATION` before the include.
