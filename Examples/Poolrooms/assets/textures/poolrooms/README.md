# Poolrooms — user tile textures

Drop PNG/JPG files here to replace the procedural placeholder materials.
Every file is optional; any slot without a file keeps its generated texture.

Naming: `<slot>_<kind>.png` where

- `<slot>` is one of
  `deck_tile`, `wall_tile`, `pool_wall_tile`, `pool_floor_tile`,
  `accent_tile`, `grout`, `coping`, `ceiling_plaster`, `dark_plaster`,
  `metal`, `lamp_glow`, `slide_blue`, `slide_violet`
- `<kind>` is one of `albedo`, `normal`, `roughness`

Examples: `deck_tile_albedo.png`, `wall_tile_normal.png`, `accent_tile_roughness.png`.

Authoring notes:

- **Tile slots** (`*_tile`): the UV cell `[0,1]²` is ONE tile face — author a
  single tile (the glaze, its edge, dirt in the corners), not a grid of
  tiles. The panel geometry repeats it per tile and the grout/bevel geometry
  provides the joints, so keep the texture borderless: no painted grout.
- `grout` / `coping` / `*_plaster` / `metal`: conventional tiling textures;
  UVs are in tile-size units (~20 cm per repeat on grout, ~30 cm on coping).
- `normal` maps are tangent-space, +Z out (OpenGL convention, MikkTSpace).
- `roughness` is read as grayscale.
- Any resolution works (power-of-two recommended); files are loaded with
  stb_image, so PNG/JPG/TGA are all fine.
