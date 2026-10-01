# AGENTS.md

FasterNGIO generates Skyrim SE grass caches in NGIO's `.cgid` format, with NGIO-style "no grass in
objects" rejection ray traced on the GPU (D3D12 or Vulkan) or, as a fallback, against a CPU BVH. It is
a standalone, GPL-3 command-line tool for Windows and Linux: it must not depend on SARP, BasicRenderer
or CommonLibSSE.

## Layout

| Path | Contents |
|---|---|
| `src/GameData/` | Plugin (ESM/ESP/ESL) parsing, load order, static world snapshot. Copied from SARP; adds OBND and FLOR/SCOL/TACT. |
| `src/Grass/` | Grass placement split into `GenerateCellCandidates` and `FinalizeCell`: vanilla (engine RNG emulation) and smooth (`SmoothWeightField`); the game's `[Grass]` INI settings (`GameIni`); the `.cgid` writer. |
| `src/Archives/` | Memory-mapped BSA (v103/104/105) reader and load-order-aware resolver (loose files win). |
| `src/Platform/` | `DataDirectory`: case-insensitive, either-separator resolution of Data paths (an index off Windows); `IniFile`. |
| `src/Collision/` | nifly-based Havok collision extraction (CMS, packed strips, convex hulls, boxes, spheres, capsules) into model space. |
| `src/Rejection/` | NGIO query shapes, the per-world instance index, the CPU BVH fallback (`CpuBvh`) and the brute-force reference. |
| `src/Gpu/` | `GpuRejector`: feature check, the render thread (OpenRenderGraph `PersistentGraphHost`), BLAS/TLAS, ray-tracing pipeline, one DispatchRays per frame, readback. |
| `src/Pipeline/` | Lock-free cell pipeline on ORGModuleServices' AsyncStateGraph; TBB graph scheduler; MPSC queue. |
| `shaders/` | `GrassRejection.hlsl` (DXIL for D3D12, SPIR-V for Vulkan) and `Shared/GrassQueryMath.hlsli`, which the CPU paths also compile (`src/Rejection/HlslShim.h`). |
| `tools/` | `analyze_placement_edges.py`: grid-locking and corner statistics of `--export-blades` output, with a grid-free control. |
| `external/` | Submodules: BasicRHI, OpenRenderGraph, ORGModuleServices, BasicTelemetry, volk (pinned to BasicRenderer's commits) and upstream nifly. |

## Build and test

```powershell
cmake --preset vs2026          # GPU build (D3D12 + Vulkan); vs2026-cpu builds without the GPU stack
cmake --build --preset vs2026
ctest --preset vs2026
```

```sh
cmake --preset linux -DVulkan_INCLUDE_DIR=<Vulkan-Headers>/include   # or linux-cpu
cmake --build --preset linux && ctest --preset linux
```

vcpkg (`VCPKG_ROOT`) provides zlib, lz4, TBB, spdlog, fmt, gtest, Tracy and, for the GPU build,
directx-headers, directx-dxc, flecs, boost-container-hash, nlohmann-json, sqlite3 and (off Windows)
directxmath. Vulkan headers must include `VK_EXT_descriptor_heap` (Vulkan SDK 1.4.357 or the matching
Vulkan-Headers tag); distribution headers are usually too old. Off Windows, DirectX-Headers'
`wsl/winadapter.h` supplies the Win32 scalar types the shared libraries use. Shaders and the DXC
runtime (`dxcompiler.dll`/`dxil.dll`, `libdxcompiler.so`) are deployed next to the exe by the
`FasterNGIOShaders` target and compiled (with a disk cache) at startup. `vs2026-clangcl` builds with
`-Werror`.

## Load-bearing rules

- **Parity.** With `--placement vanilla --reject none`, output reproduces the engine and must not
  drift. It matches SARP's `SARPGrassCacheGenerator --paint vanilla` byte for byte except where a land
  texture carries more grass types than `iMaxGrassTypesPerTexure`: the engine tests `count > max`
  before taking each one (so the default of 2 takes 3; confirmed in the disassembly), SARP stops at
  `max`. Vanilla changes must preserve the engine's RNG draw order. Linux output (both placements)
  must stay byte-identical to Windows output.
- **Game settings.** Only settings that change placement are read: `iMinGrassSize`,
  `iMaxGrassTypesPerTexure`, `fTexturePctThreshold`, all in the engine's Skyrim.ini collection, which
  loads `Skyrim.ini` then `SkyrimCustom.ini` (`SkyrimPrefs.ini` feeds only the prefs collection). The
  engine reads them through the Win32 profile API: first occurrence of a key wins, values parse as
  their leading number. Engine defaults, then INIs, then command-line values.
- **Smooth placement is seam-free and calibrated.** Weights merge every shared vertex (quadrants and
  cells), blades read them at their own position, and the per-type density scale is computed over the
  whole worldspace, never the selected cells, so a cell is identical whatever a run selects. Judge
  placement changes with `tools/analyze_placement_edges.py` against vanilla and the null control.
- **Rejection is a post-filter.** NGIO's own hook skips the colour/orientation/height RNG draws for a
  rejected blade; FasterNGIO deliberately keeps the vanilla layout and only drops blades.
- **One source of geometric truth.** Exact overlap tests live in `shaders/Shared/GrassQueryMath.hlsli`
  and are compiled by the GPU, the CPU BVH and the brute-force reference (`PrimitiveTests.h` wraps
  them per primitive). Change them there, never in one copy.
- **Fallback is decided up front.** `GpuRejector` checks every feature it needs before any work is
  posted and throws `GpuUnsupportedError` listing what is missing; `--reject auto` then uses the CPU
  BVH. Add any new GPU requirement to that check.
- **No locks on the hot paths.** Workers and the render thread communicate through the AsyncStateGraph
  (producers, GPU submission tokens, capacity suspensions) and `Pipeline::MpscQueue`. The CPU BVH,
  `SmoothWeightField` and `DataDirectory` are built up front and immutable afterwards. Do not add mutexes or condition variables to the
  cell pipeline, `GpuRejector` or the rejection structures.
- **DXR payload.** The hit result is written by the closest-hit and miss shaders. Do not reintroduce
  `RAY_FLAG_SKIP_CLOSEST_HIT_SHADER` and rely on the payload surviving a traversal that runs neither:
  on NVIDIA it does not.

## Validation

- `--validate-cpu` re-runs every cell through the brute-force CPU reference and reports
  disagreements: none for the CPU BVH, a handful of float-precision edge cases per million blades
  for the GPU.
- `--benchmark-rejection` places every selected cell in memory and times the CPU BVH (all threads and
  one) and the GPU on the same queries, without file I/O.
- `FASTERNGIO_DEBUG_CANDIDATE=<frame-global candidate index>` compiles shader debug capture in and logs
  every raygen/intersection invocation for that candidate. Use with `--cell` so the index is the cell's
  query index.
- `--export-blades <file>` writes every placed blade (x, y, grass form ID) for the selected cells;
  `tools/analyze_placement_edges.py` measures it.
- `--collision-survey` and `--dump-collision <model> <obj>` inspect collision extraction;
  `FASTERNGIO_SURVEY_DUMP=<file>` makes the survey write per-model primitive counts for diffing builds
  or platforms.
