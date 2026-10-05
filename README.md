# FasterNGIO

Generates No Grass In Objects (NGIO)-compatible grass caches (`Data/Grass/*.cgid`) for Skyrim Special Edition offline, without launching the game.
Grass placement reproduces the engine's; blades inside objects are found by ray tracing the world's
Havok collision on the GPU (D3D12/DXR on Windows, Vulkan on Windows and Linux), or with a CPU BVH
when the GPU cannot.

```
FasterNGIO --data "<Skyrim>\Data" --out "<mod>\Grass" [--world 0x3C] [--plugins plugins.txt]
```

With Mod Organizer 2, add `FasterNGIO.exe` as an executable and run it from MO2, so the virtual Data
folder is visible. It finds MO2's instance and profile itself, as xEdit does. With no arguments it opens
a window with the game, load order and INIs already filled in. On the command line, `--data`,
`--plugins` and `--out` become optional: they default to the instance's game, the profile's
`plugins.txt` and `Data\Grass`. As with NGIO's own pregeneration, new files then land in MO2's
Overwrite folder, or in the mod chosen in the executable's "Create files in mod" setting. Use
`--help` for the placement and NGIO `[RayCastConfig]` options. `--world all` generates every
worldspace with terrain, as NGIO's own pregeneration does. Without Mod Organizer 2, run it without
arguments to get the same window (see below). On Linux it runs natively (no Proton): Data paths are
resolved case-insensitively as the game does, and `--plugins` defaults to the Steam Proton prefix's
`plugins.txt`. The Linux build needs only glibc and, for GPU rejection, a Vulkan driver with ray tracing and
`VK_EXT_descriptor_heap`; its shaders are compiled at build time.

Grass settings come from the game's INIs, as the game would read them: `iMinGrassSize`,
`iMaxGrassTypesPerTexure` and `fTexturePctThreshold` from `[Grass]` in `Skyrim.ini` then
`SkyrimCustom.ini`. They're read from the MO2 profile when it uses profile-specific INIs, otherwise from
`My Games\Skyrim Special Edition` (which MO2's virtual filesystem also redirects). `--game-ini-dir`
picks the folder, `--no-game-ini` ignores the INIs, and the matching command-line options override
them.

Placement (`--placement`):

- `vanilla`: the engine's own algorithm. Its grass follows the terrain's 512-unit patch,
  quadrant and cell grids, which is where the blocky edges and straight seams come from.
- `smooth` (default): each blade's density comes from the texture weights at its own position, merged across
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

## Without Mod Organizer 2

Run `FasterNGIO` without arguments (double-click it) and a window opens instead:

1. Choose the Skyrim Special Edition folder, the one with `SkyrimSE.exe`. It can be anywhere,
   including a copy outside Steam. The load order is read from the game's own `plugins.txt` (Steam,
   GOG, Epic and Microsoft Store builds each keep theirs in a different folder). Under Advanced you
   can choose another `plugins.txt` or game INI folder.
2. Leave the output empty to write to `Data\Grass`, where NGIO and DynDOLOD read the cache, or choose
   another folder (a mod manager's mod folder, for example).
3. Click Generate. By default every worldspace is generated.

This is how Vortex users (whose mods are deployed into the real Data folder) and manual installs run
it. The window remembers its settings in `%LOCALAPPDATA%\FasterNGIO` (`~/.config/fasterngio` on
Linux, where the folder chooser uses zenity or kdialog). Any command-line argument runs the command
line; `--gui` opens the window too. Compiled shaders are
cached in the same folder (`~/.cache/fasterngio` on Linux).

Licensed under the GNU General Public License v3 (see `LICENSE`).
