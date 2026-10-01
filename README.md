# FasterNGIO

Generates No Grass In Objects (NGIO)-compatible grass caches (`Data/Grass/*.cgid`) for Skyrim Special Edition offline, without launching the game.
Grass placement reproduces the engine's; blades inside objects are found by ray tracing the world's
Havok collision on the GPU (D3D12/DXR on Windows, Vulkan on Windows and Linux), or with a CPU BVH
when the GPU cannot.

```
FasterNGIO --data "<Skyrim>\Data" --out "<mod>\Grass" [--world 0x3C] [--plugins plugins.txt]
```

Run it under Mod Organizer 2 (so the virtual Data folder is visible) and use `--help` for the
placement and NGIO `[RayCastConfig]` options. On Linux it runs natively (no Proton): Data paths are
resolved case-insensitively as the game does, and `--plugins` defaults to the Steam Proton prefix's
`plugins.txt`.

Grass settings come from the game's INIs, as the game would read them: `iMinGrassSize`,
`iMaxGrassTypesPerTexure` and `fTexturePctThreshold` from `[Grass]` in `Skyrim.ini` then
`SkyrimCustom.ini`. They're read from the MO2 profile when it uses profile-specific INIs, otherwise from
`My Games\Skyrim Special Edition` (which MO2's virtual filesystem also redirects). `--game-ini-dir`
picks the folder, `--no-game-ini` ignores the INIs, and the matching command-line options override
them.

Placement (`--placement`):

- `vanilla` (default): the engine's own algorithm. Its grass follows the terrain's 512-unit patch,
  quadrant and cell grids, which is where the blocky edges and straight seams come from.
- `smooth`: each blade's density comes from the texture weights at its own position, merged across
  quadrants and cells and interpolated, so edges follow the painted terrain instead of the grids.
  Each grass type is scaled so its total over the worldspace matches vanilla's (within a couple of
  percent). `--smooth-density-bias`, `--smooth-coverage` and `--smooth-warp` tune how blends look.

Rejection backends (`--reject`):

- `auto` (default): the GPU when it supports everything the ray-tracing path needs (shader model
  6.6 bindless descriptor heaps, ray-tracing pipelines, acceleration structures), otherwise the CPU
  BVH. The log names whatever is missing.
- `gpu`, `cpu`, `none`: force one. `--gpu-api d3d12|vulkan` picks the API (D3D12 is the Windows
  default, Vulkan everywhere else; Vulkan needs `VK_EXT_descriptor_heap`).

Both backends produce the same caches (to a few float-precision edge cases per million blades).

Licensed under the GNU General Public License v3 (see `LICENSE`).
