# Poolrooms

A liminal "pool core" walking demo — an endless-afternoon tiled pool hall in
the backrooms style — and the showcase scene for the engine's RHI water pass.

## What it demonstrates

- **RHI water surface** (`Renderer::waterPass`, new in this example's PR):
  Gerstner ripples, dual scrolling normal maps, screen-space reflections with
  an IBL-cubemap fallback, refraction from a scene snapshot with depth-tinted
  absorption, GGX sun specular, and edge softening where the water meets tile.
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

## How the water pass fits the frame

```
... -> MainRenderPass -> SkyAtmosphere
    -> WaterCaustics   (fullscreen: submerged pixels x caustic web, colorRT swap)
    -> Water           (snapshot colorRT -> tempColorRT, then draw the surface
                        into colorRT: SSR + IBL reflections, refraction from the
                        snapshot, sun specular, alpha = shore softening)
    -> HeightFog -> VolumetricFog -> ... -> Bloom -> PostProcess
```

Both passes are driven by `WaterData` (`graphics_effects.hpp`) and no-op until
an app configures them through the `IRenderer` water API
(`setWaterGrid` / `setWaterTransform` / `setWaterSettings` /
`setWaterEnabled` / `setWaterTextures`). The GLSL and MSL shader twins
(`Water.vert/.frag`, `WaterCaustics.frag`, `3d_water_rhi.metal`,
`3d_water_caustics_rhi.metal`) declare the same struct layout field-for-field;
`static_assert`s in `graphics_effects.hpp` pin the offsets.
