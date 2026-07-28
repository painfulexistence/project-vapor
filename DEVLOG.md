# Devlog

- 2024/04/01 - Created the repo 🎉!
- 2026/07/16 - Stochastic RT shadows.
- 2026/07/18 - MicroVoxel port
- 2026/07/22 - Adaptive GPU tessellation
- 2026/07/26 - RHI water pass (both backends) and the Poolrooms example
- 2026/07/26 - Water rewrite: FFT spectral simulation (GPU IFFT) + planar reflections (mirrored camera, waterline clip)
- 2026/07/26 - Vapor::procgen: engine-side procedural mesh toolkit (sweeps, lathe, caps with holes, box CSG, transform instancing, bake validation) extracted from Poolrooms, plus a catch2 suite
- 2026/07/28 - Vapor::proctex + scene-JSON `textures` block: procedural textures declared by generator name and referenced from material slots as `@name`, content-addressed into the renderer's texture cache
- 2026/07/28 - Scene-JSON `procMesh` block: entities name a mesh generator and its params, geometry comes back as per-material buckets with colliders co-emitted, rebuilt per load rather than stored
- 2026/07/28 - Scene-JSON `water` component + WaterSystem: the FFT surface declared as data, grouped by update cost so the look stays live-tunable while the grid and spectrum are pushed only when they change
- 2026/07/28 - procMesh colliders reach physics: generated boxes become child entities, and the new engine-side PhysicsBodySystem turns any authored collider into a Jolt body (thin boxes now shrink their convex radius instead of throwing)
