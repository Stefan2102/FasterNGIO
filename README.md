# FasterNGIO

Generates No Grass In Objects (NGIO)-compatible grass caches (`Data/Grass/*.cgid`) for Skyrim Special Edition offline, without launching the game.
Grass placement reproduces the engine's; blades inside objects are found by ray tracing the game world on the GPU (D3D12/DXR on Windows, Vulkan on Windows and Linux), or with a CPU BVH
when the GPU cannot.

# Quickstart:
**MO2**: Add the FasterNGIO executable to MO2's executables list, and launch it from there. Configure any settings you want (defaults are probably fine), and click generate.

**Vortex**: Run the FasterNGIO executable manually. Point it at your mod folder, configure any settings you want, and click generate.

**Linux users**: Download the native Linux version from Nexus (Or run the Windows version through Wine/Proton), and follow the instructions above.

-----

The generation should take less than a minute on vanilla, up to several minutes on large modlists with lots of worldspaces + seasons.

# Details:
```
FasterNGIO --data "<Skyrim>\Data" --out "<mod>\Grass" [--world 0x3C] [--plugins plugins.txt]
```

With Mod Organizer 2, add `FasterNGIO.exe` as an executable and run it from MO2, so the virtual Data
folder is visible. It finds MO2's instance and profile itself, as xEdit does. With no arguments it opens
a window with the game, load order and INIs already filled in. On the command line, `--data`,
`--plugins` and `--out` become optional: they default to the instance's game, the profile's
`plugins.txt` and `Data\Grass`. As with NGIO's own pregeneration, new files then land in MO2's
Overwrite folder, or in the mod chosen in the executable's "Create files in mod" setting. Use
`--help` for the placement and NGIO `[RayCastConfig]` options. `--world` takes one or more form IDs
(`--world 0x3C,0x16BB4` for Tamriel and Riften); `--world all` generates every worldspace with
terrain, as NGIO's own pregeneration does. Without Mod Organizer 2, run it without
arguments to get the same window (see below). On Linux it runs natively (no Proton): Data paths are
resolved case-insensitively as the game does, and `--plugins` defaults to the Steam Proton prefix's
`plugins.txt`. The Linux build needs only glibc and, for GPU rejection, a Vulkan driver with ray tracing and
`VK_EXT_descriptor_heap`; its shaders are compiled at build time.

Grass settings come from the game's INIs, as the game would read them: `iMinGrassSize`,
`iMaxGrassTypesPerTexure` and `fTexturePctThreshold` from `[Grass]` in `Skyrim.ini` then
`SkyrimCustom.ini`, then each enabled plugin's own INI in `Data` (`Data\<plugin>.ini`, in `plugins.txt`
order), later files overriding earlier ones. The game INIs are read from the MO2 profile when it uses
profile-specific INIs, otherwise from `My Games\Skyrim Special Edition` (which MO2's virtual filesystem
also redirects). `--game-ini-dir` picks the folder, `--no-game-ini` ignores all of these INIs, and the
matching command-line options override them.

A cell left with no grass gets NGIO's 4-byte empty cache file, because without any file the game
generates that cell's grass itself every time it loads, and would regrow grass that rejection removed.
When NGIO's `GrassControl.ini` has `Use-grass-cache` and `Only-load-from-cache` on (as
Extend-grass-distance requires), the game never does that, so such cells get no file at all.
`--write-empty-cells` and `--skip-empty-cells` choose either way. With `--overwrite`, a cell that
gets no file loses the cache an earlier run left for it. Under Mod Organizer 2 that file may belong to
another mod (a downloaded cache, say), and deleting it through MO2 would delete it from that mod, so
it gets the empty cache file instead, which goes to Overwrite and hides the other mod's.

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

Experimental: `--render-geometry` (in the window: "Use model geometry instead of collision") makes
objects that have collision reject grass by their visible mesh instead of their collision, which is
usually coarser. Hidden, skinned, decal and effect shapes are left out, and objects without
collision still keep their grass, as with NGIO. In Tamriel this rejects about 6% more blades. The GPU
builds roughly five times the acceleration-structure memory (about 130 MiB), and the run takes a few
seconds longer.

NGIO's own settings apply too. FasterNGIO reads `Data\SKSE\Plugins\GrassControl.ini` (under MO2,
the one its virtual filesystem shows) and the `*_NGIO.ini` files in `Data`, as NGIO does. Settings
that only affect the game at runtime are ignored. The rest:

- `[RayCastConfig]`:
  - **Shape:** ray-cast height, depth, width and width multiplier.
  - **Ray-cast-enabled:** `false` turns rejection off.
  - **Collision layers.**
  - **Ignore forms:** objects that never reject grass.
  - **Ignore grass forms:** grass types that are never rejected.
  - **Texture forms:** land textures (within the texture width) where grass is rejected.
- **Grass cliffs.** Grass under a cliff object (Grass-cliffs-forms, or `[CliffObjects]` in an
  `*_NGIO.ini`) moves onto the cliff's top, standing on its slope. This happens when the cliff
  passes NGIO's slope and neighbour checks, and `[CliffObjects]`' shape names and `Steep` flag
  apply. Unlike NGIO, grass that touches a cliff but fails those checks is rejected rather than
  left inside the rock.
- **`[IgnoredShapes]`:** hits on the named render shapes of an object don't reject grass.
- `[GrassConfig]`: global grass scale, super-dense mode, Overwrite-min-grass-size,
  Ensure-max-grass-types-setting, and the skip and only lists for pregenerated worldspaces (they
  apply to `--world all` and the window's "All worldspaces").

NGIO itself seems to not read `Ray-cast-mode`, so the
capsule query is kept whatever the file says; `--ray-mode` changes it here. Command-line options
override the file, and `--ngio-config <file|none>` picks another file or none.

## Seasons of Skyrim

With Seasons of Skyrim installed (`po3_SeasonsOfSkyrim.dll` in `Data\SKSE\Plugins`), each run also
writes every season's caches, `<cell>.WIN.cgid`, `.SPR.cgid`, `.SUM.cgid` and `.AUT.cgid`, which
Grass Cache Helper NG loads for the current season. There is nothing to rename and no need to
generate four times in-game. Each season applies Seasons' own swaps, read as Seasons reads them:

- **What is applied:**
  - each land texture's seasonal grass list;
  - for rejection, the seasonal replacement objects (statics, trees, activators, furniture, movable
    statics, flora).
- **Where the swaps come from:**
  - the automatic winter swaps Seasons generates (MainFormSwap_WIN.ini);
  - the `Data\Seasons\*_WIN|SPR|SUM|AUT.ini` files.
- **Settings honoured** from `po3_SeasonsOfSkyrim.ini`:
  - its worldspace lists;
  - its per-season `Grass` and object switches;
  - `Season Type = 0` means no seasonal caches;
  - a season whose `Grass` isn't `true` gets none, since Grass Cache Helper NG then loads the plain
    caches.
- **Seasons without differences** in a worldspace get copies of the plain cache, since Grass Cache
  Helper NG asks for the seasonal file everywhere.
- **Editor IDs:** editor-ID entries and Seasons' editor-ID rules need powerofthree's Tweaks, as they
  do in-game.

`--seasons on|off` overrides the detection (the window has the same choice), and
`--dump-season-swaps <file>` writes the resolved swaps for inspection. Expect about five times as
many files.

## Without Mod Organizer 2

Run `FasterNGIO` without arguments (double-click it) and a window opens instead:

1. Choose the Skyrim Special Edition folder, the one with `SkyrimSE.exe`. It can be anywhere,
   including a copy outside Steam. The load order is read from the game's own `plugins.txt` (Steam,
   GOG, Epic and Microsoft Store builds each keep theirs in a different folder). Under Advanced you
   can choose another `plugins.txt` or game INI folder.
2. Leave the output empty to write to `Data\Grass`, where NGIO and DynDOLOD read the cache, or choose
   another folder (a mod manager's mod folder, for example).
3. Click Generate. By default every worldspace is generated; the Worldspaces list can narrow that to
   any subset.

This is how Vortex users (whose mods are deployed into the real Data folder) and manual installs run
it. The window remembers its settings in `%LOCALAPPDATA%\FasterNGIO` (`~/.config/fasterngio` on
Linux, where the folder chooser uses zenity or kdialog). Any command-line argument runs the command
line; `--gui` opens the window too. Compiled shaders are
cached in the same folder (`~/.cache/fasterngio` on Linux).

Licensed under the GNU General Public License v3 (see `LICENSE`).
