# Implementation Plan — Opacity Micromaps (OMM)

> **Status**: plan, not started.
> **Companion analysis**: [OMM_Integration_Analysis.md](OMM_Integration_Analysis.md) (facts, measurements
> and references; this document assumes it).
> **Success criteria**: with OMM enabled on Bistro, (a) `--rhidebug` reports no D3D12 messages, (b) masked
> geometry renders identically to today within a documented tolerance, (c) ray-traced candidate counts drop
> measurably, (d) a warm start never re-bakes OMM data, (e) the three debug views answer "is the mask
> right?" without a debugger.

---

## 1. Design Decisions (locked before coding)

| # | Decision | Why |
|---|---|---|
| D1 | Bake with **meshoptimizer 1.3** (`meshopt_opacityMap*`), not the NVIDIA OMM SDK | Already vendored (`external/meshoptimizer`, `MESHOPTIMIZER_VERSION 1030`), same output format, used by the reference renderer |
| D2 | **4-state** masks, `max_level = 6`, `target_edge = 3.0f`, mip 0 by default | 4-state keeps the "unknown" band explicit, which is what the debug views need; matches niagara's defaults (`kOmmSubdivisionLevel`) |
| D3 | OMM applies to **`ALPHA_MODE_MASK` primitives only** | BLEND is stochastic by design; the baker has no single cutoff (`SceneLoader.cpp:1208-1226`) |
| D4 | Masks are **per primitive, per LOD** | BLAS is per primitive per LOD (`Scene.cpp:78-125`); OMM linkage is part of the BLAS |
| D5 | **LOD0 first**, other LODs behind a per-scene list | LOD0 is what the path tracer uses (`PathTracer.hlsl:102`) and what nearby instances pick |
| D6 | Baked bytes go to a **separate `_omm.bin`** next to `_mesh.bin` | Independent versioning/invalidation from the mesh cache (§7 of the analysis) |
| D7 | Feature is **capability-gated and runtime-toggleable** | `Feature::RayTracingOpacityMicromap` + an ImGui switch, so machines without DXR 1.2 behave exactly as today |
| D8 | 16-bit OMM indices when the mask count allows (`[0..65531]`) | 4 bytes/triangle instead of 12 (`meshoptimizer/README.md:909`) |

Invariants to assert in code:

- `ommIndices.size() == indexCount / 3` for every LOD that has masks.
- Every mask `subdivisionLevel <= max_level` and every offset is within `ommData`.
- A 16-bit index buffer is only used when `entryCount <= 65532`.
- `counts` (histogram) sums to the number of mask entries.

---

## 2. New Data Structures

New directory `src/Omm/` (mirrors how `src/Streaming/` groups the streaming system):

```cpp
// src/Omm/OmmTypes.h
struct OmmLodData                       // one per (primitive, LOD)
{
    std::vector<nvrhi::rt::OpacityMicromapUsageCount> m_Counts;  // histogram, needed by the GPU build
    std::vector<uint32_t> m_PerOmmDescs; // packed: (offset << 4) | subdivisionLevel   (niagara scene.cpp:1021)
    std::vector<int32_t>  m_Indices;     // one per triangle: mask index or special -4..-1
    std::vector<uint8_t>  m_Data;        // compacted micro-triangle states
    uint32_t m_MaxMaskIndex = 0;

    // GPU resources (rebuilt every session, never cached)
    nvrhi::BufferHandle              m_DataBuffer;
    nvrhi::BufferHandle              m_DescBuffer;
    nvrhi::BufferHandle              m_IndexBuffer;
    nvrhi::rt::OpacityMicromapHandle m_Array;
};

struct OmmDebugTexelStats               // for the texel classification view (Phase 5)
{
    std::vector<uint8_t> m_Class;       // per texel of the bake mip: 0 opaque, 1 transparent, 2 mixed
    uint32_t m_Width = 0, m_Height = 0;
};
```

`Scene::Primitive` gains `std::vector<OmmLodData> m_Omm;` (empty when the primitive has no masks).
Note `src/Scene.h` and `src/SceneLoader.cpp` are **UTF-16LE** — edits must preserve that encoding.

---

## 3. Phases

### Phase 0 — Capability spike (half a day)

Goal: prove the toolchain can do ray-query OMM at SM 6.8 *before* writing pipeline code.

**T0.1 — Enable the nvrhi path.**
- `CMakeLists.txt`, next to the existing nvrhi options at line 158-160:
  `set(NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP ON CACHE BOOL "" FORCE)`.
- Verify: `build/CMakeCache.txt` shows `NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP:BOOL=ON`; project builds;
  `external/nvrhi/src/d3d12/d3d12-raytracing.cpp` OMM bodies are no longer preprocessed away.

**T0.2 — Runtime gate.**
- In `D3D12RHI` (or `Renderer::Initialize`, next to `src/Renderer.cpp:369-372`), log
  `queryFeatureSupport(nvrhi::Feature::RayTracingOpacityMicromap)` and store it on `Renderer`
  (e.g. `bool m_bOmmSupported`).
- Verify: log shows `true` on this machine; nothing else changes.

**T0.3 — Shader spike.**
- Add a throw-away entry point that traces with `RayQuery` in an OMM-attached scene, using
  `RAY_FLAG_FORCE_OMM_2_STATE` (the HLSL flag corresponding to `InstanceFlags::ForceOMM2State`).
- Verify: DXC compiles it at `-m 6_8` with `--hlsl2021` (the exact flags in `build_shaders`,
  `CMakeLists.txt:373-410`). **If this fails, stop and report** — ray-query OMM would need a newer shader
  model, i.e. the Agility SDK, and that decision is the user's.

**Exit criteria**: flag on, feature `true`, OMM ray query compiles. No visual change.

---

### Phase 1 — Bake (CPU), no GPU OMM yet

Goal: turn (RT indices, UVs, alpha texture) into `OmmLodData` for one primitive, with statistics logged.

**T1.1 — Alpha source decode.**
- New `src/Omm/AlphaTextureReader.{h,cpp}`: given `Scene::StreamingTexture` (or the texture path),
  a mip level, and the `nvrhi::TextureDesc`, produce an RGBA8 buffer.
- Reuse `ComputeDDSMipOffsets` (`src/TextureLoader.h`, `SceneLoader.cpp:1359`) for the mip slice and
  `st.m_SourceData` (mmap) for the bytes.
- BC decode: BC1 (1-bit alpha), BC2/BC3 (explicit alpha), BC4/BC5 (no alpha → treat as fully opaque),
  BC6H (no alpha), BC7 (explicit alpha). A compact decoder is enough; no file I/O.
- Verify: unit-style log of a known texture — alpha min/max/mean per format, and that a 1×1 fully
  transparent region reports 0.

**T1.2 — `OmmBaker::BakeLod()`.**
- New `src/Omm/OmmBaker.{h,cpp}` implementing the niagara sequence against our data:
  ```cpp
  bool BakeLod(const OmmBakeInput& in, OmmBakeParams params, OmmLodData& out);
  // in: indices (RT slice), uv per vertex (from VertexQuantized.m_Uv, meshopt_dequantizeHalf),
  //     alpha RGBA8 + width/height, alphaCutoff
  ```
  Steps: `meshopt_opacityMapMeasure` → `meshopt_opacityMapEntrySize` → `meshopt_opacityMapRasterize`
  → `meshopt_opacityMapCompact` (see analysis §6), then build `m_Counts`, `m_PerOmmDescs`, `m_Indices`.
- Corner-order normalisation (rotate so the smallest index is first) happens in the RT index buffer build
  path, **before** the cache is written, so both raster/RT and the bake agree. Do it in
  `SceneLoader.cpp` where `m_RTIndexBuffer` is produced (around `:2341-2398`).
- Verify: for one Bistro mask primitive, log entry count, average level, %transparent/opaque/mixed,
  data bytes, and assert `m_Indices.size() == indexCount/3`; run twice and confirm byte-identical output
  (determinism, prerequisite for caching).

**T1.3 — Texel classification for the debug views.**
- In the same pass, classify every texel of the bake mip: `alpha >= cutoff` → opaque; `< cutoff` →
  transparent; a 2×2 footprint straddling the cutoff → mixed. Store as `OmmDebugTexelStats`.
- Verify: counts sum to `width*height`; log the three percentages for a foliage texture (expect a large
  "mixed" fraction at subdiv-6 — if it is tiny, the classification is broken).

**Exit criteria**: bake works for every MASK primitive of Bistro, produces deterministic bytes, and logs
plausible statistics. No GPU work yet, nothing visible on screen.

---

### Phase 2 — GPU masks + BLAS attachment

**T2.1 — Upload and build.**
- New `src/Omm/OmmRuntime.{h,cpp}`: for each `OmmLodData`, create the three buffers
  (`canHaveUAVs` for data/descs, `isAccelStructBuildInput` for the index buffer — see `SceneCache.cpp:168-181`
  for the pattern), fill `nvrhi::rt::OpacityMicromapDesc` (flags `FastTrace`, counts, input + descs),
  then `createOpacityMicromap` + `buildOpacityMicromap` on the scene-load command list.
- Ordering: OMM builds must be recorded **before** the BLAS builds in
  `Scene::BuildAccelerationStructures` (`src/Scene.cpp:78-125`), and the OMM data buffer needs a
  read-after-write dependency before the BLAS build; if `--rhidebug` complains, insert an explicit
  transition/`setBufferState` — nvrhi's `buildOpacityMicromap` only barriers its *own* writes
  (`d3d12-raytracing.cpp:2194-2210`).
- Verify: `--rhidebug` clean; log OMM array size per primitive (`getAccelStructMemoryRequirements`-style
  accounting already exists for BLAS at `Scene.cpp:125`).

**T2.2 — Attach to the BLAS geometry.**
- In the geometry descriptor (`src/Scene.cpp:95-113`), when masks exist for that LOD:
  `geometryDesc.geometryData.triangles.setOpacityMicromap(ommArray, indexBuffer, indexFormat,
  pOmmUsageCounts, numOmmUsageCounts)` (fields at `nvrhi.h:1596-1617`; nvrhi fills the linkage itself in
  `d3d12-raytracing.cpp:648-656`).
- Keep `geometryDesc.flags = GeometryFlags::None` (`Scene.cpp:113`) — masked geometry must stay non-opaque.
- Verify: BLAS build succeeds, `--rhidebug` clean, and instance rendering is unchanged when the OMM toggle
  is off (masks attached but traversal forced opaque is a *different* test, see T3.2).

**T2.3 — A/B switch plumbing.**
- `Renderer` gets `bool m_bEnableOmm = true;` shown in ImGui; a change sets a "rebuild BLAS" request
  (BLAS handles live per primitive, `Scene.h:35-36`, and are only built at load, `Scene.cpp:57`).
- Simplest correct v1: rebuild the scene's BLASes on toggle (a load-time cost, acceptable for a debug
  switch). Cheaper alternative: nvrhi patch so `InstanceFlags::DisableOMMs` works — see T3.2.
- Verify: toggling produces two visually similar images and two different candidate counts (T3.3).

**Exit criteria**: masks exist on the GPU, attached to the BLAS, validation clean, toggle works.

---

### Phase 3 — Traversal correctness

**T3.1 — Confirm the alpha test still runs for the right candidates.**
- With OMM attached, hardware skips fully-transparent micro-triangles and auto-commits fully-opaque ones;
  "unknown" arrives as a candidate. Our existing tests in `RaytracingCommon.hlsli:196-213`,
  `CommonLighting.hlsli:405-470` and `RAB_RTShaders.hlsli:17-50` must therefore stay — they are the
  fine-grained filter, not redundant work.
- Verify: screenshot diff (OMM on/off) on masked geometry: no holes in opaque areas, no brightening of
  foliage edges. Use the existing two-shot harness for a before/after pair.

**T3.2 — Per-instance OMM disable (optional but recommended).**
- nvrhi exposes `InstanceFlags::DisableOMMs` (`nvrhi.h:1747`) but never sets
  `D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS` on the BLAS, so the flag alone
  does nothing. Small nvrhi patch, wrapped in the fork's markers:
  ```cpp
  // [rlaw] BEGIN: allow per-instance OMM disabling
  // [rlaw] END
  ```
  adding the build flag when the geometry carries an OMM.
- Verify: toggling `DisableOMMs` per instance changes the candidate counters without rebuilding anything.

**T3.3 — Candidate counters (also used by the debug view).**
- Add a per-pixel `uint` counter incremented on every shader-side alpha test, in `TraceRayStandard`,
  `CalculateRTShadow` and the RTXDI bridge; clear once per frame, present via the debug pass (Phase 5).
- Verify: with OMM on, the count for masked geometry drops significantly versus OMM off; log the ratio.

**Exit criteria**: images match within tolerance, counters show a real reduction, validation clean.

---

### Phase 4 — `_omm.bin` scene cache

**T4.1 — Format and key.**
- New `src/Omm/OmmCache.{h,cpp}` mirroring `SceneCache` (`WritePOD`/`WriteVector` helpers,
  `SceneCache.h:48-68`), with its own magic and `kOmmCacheVersion = 1`.
- Header: `magic, version, kCookedMeshVersion, glTF mtime, params{maxLevel, targetEdge, states, mip}`,
  then a texture stamp list `{pathHash, size, mtime}` for every alpha texture referenced by a MASK
  primitive, then per-primitive/per-LOD counts and blobs (`counts`, `descs`, `data`, `indices`).
- Path: `<scene stem>_omm.bin` beside `_mesh.bin` (`SceneCache.cpp:155` pattern).
- Verify: file appears after a cold load; sizes match the logged totals.

**T4.2 — Validation.**
- `IsValid()` returns false when: file missing; magic/version mismatch; `kCookedMeshVersion` mismatch;
  glTF mtime newer; any recorded param differs; any texture stamp differs (path, size, mtime).
- Verify: touch the glTF → re-bake; touch one alpha texture → re-bake; change `maxLevel` → re-bake;
  change nothing → no bake (assert via a log line and a wall-clock measurement).

**T4.3 — Load path.**
- Hook into `Scene::LoadScene` (`src/Scene.cpp:44-57`): after `LoadOrCookMeshData` and
  `UpdateMaterialsAndCreateConstants` (materials are needed for the cutoff and texture indices), load
  `_omm.bin`; on miss, bake then save.
- Verify: warm-start log prints "OMM cache hit, N primitives"; measure and record cold vs warm load time in
  the plan's results table.

**Exit criteria**: warm start performs zero bakes; invalidation works for all three inputs; timings
recorded.

---

### Phase 5 — ImGui debug views

The three views share one new ImGui panel and one new debug pass. The pass follows
`TileResidencyDebugRenderer` (`src/Streaming/TileResidencyDebugRenderer.cpp`), which is the established
pattern: own MS/PS, `DepthDisabled`, `dispatchMesh`, `REGISTER_RENDERER`, scheduled before `ImGuiRenderer`
(`src/Renderer.cpp:1168-1170`).

**T5.1 — Panel + settings.**
- `Renderer` members: `int m_OmmDebugMode = 0` (0 off, 1 texel classification, 2 micromap grid,
  3 candidate heatmap), `int m_OmmDebugPrimitive = -1`, `int m_OmmDebugLod = 0`,
  `int m_OmmDebugTriangle = 0`, `bool m_bOmmShowOnlyMixed`.
- ImGui (`src/ImGuiLayer.cpp`, Property Grid block at `:78-130`): a "OMM" tree with the toggle from T2.3,
  the debug view combo, primitive/LOD/triangle pickers, and a statistics block (mask entries, data bytes,
  %opaque/transparent/mixed, candidate counts with/without OMM).
- Verify: controls appear, nothing crashes, values round-trip into the pass.

**T5.2 — Texel classification view (the requested one).**
- GPU texture built from `OmmDebugTexelStats` (R8_UINT: 0 = opaque, 1 = transparent, 2 = mixed) for the
  selected primitive's bake mip; uploaded lazily the first time the view is enabled.
- New `src/shaders/OmmDebug.hlsl` + `OmmDebug.sr`, plus `shaders.cfg` entries (pattern:
  `src/shaders/shaders.cfg:24-25` for `TileResidencyDebug`) and a `DEBUG_MODE_*`-style constant; the pass
  draws a full-screen quad with zoom/pan (uv scale/offset in the CB) and maps
  0→green, 1→red, 2→yellow; `m_bOmmShowOnlyMixed` renders mixed only, rest black.
- Verify: the view shows the expected silhouettes (foliage leaves with yellow fringes); toggling
  `maxLevel`/mip changes the ratio consistently with the logged statistics.

**T5.3 — Micromap grid view.**
- ImGui window drawing the selected triangle's `4^N` grid with `ImDrawList` (filled triangles coloured by
  state: opaque / transparent / unknown), reading the CPU-side `m_Data` for that mask.
- Verify: a fully transparent triangle shows all-transparent cells; a leaf edge shows the unknown band;
  the grid matches the texture under the same UVs.

**T5.4 — Candidate heatmap.**
- Debug pass samples the per-pixel counter buffer (T3.3), log-scale colour; the panel shows totals and the
  OMM on/off ratio.
- Verify: masked foliage is warm without OMM and cool with it; sky/opaque geometry stays black.

**Exit criteria**: all three views usable, no validation messages, panel statistics match the bake log.

---

### Phase 6 — Validation, performance, documentation

**T6.1 — Validation sweep.**
- `--rhidebug` with breaks on corruption/error/warning on Bistro and Sponza: zero messages.
- Verify: `bin/output.txt` has no `[E]`/`[W]`; app exits cleanly.

**T6.2 — Performance.**
- Record: GPU frame time with/without OMM, ray-traced candidate counts, BLAS + OMM build time, memories
  (mask data, index buffers, cache file), cold vs warm load time.
- Verify: a results table in this document (append below, do not rewrite the plan).

**T6.3 — Documentation.**
- Update `docs/OMM_Integration_Analysis.md` if measurements contradict a claim, and note the final
  defaults (`states`, `max_level`, `target_edge`, mip, index width) with the numbers that justified them.

**Exit criteria**: all numbers recorded, docs consistent, `--rhidebug` clean.

---

## 4. Test Matrix

| Case | Expected |
|---|---|
| OMM unsupported (force `m_bOmmSupported = false`) | identical rendering to today, no masks built, no cache writes |
| MASK primitive, mask all-opaque (special index) | committed as opaque; no candidate counters; raster and RT agree |
| MASK primitive, mask all-transparent | nothing hit; no candidate counters |
| Mixed edge (foliage) | unknown band produces candidates; alpha test matches raster within tolerance |
| BLEND primitive | unchanged path, no masks baked |
| Toggle OMM mid-session | BLAS rebuild path works (or per-instance `DisableOMMs`), no crash, counters change |
| Texture edited (mtime) | `_omm.bin` invalidated, re-bake, log says so |
| `maxLevel` changed | `_omm.bin` invalidated, re-bake |
| Cache file corrupted (truncate) | detected, re-bake, no crash |
| LOD > 0 instances | either masks present and correct, or explicitly no OMM for that LOD (documented) |

---

## 5. Open Questions (resolve during Phase 0–1)

1. **Does the transparent-alpha threshold have to match the raster path exactly?** The raster test is
   `albedoSample.w * material.m_BaseColor.w < m_AlphaCutoff` (`BasePass.hlsl:283-289`); the baker sees raw
   alpha only. If a material multiplies base colour alpha, either bake against the multiplied value or
   accept the (small) mismatch. Decide with a screenshot diff.
2. **sRGB vs linear alpha.** DDS BC1/BC3/BC7 sRGB formats store alpha linearly, so the raw byte is usable;
   verify per format in T1.1.
3. **Which LODs beyond LOD0** actually appear in RT at Bistro's camera positions (measure with the
   counter view), so the plan does not bake masks nothing uses.
4. **16-bit indices** — confirm the `[0..65531]` bound holds for Bistro; otherwise fall back to 32-bit for
   that primitive (per-primitive index width).

---

## 6. Out of Scope

- OMM for BLEND materials (stochastic transparency) and for ray-traced transmission volumetrics.
- OMM updates (`ALLOW_OPACITY_MICROMAP_UPDATE`) for animated/deforming geometry — BLASes here are static
  (`Scene.cpp:57`), so linkage never changes at runtime.
- Anything requiring the Agility SDK: the measured runtime already reports DXR 1.2 (§3 of the analysis).
