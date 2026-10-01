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

Rejection backends (`--reject`):

- `auto` (default): the GPU when it supports everything the ray-tracing path needs (shader model
  6.6 bindless descriptor heaps, ray-tracing pipelines, acceleration structures), otherwise the CPU
  BVH. The log names whatever is missing.
- `gpu`, `cpu`, `none`: force one. `--gpu-api d3d12|vulkan` picks the API (D3D12 is the Windows
  default, Vulkan everywhere else; Vulkan needs `VK_EXT_descriptor_heap`).

Both backends produce the same caches (to a few float-precision edge cases per million blades).

Licensed under the GNU General Public License v3 (see `LICENSE`).
