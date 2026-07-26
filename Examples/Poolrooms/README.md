# Poolrooms

A liminal "pool core" walking demo — an endless-afternoon tiled pool hall in
the backrooms style — and the showcase scene for the engine's RHI water pass.

## What it demonstrates

- **FFT water simulation** (`Renderer::waterSimPass`): a Tessendorf-style
  spectral surface — directional Phillips spectrum with capillary cutoff and
  finite-depth (2 m pool!) capillary-gravity dispersion, evolved per frame and
  inverse-FFT'd on the GPU (256², shared-memory radix-2, both backends) into
  a choppy displacement field plus a normal/whitecap-Jacobian map.
- **Planar reflections** (`Renderer::waterReflectionPass`): the scene
  re-rendered through a Householder-mirrored camera into a half-res HDR
  target with waterline clipping and flipped culling (the architecture
  Atmospheric's VoxelWorld uses) — exact for a flat pool, where SSR
  fundamentally fails: the ceiling and walls a pool reflects are off-screen.
  The surface samples it projectively with ripple distortion, falling back to
  the IBL cubemap at the borders.
- **Projected caustics** (`Renderer::waterCausticsPass`): an animated caustic
  web boosts every submerged pixel inside the pool volume, chromatically
  fringed, sun-tinted and depth-faded — the surface refracts the caustic-lit
  floor, so the pattern reads correctly through the ripples.
- **Fully procedural environment**: every mesh is generated at startup —
  beveled ceramic tiles with recessed grout channels (real geometry, not a
  normal map), a bullnose coping sweep around the rim, a waterline mosaic
  band, cove trim, six skylight wells, tiled pillars, a dark corridor stub
  and two swept-tube pool ladders down to the floor.
- **Walking, swimming, climbing player**: Jolt `CharacterVirtual` through the
  ECS `CharacterBodyComponent` — walk the deck, jump in, swim (gravity gives
  way to gentle buoyancy below the waterline), and climb either ladder back
  out (hold W against it).
- **Skylight lighting**: the sun + procedural atmosphere enter through the
  ceiling wells (PSSM-shadowed), the sky IBL fills the room, god rays and a
  bounded humid fog bank sell the air.

## Running

```bash
cmake --build --preset dev --target poolrooms -j4
./build/Poolrooms/poolrooms            # Metal on macOS
./build/Poolrooms/poolrooms --vulkan   # Vulkan
```

Controls: **WASD** move, **mouse** look (**Tab** toggles capture), **Space**
jump / swim up, **LCtrl** swim down, **LShift** sprint, **Esc** quit.

The example window has teleports and a sun-angle control; deep water tuning
(waves, SSR, caustics, foam) lives in the engine debug panel under
**Water Settings**.

## Using your own tile textures

The scene renders with procedural placeholder textures. To swap in real
ones, drop files into `Res/textures/poolrooms/` next to the built binary
(source: `Examples/Poolrooms/assets/textures/poolrooms/`) — see the README
in that folder for slot names and authoring notes. Geometry UVs map one tile
face per texture, so a single photographed tile works as-is; grout lines and
tile bevels are real geometry and stay crisp at any angle.

## How the water passes fit the frame

```
... -> MainRenderPass -> SkyAtmosphere
    -> WaterSim        (compute: spectrum evolve -> inverse FFT -> displacement
                        SSBO/texture + normal/foam map)
    -> WaterReflection (mirrored camera -> half-res HDR RT, waterline-clipped,
                        cull-flipped, simplified irradiance+sun shading)
    -> WaterCaustics   (fullscreen: submerged pixels x caustic web, colorRT swap)
    -> Water           (snapshot colorRT -> tempColorRT, then draw the surface
                        into colorRT: FFT displacement, fresnel blend of planar
                        reflection vs refraction, sun specular, Jacobian foam)
    -> HeightFog -> VolumetricFog -> ... -> Bloom -> PostProcess
```

The passes are driven by `WaterData` + `WaterSimParams`
(`graphics_effects.hpp`) and no-op until an app configures them through the
`IRenderer` water API (`setWaterGrid` / `setWaterTransform` /
`setWaterSettings` / `setWaterSimParams` / `setWaterEnabled` /
`setWaterTextures`). The GLSL and MSL shader twins declare the same struct
layouts field-for-field; `static_assert`s in `graphics_effects.hpp` pin the
offsets, and the FFT butterfly is numerically verified against a brute-force
DFT in the development notes.
