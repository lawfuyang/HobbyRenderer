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
    std::vector<OmmPerOmmDesc> m_PerOmmDescs; // D3D12_RAYTRACING_OPACITY_MICROMAP_DESC: {byteOffset, level:16 | format:16}
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

**T0.3 — Shader spike.** — **done, outcome changed.** DXC does reject
`RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS` and `RAY_FLAG_FORCE_OMM_2_STATE` at `-m 6_8` (both are SM 6.9), but
the shader-side spelling is not the only opt-in: the DXR spec defines
`D3D12_PIPELINE_STATE_FLAG_RAYQUERY_ALLOW_OPACITY_MICROMAPS_ON_PRE_SM69_SHADERS` (0x80) for pipelines and
`..._STATE_OBJECT_FLAG_...` (0x800) for state objects. The engine's ray tracing is inline `RayQuery` in
compute passes, so the pipeline flag suffices and **no SM 6.9 / Agility SDK is needed**. nvrhi upstream set
no pipeline flags of any kind for compute/graphics, so the fork adds them (`[rlaw]` blocks in
`d3d12-backend.h` and `d3d12-compute.cpp`). The HLSL `RAY_FLAG_FORCE_OMM_2_STATE` is replaced by
`InstanceFlags::ForceOMM2State` (`D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OMM_2_STATE`), which has no shader
model requirement. See analysis §5.3.

**Exit criteria (met, with a correction)**: flag on, feature `true`, `--rhidebug` clean, no visual change.
The earlier note that the pre-SM6.9 pipeline flag is "accepted" was measured on a run in which no pipeline
set it: setting it makes `CreateComputePipelineState` return E_INVALIDARG on this machine, see Phase 3.

---

### Phase 3 — Traversal correctness — **unblocked via the Agility SDK and shader model 6.9**

The pre-SM6.9 pipeline flag (`D3D12_PIPELINE_STATE_FLAG_RAYQUERY_ALLOW_OPACITY_MICROMAPS_ON_PRE_SM69_SHADERS`,
0x80, DXR spec v1.47) is rejected by the system runtime: `CreateComputePipelineState` returned E_INVALIDARG
for every compute pipeline that set it, including passes with no reachable ray query. That is no longer the
route taken. The spec's supported route is a shader-model-6.9 `RayQuery`, which takes the opt-in as its
second template parameter:

```hlsl
RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS> q;
```

SM 6.9 needs a D3D12Core newer than the system one, so the build now ships the Agility SDK:

- `CMakeLists.txt` downloads `Microsoft.Direct3D.D3D12` 1.620.1 into `external/AgilitySDK`, stages
  `D3D12Core.dll` + `d3d12SDKLayers.dll` next to the executable, and puts its headers first for the exe.
- `src/D3D12RHI.cpp` exports `D3D12SDKVersion = D3D12_SDK_VERSION` (620) and `D3D12SDKPath = ".\\"`, which is
  what makes the process load the staged core instead of the system one.
- `src/shaders/shaders.cfg` pins every entry at `-m 6_9`; the per-entry pins override the `--shaderModel`
  default, so the two must agree.
- Every ray query in the renderer (`RaytracingCommon.hlsli`, `CommonLighting.hlsli`, `RAB_LightSampling.hlsli`,
  `RAB_VisibilityTest.hlsli`, `BrdfRayTracing.hlsl`) names `RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS`. This is
  **required**, not optional: the scene BLASes carry OMM links, and traversing them from a ray query that did
  not opt in is undefined behaviour.

Measured on Bistro: `[Init] Opacity micromaps: supported`, 107 arrays built as before, a full scene renders
with no NVRHI/assertion messages, and hiding the staged `D3D12Core.dll` reproduces the pre-Agility failure
(`D3D12CreateDevice` refuses the adapter) — the positive control that the new core is the one in use. A/B
screenshot of frame 8 with `--disable-omm`: mean absolute difference 2.0/255 per channel, no holes or black
patches on the alpha-tested foliage.

Open before T3.1/T3.3 can be *measured* rather than eyeballed:

- `--rhidebug` now aborts while enabling the debug layer (exception `0x87A` from KERNELBASE, no layer
  message) once the process has opted into an Agility core. Without the staged layers DLL the same call
  returns `DXGI_ERROR_SDK_COMPONENT_MISSING` (0x887A002D), as expected for an app that asked for SDK 620.
  Acquiring the controller through `D3D12GetInterface(CLSID_D3D12Debug)` instead of the legacy
  `D3D12GetDebugInterface` did not change it. Needs its own investigation (layer/core version interop).
- No candidate counter yet (T3.3), so "OMM changes traversal" is currently evidenced only by the A/B frame.
- RenderDoc still cannot show the OMM stage: its D3D12 layer clamps `OPTIONS5.RaytracingTier` to 1.1, which
  turns nvrhi's `RayTracingOpacityMicromap` gate off for capture runs.



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

**Exit criteria (met)**: every masked Bistro primitive bakes (39 of 42 primitives, 107 LODs, 18,735 masks,
7.7 MiB of mask data), the pass runs in parallel over the task scheduler (2.1 s against 4.97 s serial), and
the digest the summary logs is identical across runs (`811da39c2ca43b7f`). The three skipped primitives have
no CPU-readable alpha texture. No GPU work yet, nothing visible on screen.

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

**Exit criteria (met)**: `src/Omm/OmmRuntime.cpp` builds 107 arrays in 0.10 s (7.7 MiB mask data, 0.7 MiB
indices), per (primitive, LOD), and the BLAS geometry links them; `--rhidebug` on Bistro reports no
messages. The panel's toggle rebuilds the acceleration structures, which was exercised by forcing the
request once at frame 120 with the debug layer on (no messages, no crash). Letting traversal actually *use*
the masks is Phase 3: no compute pipeline sets the pre-SM6.9 opt-in yet.

#### Capture verification (rdc-tools, offline)

Two captures of Bistro, analysed with `D:\Workspace\rdc-tools` (`rdc_analysis.py`), neither of which can
show the OMM path — see "Why no capture contains the OMM stage" below.

*Load stage* — `--renderdoc-capture-frame 0` (177 MB, `verify`: `problems: 0`). Frame 0 is the frame the
load's command lists execute in, so the capture bracket now opens before `Scene::LoadScene`:
`LoadTextures` (3029 `List_CopyTextureRegion`), `Upload MaterialConstants`, `Upload Scene Buffers`,
`Upload Light Buffer`, **`Build Scene Accel Structs`** (1301 `List_BuildRaytracingAccelerationStructure`
calls), `RTXDI::Initialize`, then the first frame's passes.

*Frame 60* — `--renderdoc-capture-frame 60` (1.31 GB, `verify`: `problems: 0`, 14,081 chunks, 54
draw/dispatch calls, 125 marker ranges). Every stage the engine declares is present with its shader entry
point in the marker name: `GPUCulling_Culling_CSMain` / `GPUCulling_BuildIndirect_CSMain` (occlusion
culling phases 1–2 for opaque, masked and transparent, plus `Build Indirect Arguments`), the four base-pass
phases (`Base Pass Render (Phase 1/2) — Opaque / Opaque (Occluded) / Masked / Transparent`),
`DeferredLighting_DeferredLighting_PSMain`, the full RTXDI chain (`GenerateViewZ`, `PrepareLights`,
`PresampleLights`, `PresampleEnvironmentMap`, `PresampleReGIR`, `GenerateInitialSamples`,
`TemporalResampling`, `SpatialResampling`, `ShadeSamples`, `CompositingPass`), SHARC
(`SHARCUpdate`/`SharcResolve`/`SharcIndirectQuery`), `TLAS Update`/`TLAS Patch`, the NRD REBLUR chain
(7 sub-passes), HZB, SPD mip chains, bloom (prefilter/downsample/upsample/composite), luminance histogram,
exposure adaptation, tonemapping, FSR3 TAA and the sky pass.

Verdict-producing checks on that frame: `hazards` 10 findings / 8 distinct, `samplers` 0 arithmetic
verdicts (26 samplers, 28 pairs, every reachable mip range satisfied), `rootsig-check` 0 certain / 7
question. Every `hazards` finding has the same shape — one SRV plus ten UAVs all from `rp1 (table …)`, i.e.
a bindless descriptor *range* counted as if every descriptor in it were bound, on render-graph transients
(`HZB`, `OpaqueColorTexture`, `RTXDI_EnvLightPDF`, `RTXDI_LocalLight…`). The earlier Cornell capture
reports the identical four-shape set, so it is the analyser's bindless artefact and not something these
changes introduced; the OMM work adds no draw or dispatch to the frame.

*Sponza, frame 60* — `Sponza_capture.rdc` (176 MB, `verify`: `problems: 0`, 5408 chunks, 46 draw/dispatch
calls, 108 marker ranges): the same stage set (culling phases 1–2 for opaque and masked, both base-pass
phases, TLAS update/patch, HZB, the REBLUR chain, TAA, exposure adaptation, HDR post-processing) with
`hazards` 6 findings / 4 distinct (same bindless shape), `samplers` 0 verdicts, `rootsig-check` 0 certain.

**Why no capture contains the OMM stage.** `Feature::RayTracingOpacityMicromap` is answered by
`m_Options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_2` (`external/nvrhi/src/d3d12/d3d12-device.cpp:260`),
and RenderDoc's `WrappedID3D12Device::CheckFeatureSupport` clamps `D3D12_FEATURE_D3D12_OPTIONS5`'s
`RaytracingTier` to `D3D12_RAYTRACING_TIER_1_1` ("don't support DXR 1.2") — it also forces sampler feedback
to not-supported and zeroes work graphs. So with `renderdoc.dll` in the process the log reads
`[Init] Opacity micromaps: unsupported`, the bake and array build are skipped, and no BLAS is linked to a
micromap. Measured differentially on the same binary: `supported` without `--renderdoc`, `unsupported`
with it. The OMM stage is therefore verified by its own logs, the bake digest and a clean `--rhidebug`,
not by a capture.


---

### Phase 3 — Traversal correctness

**Unblocked**: traversal now opts in through shader model 6.9's `RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS` on the
Agility SDK runtime — see the note at the end of Phase 0 for the wiring, the measurements, and the two open
items (the `--rhidebug` abort and the missing candidate counter).

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
- Anything requiring the Agility SDK: Phase 0 established that none is needed (analysis §5.3).
