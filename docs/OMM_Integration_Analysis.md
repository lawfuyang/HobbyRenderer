# OMM (Opacity Micromaps) Integration Analysis for HobbyRenderer

> **Regenerated**: 2026-11-04 — supersedes the 2026-06-24 / 2026-07-03 / 2026-07-04 versions.
> **Scope**: what OMM is, what this codebase already has, what the machine can do, how the bake works,
> how it is cached, how it is debugged. The companion plan is
> [implementation_plan_omm.md](implementation_plan_omm.md).
> **References**: `D:\Workspace\niagara` (Arseny Kapoulkine's Vulkan renderer, load-time OMM bake +
> ray queries) and
> `D:\Workspace\DirectX-Graphics-Samples\Samples\Desktop\D3D12Raytracing\src\D3D12RaytracingOpacityMicromaps`
> (Microsoft's D3D12 sample, offline-baked OMM binaries, hit groups).

---

## 1. Executive Summary

| Question | Answer |
|---|---|
| Does the GPU/driver here support OMM? | **Yes.** The device reports `D3D12_RAYTRACING_TIER_1_2` and `D3D_SHADER_MODEL_6_8` (measured, §3). |
| Does it need the Agility SDK back? | **No.** The system runtime (`C:\Windows\System32\D3D12Core.dll`, 10.0.26100.9818) reports DXR 1.2, so no `D3D12SDKVersion` opt-in — the one removed in `263d41f` — is required. |
| Does nvrhi support OMM? | **Yes, fully implemented but compiled out.** `NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP:BOOL=OFF` in this build. |
| Does meshoptimizer bake OMMs? | **Yes.** Vendored `1.3` (`MESHOPTIMIZER_VERSION 1030`) ships `meshopt_opacityMapMeasure` / `Rasterize` / `EntrySize` / `Compact`. |
| Does the renderer use hit shaders? | **No.** All ray tracing is inline `RayQuery`, so OMM has to work in the ray-query path (SM 6.8 — which we build). |
| Is OMM a fit for this renderer? | **Yes, for MASK materials.** Every BLAS uses `GeometryFlags::None`, so every RT path alpha-tests candidates in shader code — exactly the work OMM removes. |
| Where should baked data live? | **A separate `_omm.bin`** next to `_mesh.bin`, keyed on the glTF timestamp, the bake parameters and the alpha texture timestamps. |

Three findings that change the earlier analysis:

1. **No Agility SDK requirement.** The previous document claimed the D3D12 OMM path needs it. On this
   machine the runtime already exposes DXR 1.2 through the system `D3D12Core.dll`, and the vendored
   DirectX-Headers (`v1.717.0-preview`) declare every OMM type:
   `D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC` (`d3d12.h:15353`),
   `D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES` (`d3d12.h:15056`),
   `D3D12_RAYTRACING_TIER_1_2` (`d3d12.h:2788`).
2. **meshoptimizer is 1.3, not 1.2**, and its OMM functions are plain declarations rather than something
   behind a define (`external/meshoptimizer/src/meshoptimizer.h:899-928`).
3. **nvrhi's OMM support is complete** — the only reason it is unavailable is the OFF CMake option.

---

## 2. What OMM Does

An opacity micromap is a per-triangle opacity mask. The triangle is subdivided into a uniform grid of
`4^N` micro-triangles (subdivision level N), each storing 1 bit (2-state: transparent / opaque) or 2 bits
(4-state: transparent / opaque / unknown-transparent / unknown-opaque). Traversal consults the mask before
reporting a hit:

- **fully transparent** micro-triangles are skipped in hardware — no candidate, no shader work;
- **fully opaque** micro-triangles are committed as hits without consulting shader code;
- **unknown** micro-triangles are reported as candidates and the application's alpha test decides.

Masks are shared through an OMM index buffer (one index per triangle) and the indices `-4..-1` express
"whole triangle is opaque/transparent/…" with no mask storage at all
(`external/meshoptimizer/README.md:851`, `:900`, `:909`).

Why it matters here: every RT path currently performs a shader-side alpha test per candidate triangle
(`src/shaders/RaytracingCommon.hlsli:196-213`, `src/shaders/CommonLighting.hlsli:405-470`,
`src/shaders/rtxdi/LightingPasses/RtxdiApplicationBridge/RAB_RTShaders.hlsli:17-50`). OMM removes the
candidates no shader needs to see, and removes the any-hit-equivalent entirely for opaque islands.

Costs, from the references: ~120 masks for a tree model cost ~3 KB; a 4-byte index per triangle for
82 K triangles costs ~2.5 MB with the NVIDIA SDK's 32-bit indices, ~50 % of that with 16-bit indices
(`D3D12RaytracingOpacityMicromaps/readme.md`, "Things to note").

---

## 3. Measured Capability

A temporary probe logged the device capabilities through `CD3DX12FeatureSupport` (in
`src/D3D12RHI::Initialize`, reverted after measuring):

```
[Probe] RaytracingTier=12 MaxShaderModel=104      // D3D12_RAYTRACING_TIER_1_2, D3D_SHADER_MODEL_6_8
```

Consequences:

- `D3D12_RAYTRACING_TIER_1_2` is exactly what nvrhi tests for `m_OpacityMicromapSupported`
  (`external/nvrhi/src/d3d12/d3d12-device.cpp:259-261`), so the capability check will pass once the build
  flag is on.
- Shader model 6.8 is also the ceiling without the Agility SDK opt-in (`CMakeLists.txt`, the
  `build_shaders` comment). Ray-query OMM is a 6.8 feature, which matches what we build — but this is the
  one assumption that must be proven by an actual DXC compile in Phase 0 of the plan.
- RTXMU is off (`NVRHI_WITH_RTXMU:BOOL=OFF`), which matters because nvrhi forces OMM off when RTXMU is
  enabled (`d3d12-device.cpp:418-422`, "RTXMU does not support OMMs").

Engine-side feature query:

```cpp
m_RHI->m_NvrhiDevice->queryFeatureSupport(nvrhi::Feature::RayTracingOpacityMicromap)
// d3d12-device.cpp:854  case Feature::RayTracingOpacityMicromap: return m_OpacityMicromapSupported;
```

---

## 4. What the Codebase Already Has

### 4.1 nvrhi runtime API (present, compiled out)

| Item | Location |
|---|---|
| `rt::OpacityMicromapFormat` (`OC1_2_State`, `OC1_4_State`), `OpacityMicromapBuildFlags`, `OpacityMicromapUsageCount`, `OpacityMicromapDesc`, `IOpacityMicromap`, `OpacityMicromapHandle` | `external/nvrhi/include/nvrhi/nvrhi.h:1473-1537` |
| `GeometryTriangles::opacityMicromap / ommIndexBuffer / ommIndexBufferOffset / ommIndexFormat / pOmmUsageCounts / numOmmUsageCounts`, `setOpacityMicromap(...)` | `nvrhi.h:1596-1617` |
| `Device::createOpacityMicromap(desc)` | `nvrhi.h:3988`; impl `d3d12-raytracing.cpp:784-812` (D3D12 native branch under `#if NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP`) |
| `CommandList::buildOpacityMicromap(omm, desc)` | `nvrhi.h:3667`; impl `d3d12-raytracing.cpp:2189-2260` (barriers, scratch, build call) |
| OMM linkage fill for BLAS geometry | `d3d12-raytracing.cpp:648-656` (`fillD3dGeometryOMMLinkageDesc`), geometry switch `:695-703`, BLAS path `:776-777` |
| Instance flags `ForceOMM2State` (16), `DisableOMMs` (32) | `nvrhi.h:1746-1747` |
| Pipeline flag `allowOpacityMicromaps` — upstream only consumes it for ray-tracing state objects (`d3d12-raytracing.cpp:1751-1754`); the fork also maps it onto compute/graphics pipelines, see §5.3 | `nvrhi.h:2978` |

The build switch to flip, next to the existing nvrhi options at `CMakeLists.txt:158-160`:

```cmake
set(NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP ON CACHE BOOL "" FORCE)
```

(`external/nvrhi/CMakeLists.txt:56` defines the option, `:401` turns it into a compile definition.)

### 4.2 meshoptimizer 1.3 baking API (present, unused)

```cpp
// external/meshoptimizer/src/meshoptimizer.h
:899  size_t meshopt_opacityMapMeasure(unsigned char* levels, unsigned int* sources, int* omm_indices,
                                       const unsigned int* indices, size_t index_count,
                                       const float* vertex_uvs, size_t vertex_count, size_t vertex_uvs_stride,
                                       unsigned int texture_width, unsigned int texture_height,
                                       int max_level, float target_edge);
:913  void   meshopt_opacityMapRasterize(unsigned char* result, int level, int states,
                                       const float* uv0, const float* uv1, const float* uv2,
                                       const unsigned char* texture_data, size_t texture_stride,
                                       size_t texture_pitch, unsigned int texture_width, unsigned int texture_height);
:914  size_t meshopt_opacityMapEntrySize(int level, int states);
:928  size_t meshopt_opacityMapCompact(unsigned char* data, size_t data_size, unsigned char* levels,
                                       unsigned int* offsets, size_t omm_count, int* omm_indices,
                                       size_t triangle_count, int states);
```

Semantics (from the vendored README, `:849-911`):

- `measure` picks a subdivision level per triangle from its texel footprint and **deduplicates triangles
  sharing a UV region**; `omm_indices` already points at shared entries.
- `rasterize` samples the alpha channel (`texture_data + 3`, stride 4) at micro-triangle centres: 4-state
  produces unknown states at edges, 2-state is conservative. Mip 0 for 4-state quality; mips 1-2 are fine
  for 2-state.
- `compact` merges identical entries and rewrites whole-triangle cases into special indices.
- Serialisation after compaction is explicitly endorsed; 16-bit indices are allowed while values stay in
  `[0..65531]`.
- **Index order matters**: triangles get rotated, so bake after the index order is final, or normalise
  corner order consistently — niagara rotates each triangle so the smallest index comes first
  (`D:\Workspace\niagara\src\scene.cpp:855-878`).

### 4.3 Acceleration structure layout (the key constraint)

- One **BLAS per primitive per LOD**, built once at load: `src/Scene.cpp:78-125`, with
  `m_RTIndexBuffer` (`:103`), `R32_UINT` indices (`:105`), `GeometryFlags::None` (`:113`), a single
  geometry per BLAS (`:117`), create+build (`:121-122`).
- BLAS handles live on the primitive: `src/Scene.h:35-36`.
- The TLAS is rebuilt **every frame** from `m_RTInstanceDescBuffer` with per-instance BLAS addresses
  patched by `TLASPatch_CS`: `src/CommonRenderers.cpp:225-247`, `src/shaders/TLASPatch.hlsl:29-50`;
  instance list at `src/Scene.cpp:139-165`, `blasDeviceAddress` at `:164`.
- Instances already carry opaqueness: `ForceOpaque` for `ALPHA_MODE_OPAQUE`, else `ForceNonOpaque`
  (`src/Scene.cpp:156-162`); `instanceMask` is always 1 (`:160`).
- Because BLASes are per LOD, **OMM arrays and OMM index buffers must be per LOD** (or explicitly limited
  to LOD0). A LOD is an index range of the same RT index buffer (`meshData.m_IndexOffsets[lod]`,
  `m_IndexCounts[lod]`), so mask index buffers slice the same ranges.

### 4.4 Materials and alpha

- `Scene::Material` holds `srrhi::MaterialConstants m_GPU` with `m_AlphaMode` / `m_AlphaCutoff`
  (`src/Scene.h:122-144`; generated struct `src/shaders/Instance.sr:31-32`).
- glTF mapping: `mask` → `ALPHA_MODE_MASK` + `alpha_cutoff`; `blend` → `ALPHA_MODE_BLEND`;
  `KHR_materials_transmission` forces BLEND (`src/SceneLoader.cpp:1208-1226`).
- Render buckets by alpha mode: `src/Scene.cpp:303-337`.
- Raster alpha test: `ALPHA_TEST=1` permutation (`src/shaders/shaders.cfg:8`,
  `src/shaders/BasePass.hlsl:283-289`, selected at `src/BasePassRenderer.cpp:419-421`).
- RT alpha tests, all shader-side (no hit shaders): `RaytracingCommon.hlsli:121-215`,
  `CommonLighting.hlsli:390-478`, `RAB_RTShaders.hlsli:17-50`, `RAB_VisibilityTest.hlsli:52-131`.

**Consequence**: OMM applies to `ALPHA_MODE_MASK` primitives only. BLEND materials are stochastic and keep
their current path — niagara draws the same line (`postPass == 1` only,
`D:\Workspace\niagara\src\scene.cpp:887-903`).

### 4.5 Textures available for baking

- Scene textures are DDS and BC-compressed: `bIsBC` at `src/SceneLoader.cpp:1322`, per-mip offsets via
  `ComputeDDSMipOffsets` (`src/TextureLoader.h`, used `SceneLoader.cpp:1359-1360`), pixel data mmap'd
  through `MemoryMappedDataReader` (`st.m_SourceData`).
- The bake therefore cannot read RGBA8 directly: it needs a **BC decoder for the chosen mip**
  (BC1 has 1-bit alpha, BC3/BC7 carry alpha, BC4/BC5 have none). This is the largest new piece of code in
  the bake path, and part of why baking must be cached.
- No GPU involvement is needed: the mmap holds the bytes.

### 4.6 Debug-view infrastructure

| Mechanism | Where |
|---|---|
| Debug mode enum (`DEBUG_MODE_*`) | `src/shaders/Common.sr:83-94` → `src/shaders/srrhi/hlsl/Common.hlsli:67-78` |
| Debug mode into shaders | `Renderer::m_DebugMode` (`src/Renderer.h:415-417`) → pass CBs (`src/BasePassRenderer.cpp:365`, `src/DeferredRenderer.cpp:75`) → `BasePass.hlsl:445-495`, `DeferredLighting.hlsl:110-130` |
| Full-screen debug pass template | `src/Streaming/TileResidencyDebugRenderer.cpp:1-152`, scheduled before `ImGuiRenderer` (`src/Renderer.cpp:1168-1170`) |
| ImGui controls and stats | `src/ImGuiLayer.cpp:78-130`, `:413-430`, `:504-645` |
| Render-graph debug UI | `src/RenderGraphUI.cpp:6+` |

All three OMM debug views can be built from these: one new debug pass modelled on
`TileResidencyDebugRenderer`, new `DEBUG_MODE_*` values, one ImGui panel.

---

## 5. Hardware and Runtime Caveats

1. **The device reports DXR 1.2.** OMM is a Turing+ hardware feature; the D3D12 tier is what nvrhi checks.
   Verify a second machine before treating it as guaranteed.
2. **No Agility SDK means the runtime is whatever Windows ships.** An older `D3D12Core.dll` reports a lower
   tier, so the feature must degrade gracefully (feature query gate; simply not attaching OMM).
3. **Ray queries, not hit groups.** OMM works through `RayQuery` here: hardware filters candidates, our
   existing alpha test handles "unknown" ones. `D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS`
   applies to state objects we never create.
4. **OMM has no mips.** At distance a subdiv-6 mask against a minified texture can alias; the D3D12 sample
   notes exactly this. Mitigation: LOD0-only masks first, lower `max_level` for small instances.
5. **`ForceOMM2State`** (`InstanceFlags::ForceOMM2State`) makes 4-state data behave as 2-state at traversal
   time — a quality/perf knob and an A/B switch for debugging.

### 5.3 Ray-query opt-in at shader model 6.8 (measured)

`RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS` is a **template parameter as of shader model 6.9**, and DXC rejects
both it and `RAY_FLAG_FORCE_OMM_2_STATE` at `--shader-model 6_8` ("introduced in shader model 6.9"). That
looked like an Agility SDK / SM 6.9 requirement. It is not: the DXR spec ("RayQuery flags on pre Shader
Model 6.9 shaders") defines an API-level opt-in for exactly this case:

| Level | Flag |
|---|---|
| pipeline (`CreateComputePipelineState`, `CreateGraphicsPipelineState`, `CreatePipelineState`) | `D3D12_PIPELINE_STATE_FLAG_RAYQUERY_ALLOW_OPACITY_MICROMAPS_ON_PRE_SM69_SHADERS` (0x80) |
| state object (`CreateStateObject`) | `D3D12_STATE_OBJECT_FLAG_RAYQUERY_ALLOW_OPACITY_MICROMAPS_ON_PRE_SM69_SHADERS` (0x800) |

Because all ray tracing here is inline `RayQuery` inside compute passes, the pipeline-level flag is the one
that matters. nvrhi upstream never set it — `allowOpacityMicromaps` reached only ray-tracing state objects,
and `Device::createPipelineState` for compute/graphics/meshlet passed no `D3D12_PIPELINE_STATE_FLAGS` at
all. The fork closes that gap (`d3d12-backend.h` constant, `d3d12-compute.cpp` application, both wrapped in
`[rlaw]` markers); the vendored DirectX-Headers stop before the enum value, so the backend carries the
spec's `0x80` literal with the name in a comment.

Verified on this machine: `Feature::RayTracingOpacityMicromap` is `true`, the flag is accepted by
D3D12Core 10.0.26100.9818, and `--rhidebug` reports no messages. **No Agility SDK, no SM 6.9.** The
6.9-only `RAY_FLAG_FORCE_OMM_2_STATE` is replaced by `D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OMM_2_STATE`
(`InstanceFlags::ForceOMM2State`), which has no shader model requirement.

---

## 6. Bake Pipeline (CPU, load time)

```
glTF primitive (MASK material)
        │
        ├── RT index slice  (Scene.cpp:103  m_RTIndexBuffer + m_IndexOffsets[lod])
        ├── UVs             (VertexQuantized.m_Uv = half2 in one uint  ->  meshopt_dequantizeHalf)
        └── alpha texture   (DDS mip N, BC-decoded to RGBA8)
                 │
                 ▼  meshopt_opacityMapMeasure      -> levels[], sources[], ommIndices[] (deduplicated)
                 ▼  meshopt_opacityMapEntrySize    -> per-entry offsets, allocate data
                 ▼  meshopt_opacityMapRasterize    -> per-entry micro-triangle states (2- or 4-state)
                 ▼  meshopt_opacityMapCompact      -> dedup + special indices (-4..-1), trim data
                 ▼
   { ommData: uint8[], ommLevels: uint8[], ommOffsets: uint32[], ommIndices: int32[] }
                 ▼
   upload -> nvrhi::rt::OpacityMicromapDesc{ counts (histogram), inputBuffer, perOmmDescs }
                 ▼
   createOpacityMicromap + buildOpacityMicromap -> attach to BLAS GeometryTriangles
```

Details that follow from *this* codebase:

- **Corner order.** Rotate each triangle of the *RT* index buffer so the smallest index comes first, as in
  `D:\Workspace\niagara\src\scene.cpp:855-878`. The raster index buffer is separate
  (`Scene_IndexBuffer`, `SceneLoader.cpp:2323`), so only the RT buffer is touched; barycentrics from ray
  hits stay consistent because they are relative to the stored triangle order.
- **Per-LOD decision.** Bake LOD0 first (path tracer and closest instances always use it,
  `PathTracer.hlsl:102`), then extend to LODs that actually appear in RT.
- **Per-primitive, not per-mesh.** Two primitives can share a mesh with different materials (and texts),
  and the BLAS is per primitive — masks are per primitive.
- **Histogram.** `OpacityMicromapUsageCount { count, subdivisionLevel, format }` is required input
  (`nvrhi.h:1489-1497`), converted to `D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY`
  (`d3d12-raytracing.cpp:511-543`).

---

## 7. SceneCache: Bake Once, Reload Fast

### 7.1 What the cache does today

```
[glTF] ──► IsCacheValid (mtime only, SceneCache.cpp:7-21)
              │ yes                                    │ no
              ▼                                        ▼
      LoadCookedMesh                     SceneLoader::ProcessMeshesFromGLTF
      (SceneCache.cpp:78-143)                       │
              │                                     ▼
              └──────────► GPU upload      SaveCookedMesh (SceneCache.cpp:22-67)
```

- Path: `<scene stem>_mesh.bin` next to the glTF (`SceneCache.cpp:155`); `kCookedMeshVersion = 2`
  (`SceneCache.h:33`), magic `0x59464C52` (`SceneCache.h:26`).
- Format: `magic, version, meshes, meshData, meshlets, meshletVertices, meshletTriangles,
  allVerticesQuantized, allIndices` — a fixed sequence with no section table (`SceneCache.h:6-26`).
- Entry point `SceneCache::LoadOrCookMeshData` (`SceneCache.cpp:148-230`), called from
  `Scene::LoadScene` (`src/Scene.cpp:42`) after materials/nodes are parsed (`Scene.cpp:38`) and before
  `FinalizeLoadedScene` / `BuildAccelerationStructures` (`Scene.cpp:44-57`).

### 7.2 Why OMM data gets its own cache file

| Option | Verdict |
|---|---|
| Append an OMM section to `_mesh.bin` | Rejected: one version constant for both, so tuning an OMM parameter (or an OMM API change in meshoptimizer) would discard the mesh cache too |
| **Separate `_omm.bin`** | **Chosen**: same POD read/write helpers, own magic and version, independent invalidation |

Cache key — everything that changes the baked bytes:

```
glTF last_write_time
+ OMM parameters: maxLevel, targetEdge, states (2/4), texture mip index, (alpha cutoff is in the material,
  so it is already covered by the glTF timestamp)
+ for every alpha texture used by a MASK primitive: path, size, last_write_time
+ kCookedMeshVersion (the bake consumes the cached index order)
```

The texture term is what today's cache lacks, and the reason the earlier analysis recommended no caching.
It is cheap: the bake touches those files anyway, so the same pass records `(path, size, mtime)`.

Expected saving: decoding BC blocks for one mip and rasterising `4^N` cells per unique triangle is
milliseconds to tens of milliseconds per MASK primitive and scales with texture resolution — Bistro's
foliage/fence/sign geometry is exactly the expensive case. **Measure in Phase 4** (cold vs warm load time);
do not assume.

### 7.3 GPU-side work is always rebuilt

`_omm.bin` only removes the CPU bake. `createOpacityMicromap` + `buildOpacityMicromap` are GPU operations
with per-session allocation and a recorded build (`d3d12-raytracing.cpp:784-812`, `:2189-2260`), and the
BLAS must be rebuilt because OMM linkage is part of it.

---

## 8. Debug Visualisation Design

### 8.1 Texel classification (opaque / transparent / mixed)

Goal: show, per texel, how the baker classified it — where the conservative rules bite and where a higher
subdivision would help.

- Classification used by the baker: for a micro-triangle, all samples ≥ cutoff → **opaque**; all < cutoff →
  **transparent**; otherwise **mixed** (unknown in 4-state; forced opaque in 2-state).
- Implementation: a debug pass (modelled on `TileResidencyDebugRenderer`) that samples the same mip the
  bake read and outputs colour: green = fully opaque, red = fully transparent, yellow = mixed. Zoom, pan
  and "only mixed" toggles in the ImGui panel.
- Same panel shows per-primitive statistics: entry count, average subdivision level, and the percentage of
  transparent / opaque / mixed micro-triangles.

### 8.2 Micromap grid view

Goal: see one triangle's actual mask. Draw the `4^N` grid in an ImGui window (`ImDrawList`, one filled
triangle per micro-triangle, coloured by state) for a triangle selected by index, and/or rasterise the
whole mask array into a strip texture from a debug pass. This is what explains "the mask does not match the
texture".

### 8.3 Traversal cost view

Goal: prove OMM pays off. A per-pixel `uint` counter incremented for every shader-side alpha test
(candidate) in `TraceRayStandard` / `CalculateRTShadow` / the RTXDI bridge, plus the same measurement with
`InstanceFlags::DisableOMMs`. Present as a heatmap in the debug pass and as totals (candidates/frame and
the with/without delta) in the ImGui panel.

---

## 9. Risk Register

| Risk | Mitigation |
|---|---|
| Ray-query OMM needs more than SM 6.8 | Phase 0 compiles a `RayQuery` shader using the OMM ray flag with DXC before any engine work; if it fails, OMM is deferred rather than half-built |
| OMM data mismatched to a LOD (index order) | Bake after the index order is final; assert `ommIndices.size() == m_IndexCounts[lod]/3`; debug view 8.2 shows the grid |
| Texture changed but cache still valid | `_omm.bin` key includes size + mtime of every referenced alpha texture |
| 4-state vs 2-state quality regressions | Per-material `states` switch plus `ForceOMM2State`; A/B through the traversal counters |
| Memory growth (masks, index buffers, cache) | Log per-scene totals; cap `max_level` (6 default); 16-bit indices; compaction on |
| Aliasing at distance (no mask mips) | Leave small instances without OMM, or lower the level; compare screenshots |
| BLAS build cost with OMM attached | Measure BLAS build time with and without linkage |

---

## 10. Recommendations

1. **Enable the nvrhi path and prove the capability first** — flip
   `NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP`, assert `Feature::RayTracingOpacityMicromap`, and compile one
   ray-query shader that uses the OMM ray flag (Phase 0).
2. **Bake with meshoptimizer 1.3** (measure → rasterize → compact) rather than the NVIDIA OMM SDK the D3D12
   sample uses: no new dependency, same format, and it is the pipeline the reference renderer uses.
3. **Start with 4-state, `max_level = 6`, `target_edge = 3`, mip 0**, and expose `states` / `max_level` as
   settings so scenes can be tuned.
4. **Attach OMM per primitive per LOD, LOD0 first**, gated on the capability check and an ImGui toggle.
5. **Cache in `_omm.bin`** with the key from §7.2 — that is what makes reloads cheap and keeps the mesh
   cache stable.
6. **Ship the three debug views with the feature**, not after it: without them "is the mask right?" cannot
   be answered.

---

## 11. References

Local references used for this analysis:

- `D:\Workspace\niagara` — `src/scene.cpp:855-878` (`normalizeIndicesForOMM`), `:880-1070`
  (`buildSceneOmm`: measure → rasterize → compact, mesh→texture resolution, statistics),
  `src/scene.h:105-108` (`ommStates`, `ommData`, `ommDescs`), `src/scenert.cpp:581-692` (`buildOMM`),
  `src/scenecache.cpp:150-190`, `:273-345` (OMM data serialised in the scene cache),
  `src/shaders/shadow.comp.glsl:10-22`, `:155` (ray query + OMM,
  `gl_RayFlagsForceOpacityMicromap2StateEXT`).
- `D:\Workspace\DirectX-Graphics-Samples\Samples\Desktop\D3D12Raytracing\src\D3D12RaytracingOpacityMicromaps`
  — `D3D12RaytracingOpacityMicromaps.cpp:288-336` (offline `.bin` load: descs, array, histogram, indices),
  `:484-707` (OMM array build, OMM geometry + linkage, BLAS/TLAS, `ALLOW_DISABLE_OMMS`), `readme.md`
  (2 vs 4-state, memory costs, any-hit comparison).
- `external/meshoptimizer/README.md:849-911` — authoritative bake semantics and serialisation rules.
- `external/nvrhi` — `include/nvrhi/nvrhi.h:1473-1537`, `:1596-1617`, `:1746-1747`, `:3667`, `:3988`;
  `src/d3d12/d3d12-raytracing.cpp:648-656`, `:695-703`, `:776-777`, `:784-812`, `:2189-2260`;
  `src/d3d12/d3d12-device.cpp:259-261`, `:418-434`, `:854-855`.
- `PIPELINE.md` — IRenderer, Render Graph, `srrhi` / `ShaderIDs` generation rules.
