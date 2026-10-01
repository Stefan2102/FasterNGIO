# FasterNGIO

Generates No Grass In Objects (NGIO)-compatible grass caches (`Data/Grass/*.cgid`) for Skyrim Special Edition offline, without launching the game.
Grass placement reproduces the engine's; blades inside objects are found by ray tracing the world's
Havok collision on the GPU (DXR).

```
FasterNGIO --data "<Skyrim>\Data" --out "<mod>\Grass" [--world 0x3C] [--plugins plugins.txt]
```

Run it under Mod Organizer 2 (so the virtual Data folder is visible) and use `--help` for the
placement and NGIO `[RayCastConfig]` options. Requires a DXR-capable GPU; `--reject cpu` runs the
same tests on the CPU.

Licensed under the GNU General Public License v3 (see `LICENSE`).
