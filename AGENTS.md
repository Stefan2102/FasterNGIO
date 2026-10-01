# AGENTS.md

FasterNGIO generates Skyrim SE grass caches in NGIO's `.cgid` format, with NGIO-style "no grass in
objects" rejection traced on the GPU. It is a standalone, GPL-3 command-line tool: it must not depend on
SARP, BasicRenderer or CommonLibSSE.

## Layout

| Path | Contents |
|---|---|
| `src/GameData/` | Plugin (ESM/ESP/ESL) parsing, load order, static world snapshot. Copied from SARP; adds OBND and FLOR/SCOL/TACT. |
| `src/Grass/` | Vanilla grass placement (engine RNG emulation) split into `GenerateCellCandidates` and `FinalizeCell`; the `.cgid` writer. |
| `src/Archives/` | Memory-mapped BSA (v103/104/105) reader and load-order-aware resolver (loose files win). |
| `src/Collision/` | nifly-based Havok collision extraction (CMS, packed strips, convex hulls, boxes, spheres, capsules) into model space. |
| `src/Rejection/` | NGIO query shapes, the per-world instance index, and the CPU reference rejection. |
| `src/Gpu/` | `GpuRejector`: the render thread (OpenRenderGraph `PersistentGraphHost`), BLAS/TLAS, DXR pipeline, indirect DispatchRays, readback. |
| `src/Pipeline/` | Lock-free cell pipeline on ORGModuleServices' AsyncStateGraph; TBB graph scheduler; MPSC queue. |
| `shaders/` | `GrassRejection.hlsl` and `Shared/GrassQueryMath.hlsli`, which the CPU reference also compiles (`src/Rejection/HlslShim.h`). |
| `external/` | Submodules: BasicRHI, OpenRenderGraph, ORGModuleServices, BasicTelemetry, volk (pinned to BasicRenderer's commits) and upstream nifly. |

## Build and test

```powershell
cmake --preset vs2026          # GPU build (D3D12 + DXR); vs2026-cpu builds without the GPU stack
cmake --build --preset vs2026
ctest --preset vs2026
```

vcpkg (`VCPKG_ROOT`) provides zlib, lz4, TBB, spdlog, fmt, gtest, Tracy and, for the GPU build,
directx-headers, directx-dxc, flecs, boost-container-hash, nlohmann-json and sqlite3. The Vulkan SDK
headers are needed even for the D3D12-only tool: OpenRenderGraph uses BasicRHI's Vulkan interop
unconditionally. Shaders and the DXC DLLs are deployed next to the exe by the `FasterNGIOShaders`
target and compiled (with a disk cache) at startup.

## Load-bearing rules

- **Parity.** With `--reject none`, output must stay byte-identical to SARP's
  `SARPGrassCacheGenerator --paint vanilla`. Placement changes must preserve the engine's RNG draw order.
- **Rejection is a post-filter.** NGIO's own hook skips the colour/orientation/height RNG draws for a
  rejected blade; FasterNGIO deliberately keeps the vanilla layout and only drops blades.
- **One source of geometric truth.** Exact overlap tests live in `shaders/Shared/GrassQueryMath.hlsli`
  and are compiled by both the GPU and the CPU reference. Change them there, never in one copy.
- **No locks on the hot paths.** Workers and the render thread communicate through the AsyncStateGraph
  (producers, GPU submission tokens, capacity suspensions) and `Pipeline::MpscQueue`. Do not add
  mutexes or condition variables to the cell pipeline or `GpuRejector`.
- **DXR payload.** The hit result is written by the closest-hit and miss shaders. Do not reintroduce
  `RAY_FLAG_SKIP_CLOSEST_HIT_SHADER` and rely on the payload surviving a traversal that runs neither:
  on NVIDIA it does not.

## Validation

- `--validate-cpu` runs the CPU reference for every GPU-traced cell and reports disagreements (expect a
  handful of float-precision edge cases per million blades, nothing else).
- `FASTERNGIO_DEBUG_CANDIDATE=<frame-global candidate index>` compiles shader debug capture in and logs
  every raygen/intersection invocation for that candidate. Use with `--cell` so the index is the cell's
  query index.
- `--collision-survey` and `--dump-collision <model> <obj>` inspect collision extraction.
