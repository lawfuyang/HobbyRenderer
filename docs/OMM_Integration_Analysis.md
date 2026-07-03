# OMM (Opacity Micro-Maps) Integration Analysis for HobbyRenderer

> **Date**: 2026-06-24 (original) | **Updated**: 2026-07-03 — meshoptimizer upgraded to v1.2
> **Scope**: Full analysis of OMM generation and runtime usage — load-time generation via meshoptimizer v1.2's built-in `opacityMap*` APIs (as used by [zeux/niagara](https://github.com/zeux/niagara)), plus NVRHI native runtime support

---

## 1. Executive Summary

**Your NVRHI already handles runtime OMM fully, and meshoptimizer 1.2 now provides built-in OMM baking — no custom code required.**

| Concern | What provides it | Status in your codebase |
|---|---|---|
| **OMM Generation** (load-time CPU) | `meshoptimizer` v1.2 — `meshopt_opacityMapMeasure` / `meshopt_opacityMapRasterize` / `meshopt_opacityMapCompact` (EXPERIMENTAL) | ✅ Already present (requires integration) |
| **OMM Runtime** (GPU build, BLAS attachment) | Your NVRHI (`external/nvrhi`) | ✅ Fully implemented |
| **meshoptimizer** (meshlet clustering, compression, etc.) | meshoptimizer v1.2 (`external/meshoptimizer`) | ✅ Already present, **now includes OMM** |

**Key insight**: meshoptimizer 1.2 ships with experimental OMM baking APIs that handle subdivision-level measurement, alpha rasterization, and duplicate compaction — the exact pipeline the [zeux/niagara](https://github.com/zeux/niagara) reference renderer uses. You don't need to write a custom `BakeOmmData()` function; instead, integrate `meshopt_opacityMapMeasure` → `meshopt_opacityMapRasterize` → `meshopt_opacityMapCompact` for a proven, zero-dependency OMM generation pipeline.

---

## 2. Detailed Analysis

### 2.1 meshoptimizer — Now Includes OMM Baking (v1.2)

- **Repo**: [https://github.com/zeux/meshoptimizer](https://github.com/zeux/meshoptimizer)
- **Your version**: `v1.2` (`MESHOPTIMIZER_VERSION 1020`)
- **License**: MIT

#### What it does have:

meshoptimizer is a mesh optimization library focused on GPU rendering efficiency. It provides:

| Category | Functions |
|---|---|
| Vertex cache optimization | `meshopt_optimizeVertexCache`, `meshopt_optimizeVertexCacheStrip`, `meshopt_optimizeVertexCacheFifo` |
| Overdraw optimization | `meshopt_optimizeOverdraw` |
| Vertex fetch optimization | `meshopt_optimizeVertexFetch`, `meshopt_optimizeVertexFetchRemap` |
| Vertex remap generation | `meshopt_generateVertexRemap`, `meshopt_generateVertexRemapMulti`, `meshopt_generateVertexRemapCustom` |
| Index/vertex remapping | `meshopt_remapVertexBuffer`, `meshopt_remapIndexBuffer` |
| Index buffer filtering | `meshopt_filterIndexBuffer`, `meshopt_filterIndexBufferMulti` |
| Shadow index buffer | `meshopt_generateShadowIndexBuffer`, `meshopt_generateShadowIndexBufferMulti` |
| Position remap | `meshopt_generatePositionRemap` |
| Adjacency/tessellation index | `meshopt_generateAdjacencyIndexBuffer`, `meshopt_generateTessellationIndexBuffer` |
| Provoking vertex index | `meshopt_generateProvokingIndexBuffer` |
| Mesh simplification | `meshopt_simplify`, `meshopt_simplifyWithAttributes`, `meshopt_simplifyWithUpdate`, `meshopt_simplifySloppy`, `meshopt_simplifyPrune`, `meshopt_simplifyPoints` |
| Meshlet/cluster building | `meshopt_buildMeshlets`, `meshopt_buildMeshletsScan`, `meshopt_buildMeshletsSpatial` |
| Meshlet encoding | `meshopt_encodeMeshlet`, `meshopt_decodeMeshlet`, `meshopt_decodeMeshletRaw` |
| Index/vertex compression | `meshopt_encodeIndexBuffer`, `meshopt_encodeIndexSequence`, `meshopt_encodeVertexBuffer`, `meshopt_encodeVertexBufferLevel` |
| Spatial sorting | `meshopt_spatialSortRemap`, `meshopt_spatialSortTriangles`, `meshopt_spatialClusterPoints` |
| Stripification | `meshopt_stripify`, `meshopt_unstripify` |
| Tangent generation | `meshopt_generateTangents` |
| OMM baking (EXPERIMENTAL) | `meshopt_opacityMapMeasure`, `meshopt_opacityMapRasterize`, `meshopt_opacityMapEntrySize`, `meshopt_opacityMapCompact` |
| Analysis | `meshopt_analyzeVertexCache`, `meshopt_analyzeVertexFetch`, `meshopt_analyzeOverdraw`, `meshopt_analyzeCoverage` |
| Bounds computation | `meshopt_computeClusterBounds`, `meshopt_computeMeshletBounds` |

#### OMM Baking Functions (NEW in v1.2 — EXPERIMENTAL)

meshoptimizer 1.2 ships with a complete OMM baking pipeline behind the `MESHOPTIMIZER_EXPERIMENTAL` flag. These are the same APIs used by the [zeux/niagara](https://github.com/zeux/niagara) reference renderer (see `REFERENCES/niagara/src/scene.cpp:buildSceneOmm()`):

| Function | Purpose |
|---|---|
| `meshopt_opacityMapMeasure()` | Analyzes alpha-masked triangles, computes per-triangle subdivision levels, deduplicates identical UV regions |
| `meshopt_opacityMapRasterize()` | Rasterizes alpha texture at micro-triangle sample points using bilinear filtering and 0.5 alpha cutoff |
| `meshopt_opacityMapEntrySize()` | Returns the byte size for an OMM entry at a given subdivision level |
| `meshopt_opacityMapCompact()` | Merges identical micropmap entries and replaces them with special indices (-4..-1) when the entire entry is fully opaque/transparent |

Supports both **2-state** (opaque/transparent, `OC1_2_State`) and **4-state** (opaque/transparent/unknown, `OC1_4_State`) formats. Subdivision levels range from 0–12 with adaptive subdivision via `target_edge`.

#### What it still does NOT have:

- ❌ **No GPU OMM build** — that's NVRHI's job
- ❌ **No BLAS attachment** — that's NVRHI's job
- ❌ **No budget management** — you must manage OMM data size yourself

#### How meshoptimizer helps in an OMM pipeline:

- **OMM baking**: The `meshopt_opacityMap*` family provides the full CPU-side OMM pipeline: measure → rasterize → compact. No custom code needed.
- **Meshlet clustering**: `meshopt_buildMeshlets()` produces spatial clusters that improve OMM efficiency by grouping related triangles together.
- **Spatial sorting**: `meshopt_spatialSortTriangles()` reorders triangles for better locality, improving OMM bake quality.
- **Vertex optimization**: Reduces vertex shader overhead when rendering the base geometry alongside OMMs.

**Conclusion**: meshoptimizer v1.2 is now a complete OMM baking solution when paired with NVRHI's runtime. No upgrade is needed — you already have v1.2. Use the `meshopt_opacityMap*` APIs directly instead of writing custom OMM generation code.

---

### 2.2 NVRHI — Runtime OMM Support (Already Present)

- **Location**: `external/nvrhi/`
- **Status**: ✅ **Comprehensive runtime OMM support already implemented**

#### Architecture

NVRHI supports OMM through **two parallel code paths**, auto-selected at compile time:

```
┌─────────────────────────────────────────────────────┐
│              NVRHI OMM Runtime Support               │
├───────────────────────┬─────────────────────────────┤
│   DXR12 Agility SDK   │      NVAPI (Ada+)           │
│  (NVRHI_D3D12_WITH_   │  (NVRHI_WITH_NVAPI_         │
│   DXR12_OPACITY_      │   OPACITY_MICROMAP)          │
│   MICROMAP)           │                             │
├───────────────────────┼─────────────────────────────┤
│ • Uses D3D12 native   │ • Uses NvAPI_D3D12_*        │
│   OMM API             │   extension functions        │
│ • Requires Agility SDK│ • Requires NVAPI SDK ≥R520  │
│ • Dev Guide Preview   │ • Ada GPU (RTX 40 series)   │
└───────────────────────┴─────────────────────────────┘
```

Also supported on Vulkan via `VK_EXT_opacity_micromap`.

#### NVRHI OMM API Surface

**Data structures** (from `external/nvrhi/include/nvrhi/nvrhi.h`):

```cpp
namespace rt {
    enum class OpacityMicromapFormat {
        OC1_2_State = 1,  // 1-bit per micro-triangle (opaque/transparent)
    };

    enum class OpacityMicromapBuildFlags : uint8_t {
        None           = 0,
        FastTrace      = 1,  // Prefer trace performance
        FastBuild      = 2,  // Prefer build speed
        AllowCompaction = 4, // OMM array can be compacted
    };

    struct OpacityMicromapUsageCount {
        uint32_t count;              // Number of OMMs with these params
        uint32_t subdivisionLevel;   // Micro-triangle count = 4^N
        OpacityMicromapFormat format;
    };

    struct OpacityMicromapDesc {
        OpacityMicromapBuildFlags flags;
        std::vector<OpacityMicromapUsageCount> counts; // Histogram
        IBuffer* inputBuffer;        // Raw OMM data
        uint64_t inputBufferOffset;
        IBuffer* perOmmDescs;        // Per-OMM descriptors
        uint64_t perOmmDescsOffset;
        // ...
    };
}
```

**Device API:**
- `Device::createOpacityMicromap(desc)` — allocates GPU buffer for OMM array
- `CommandList::buildOpacityMicromap(omm, desc)` — builds OMM array from input data on GPU
- `CommandList::compactOpacityMicromap()` — post-build compaction (when `AllowCompaction` is set)

**BLAS attachment** (the geometry descriptor includes OMM fields):
```cpp
// In GeometryDesc::Triangles:
IOpacityMicromap* opacityMicromap;   // OMM array to attach
IBuffer*          ommIndexBuffer;    // Per-triangle OMM index (R16 or R32)
Format            ommIndexFormat;    // R16_UINT or R32_UINT
OpacityMicromapUsageCount* pOmmUsageCounts;
uint32_t           numOmmUsageCounts;
```

#### Capability Detection

NVRHI queries hardware OMM support:
- DXR12 path: Assumed available when Agility SDK is linked
- NVAPI path: Checks `NVAPI_D3D12_RAYTRACING_CAPS_TYPE_OPACITY_MICROMAP` → `NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAP_STANDARD`

---

### 2.3 Load-Time OMM Generation — Use meshoptimizer's Built-in APIs

- **Reference**: [zeux/niagara](https://github.com/zeux/niagara) by Arseny Kapoulkine (same author as meshoptimizer), specifically `REFERENCES/niagara/src/scene.cpp:buildSceneOmm()`
- **License**: MIT
- **Language**: C++ (Vulkan renderer)

#### Key Insight

The OMM binary data format is conceptually very simple:

```
For each triangle at subdivision level N:
  - There are 4^N micro-triangles (N=2 → 16, N=3 → 64, N=4 → 256)
  - Each micro-triangle stores 1 bit (OC1_2_State): opaque or transparent
  - The GPU reads this data to skip fully-transparent micro-triangles in ray tracing

That's it. You just need to rasterize alpha textures at micro-triangle sample points.

CRITICAL: In OC1_2_State, there is no "unknown" state. The choice is binary:
  • Mark a micro-triangle TRANSPARENT → ray skips it entirely (no AnyHit invoked)
  • Mark a micro-triangle OPAQUE   → ray hits it, AnyHit resolves final visibility

This means you MUST be conservative: only mark a micro-triangle TRANSPARENT
when ALL texel samples within it are below the alpha threshold.
Edge/boundary micro-triangles (mix of above and below) must be marked OPAQUE,
and the AnyHit shader handles the fine-grained alpha test for those regions.
```

#### The meshoptimizer OMM Pipeline

meshoptimizer 1.2 provides a complete OMM baking pipeline via four experimental APIs. The [zeux/niagara](https://github.com/zeux/niagara) renderer (see `REFERENCES/niagara/src/scene.cpp`) demonstrates the end-to-end flow:

**Step 1: Measure** — `meshopt_opacityMapMeasure()`
```cpp
// For each alpha-masked mesh, determine subdivision levels and deduplicate triangles:
// - levels[i]: subdivision level for OMM entry i (0–12)
// - sources[i]: source triangle index for entry i
// - ommIndices[i]: maps each triangle to its OMM entry (-1 = fully opaque, skip)
size_t ommCount = meshopt_opacityMapMeasure(
    levels.data(), sources.data(), ommIndices.data(),
    indexBuffer, triangleCount * 3,
    reinterpret_cast<const float*>(texcoords.data()), vertexCount, sizeof(vec2),
    texture.width, texture.height,
    maxLevel,       // e.g., 6 (see kOmmSubdivisionLevel in niagara)
    targetEdge);    // e.g., 3.0 / (1 << ommMip) — adaptive subdivision target
```

**Step 2: Rasterize** — `meshopt_opacityMapRasterize()`
```cpp
// For each unique OMM entry, rasterize the alpha texture at micro-triangle sample points:
for (size_t j = 0; j < ommCount; ++j)
{
    uint32_t tri = sources[j];
    uint8_t* outputData = ommData.data() + ommOffsets[j];
    
    meshopt_opacityMapRasterize(
        outputData, levels[j], ommStates,
        &uv0.x, &uv1.x, &uv2.x,                // UV coordinates for triangle corners
        texture.rgba + 3, 4, texture.width * 4, // alpha channel, pixel stride, row pitch
        texture.width, texture.height);
}
```

**Step 3: Compact** — `meshopt_opacityMapCompact()`
```cpp
// Deduplicate identical OMM entries and replace fully-opaque/transparent with special indices:
size_t compactCount = meshopt_opacityMapCompact(
    ommData.data(), ommData.size(),
    levels.data(), offsets.data(), ommCount,
    ommIndices.data(), triangleCount, ommStates);
```

**Step 4: Feed into NVRHI**
```cpp
// Upload ommData + ommDescs to GPU buffers, then call NVRHI's existing runtime:
NVRHI::buildOpacityMicromap(omm, desc);
// Attach to BLAS via GeometryDesc::Triangles
```

#### Performance

This load-time approach is **very fast** because:
- `meshopt_opacityMapMeasure` efficiently deduplicates triangles with identical UV regions, minimizing rasterization work
- `meshopt_opacityMapRasterize` uses bilinear filtering for high-quality results with minimal samples
- `meshopt_opacityMapCompact` further reduces data size by merging identical entries
- For a mesh with 10K alpha-masked triangles at subdiv level 3 (64 micro-triangles each): done in milliseconds on CPU

#### Additional Considerations

- The OMM APIs are marked `MESHOPTIMIZER_EXPERIMENTAL` — the interface may evolve but is already proven in [zeux/niagara](https://github.com/zeux/niagara)
- Triangle indices must be normalized before passing to `meshopt_opacityMapMeasure` (see niagara's `normalizeIndicesForOMM()` which rotates each triangle so the smallest index is first)
- Supports both 2-state (`OC1_2_State`) and 4-state (`OC1_4_State`) formats
- For 2-state, meshes whose triangles are all fully opaque can be skipped entirely (set `OMM_INDEX_BUFFER_ENTRY_OPAQUE` = -1)

#### What You Give Up vs a Full Offline Baker

Skipping the heaviest optimization passes means:
- No per-triangle format optimization → manual format selection
- No Z-order optimization → potentially worse cache behavior during traversal

For most use cases (especially prototyping and small-to-medium content), the memory and performance cost is negligible. meshoptimizer's compaction pass (`meshopt_opacityMapCompact`) handles the most impactful optimization (near-duplicate detection) automatically.

---

## 3. End-to-End OMM Pipeline

```
┌─────────────────────────────────────────────────────────────────────┐
│                    LOAD-TIME OMM GENERATION                          │
│                                                                      │
│  [Source Mesh]  +  [Opacity Texture(s)]                             │
│         │                   │                                        │
│         └────────┬──────────┘                                        │
│                  ▼                                                   │
│     ┌─────────────────────────────┐                                 │
│     │  meshoptimizer OMM Pipeline │  ← Already in external/         │
│     │  1. opacityMapMeasure()     │    meshoptimizer v1.2           │
│     │  2. opacityMapRasterize()   │                                  │
│     │  3. opacityMapCompact()     │    (see niagara reference:       │
│     │  4. normalizeIndicesForOMM()│     REFERENCES/niagara/src/      │
│     └──────────┬──────────────────┘     scene.cpp:buildSceneOmm())   │
│                │                                                     │
│                ▼                                                     │
│     ┌─────────────────────────────┐                                 │
│     │  OMM Raw Data + Desc Arrays │  (in-memory, then GPU upload)   │
│     └──────────┬──────────────────┘                                  │
│                │                                                     │
└────────────────┼─────────────────────────────────────────────────────┘
                 │
                 │ (upload to GPU buffers)
                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         RUNTIME / GPU                                │
│                                                                      │
│  1. Upload OMM data to GPU buffers (IBuffer)                        │
│                 │                                                     │
│                 ▼                                                     │
│  2. NVRHI::createOpacityMicromap(desc)     ← Already implemented     │
│     → Allocates GPU buffer for built OMM array                       │
│                 │                                                     │
│                 ▼                                                     │
│  3. NVRHI::buildOpacityMicromap(omm, desc) ← Already implemented     │
│     → GPU builds OMM array from input data                           │
│                 │                                                     │
│                 ▼                                                     │
│  4. Attach to BLAS geometry via GeometryDesc::Triangles:             │
│     • opacityMicromap  = ommHandle                                   │
│     • ommIndexBuffer   = per-triangle OMM index buffer               │
│     • ommIndexFormat   = R16_UINT or R32_UINT                        │
│                 │                                                     │
│                 ▼                                                     │
│  5. Ray Tracing Dispatch: GPU uses OMM to skip transparent          │
│     micro-triangles → fewer AnyHit shader invocations                │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 4. What You Need to Do

### 4.1 DO: Integrate meshoptimizer's OMM Pipeline

Write the glue code that connects meshoptimizer's OMM baking APIs to your NVRHI runtime. The reference implementation is in `REFERENCES/niagara/src/scene.cpp:buildSceneOmm()` — study it for the exact integration pattern.

**Steps:**
1. **Normalize triangle indices** — rotate each triangle so the smallest vertex index is first (see niagara's `normalizeIndicesForOMM()`). This is required for correct deduplication in `meshopt_opacityMapMeasure`.
2. **Resolve mesh→texture mapping** — for each alpha-masked mesh, determine which opacity texture to use. In niagara, this is done by looking up `Material::albedoTexture` for draws with `postPass == 1` (transparent).
3. **Call `meshopt_opacityMapMeasure()`** — computes per-triangle subdivision levels and deduplicates identical UV regions. Returns the number of unique OMM entries to rasterize.
4. **Call `meshopt_opacityMapRasterize()`** — for each unique entry, rasterizes the alpha texture at micro-triangle sample points using bilinear filtering.
5. **Call `meshopt_opacityMapCompact()`** — merges identical OMM entries and replaces fully-opaque/transparent entries with special indices (-4..-1).
6. **Upload to GPU** — copy `ommData`, `ommDescs`, and `ommIndices` to GPU buffers.
7. **Feed into NVRHI** — call `createOpacityMicromap()` → `buildOpacityMicromap()` → attach to BLAS via `GeometryDesc::Triangles`.
8. **Shader side** — ensure your ray tracing hit shaders are aware of OMM (DXR/Vulkan handles this transparently for `D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE` geometry; for non-opaque geometry with OMM, the system automatically invokes AnyHit shaders only for opaque/unknown micro-triangles).

**Reference implementation**: `REFERENCES/niagara/src/scene.cpp:buildSceneOmm()` (~200 lines of integration code).

**No new dependencies. No submodules. No custom baking code.**

### 4.2 ALREADY HAVE: Runtime OMM in NVRHI

Your NVRHI already supports:
- Creating OMM arrays (`createOpacityMicromap`)
- Building OMMs on GPU (`buildOpacityMicromap`)
- Attaching OMMs to BLAS geometry (via `GeometryDesc::Triangles`)
- Both D3D12 (Agility SDK + NVAPI) and Vulkan backends

**What you still need to write** in your engine code:
- Asset loading integration that calls the meshoptimizer OMM pipeline at load time
- Pipeline that: bakes OMM data → uploads to GPU → calls `buildOpacityMicromap()` → attaches to BLAS → builds BLAS → uses in ray tracing
- Shader side: ensure your ray tracing hit shaders are aware of OMM (DXR/Vulkan handles this transparently for `D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE` geometry; for non-opaque geometry with OMM, the system automatically invokes AnyHit shaders only for opaque/unknown micro-triangles)

### 4.3 NOT NEEDED: Write Custom OMM Baker

meshoptimizer v1.2 already provides the complete OMM baking pipeline. The previous recommendation to write a ~300-line `BakeOmmData()` function is **no longer necessary** — use `meshopt_opacityMap*` APIs instead. Your existing meshoptimizer v1.2 includes everything needed.

---

## 5. Comparison Matrix

| Feature | meshoptimizer v1.2 | NVRHI (your version) |
|---|---|---|
| OMM Generation (CPU) | ✅ Full (`opacityMapMeasure` / `opacityMapRasterize` / `opacityMapCompact`) | ❌ None |
| OMM GPU Build | ❌ (uses NVRHI) | ✅ Full |
| OMM BLAS Attachment | ❌ (uses NVRHI) | ✅ Full |
| External Dependency | 🟢 Already present | 🟢 Already present |
| Code Footprint | ~200 LOC integration glue | N/A |
| Generation Speed | 🟢 Very fast (ms) | N/A |
| Near-Duplicate Detection | ✅ (`meshopt_opacityMapCompact`) | ❌ |
| Per-Triangle Format Sel. | ✅ (`meshopt_opacityMapMeasure` auto-selects level) | ❌ |
| Budget Management | ❌ (manual) | ❌ |
| Meshlet Clustering | ✅ Full | ❌ N/A |
| Vertex Optimization | ✅ Full | ❌ N/A |
| Tangent Generation | ✅ (`meshopt_generateTangents`) | ❌ |
| D3D12 Agility SDK OMM | ❌ | ✅ |
| NVAPI OMM (Ada+) | ❌ | ✅ |
| Vulkan OMM (VK_EXT) | ❌ | ✅ |

---

## 6. Recommendations

1. **Use meshoptimizer's built-in OMM APIs** — `meshopt_opacityMapMeasure` → `meshopt_opacityMapRasterize` → `meshopt_opacityMapCompact`. Study `REFERENCES/niagara/src/scene.cpp:buildSceneOmm()` for the exact integration pattern (~200 LOC of glue code). This avoids any custom OMM baking code and leverages the same pipeline used by the [zeux/niagara](https://github.com/zeux/niagara) reference renderer.
2. **Keep your current meshoptimizer v1.2** — no upgrade needed, it already includes the OMM baking functions. The meshlet clustering and vertex optimization functions remain valuable for geometry preprocessing.
3. **Your NVRHI is ready** for runtime OMM — no changes needed.
4. **Start with 2-state format** (`ommStates = 2`, `OC1_2_State`) — 1 bit per micro-triangle (opaque/transparent), simplest to implement and sufficient for most alpha-masked content.
5. **Begin with a fixed max subdivision level** (e.g., N=6 → 4096 micro-triangles max, matching niagara's `kOmmSubdivisionLevel`) with adaptive subdivision via `target_edge`. Tune per-mesh later if needed.
6. **Note: OMM APIs are EXPERIMENTAL** — the `meshopt_opacityMap*` functions are behind `MESHOPTIMIZER_EXPERIMENTAL`. They are proven in [zeux/niagara](https://github.com/zeux/niagara) but the API surface may evolve in future meshoptimizer releases. Pin your integration against v1.2 and plan to update when upgrading meshoptimizer.

---

## 7. References

- [zeux/niagara — Vulkan renderer with load-time OMM generation](https://github.com/zeux/niagara)
  - Local reference: `REFERENCES/niagara/src/scene.cpp:buildSceneOmm()` — end-to-end OMM baking using meshoptimizer APIs
  - Local reference: `REFERENCES/niagara/src/scene.h` — OMM data structures (`Geometry::ommData`, `ommIndices`, `ommDescs`)
  - Local reference: `REFERENCES/niagara/src/scenert.h` — Runtime OMM (`buildOMM()`, `buildBLAS()` with OMM attachment)
- [meshoptimizer on GitHub](https://github.com/zeux/meshoptimizer)
  - `meshopt_opacityMapMeasure` / `meshopt_opacityMapRasterize` / `meshopt_opacityMapCompact` in `external/meshoptimizer/src/meshoptimizer.h` (lines ~895-925)
- [NVIDIA Micro-Mesh Developer Page](https://developer.nvidia.com/rtx/ray-tracing/micro-mesh)
- NVRHI OMM implementation: `external/nvrhi/src/d3d12/d3d12-raytracing.cpp`
- NVRHI OMM interface: `external/nvrhi/include/nvrhi/nvrhi.h` (lines 1377-1445)
- NVRHI OMM backend: `external/nvrhi/src/d3d12/d3d12-backend.h` (lines 813-829)
