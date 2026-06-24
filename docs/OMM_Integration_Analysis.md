# OMM (Opacity Micro-Maps) Integration Analysis for HobbyRenderer

> **Date**: 2026-06-24
> **Scope**: Full analysis of OMM generation and runtime usage — load-time generation inspired by [zeux/niagara](https://github.com/zeux/niagara), plus NVRHI native runtime support

---

## 1. Executive Summary

**Your NVRHI already handles runtime OMM fully. For OMM data generation, use a minimal load-time rasterization approach — no fat submodule required.**

| Concern | What provides it | Status in your codebase |
|---|---|---|
| **OMM Generation** (load-time CPU) | Custom `BakeOmmData()` function (~300 LOC), inspired by [zeux/niagara](https://github.com/zeux/niagara) | ❌ Not yet written |
| **OMM Runtime** (GPU build, BLAS attachment) | Your NVRHI (`external/nvrhi`) | ✅ Fully implemented |
| **meshoptimizer** (meshlet clustering, compression, etc.) | meshoptimizer v1.0 (`external/meshoptimizer`) | ✅ Already present, but **no OMM** |

**Key insight**: The OMM binary data format is very simple — 1 bit per micro-triangle encoding opaque/transparent states. You can generate this at load time by rasterizing opacity textures at micro-triangle sample points, just as [zeux/niagara](https://github.com/zeux/niagara) does. This approach is **~200-400 lines of self-contained C++** with zero external dependencies, and runs in milliseconds for typical asset loads.

---

## 2. Detailed Analysis

### 2.1 meshoptimizer — Not an OMM Baker, but Helpful

- **Repo**: [https://github.com/zeux/meshoptimizer](https://github.com/zeux/meshoptimizer)
- **Your version**: `v1.0` (`MESHOPTIMIZER_VERSION 1000`)
- **License**: MIT

#### What it does have:

meshoptimizer is a mesh optimization library focused on GPU rendering efficiency. It provides:

| Category | Functions |
|---|---|
| Vertex cache optimization | `meshopt_optimizeVertexCache`, `meshopt_optimizeVertexCacheFifo` |
| Overdraw optimization | `meshopt_optimizeOverdraw` |
| Vertex fetch optimization | `meshopt_optimizeVertexFetch`, `meshopt_optimizeVertexFetchRemap` |
| Mesh simplification | `meshopt_simplify`, `meshopt_simplifyWithAttributes`, `meshopt_simplifySloppy` |
| Meshlet/cluster building | `meshopt_buildMeshlets`, `meshopt_buildMeshletsScan`, `meshopt_buildMeshletsSpatial` |
| Index/vertex compression | `meshopt_encodeIndexBuffer`, `meshopt_encodeVertexBuffer` |
| Spatial sorting | `meshopt_spatialSortRemap`, `meshopt_spatialSortTriangles` |
| Stripification | `meshopt_stripify`, `meshopt_unstripify` |
| Bounds computation | `meshopt_computeClusterBounds`, `meshopt_computeMeshletBounds` |

#### What it does NOT have:

- ❌ **No OMM baking/generation** — there is no `meshopt_buildOpacityMicromap` or equivalent function
- ❌ **No OMM-related data structures** in the header at all
- ❌ **No opacity/alpha texture processing** for micro-map generation

#### How meshoptimizer helps in an OMM pipeline:

- **Meshlet clustering**: `meshopt_buildMeshlets()` produces spatial clusters that improve OMM efficiency by grouping related triangles together.
- **Spatial sorting**: `meshopt_spatialSortTriangles()` reorders triangles for better locality, improving OMM bake quality.
- **Vertex optimization**: Reduces vertex shader overhead when rendering the base geometry alongside OMMs.

**Conclusion**: meshoptimizer is a valuable complementary tool. Keep it and use it alongside the load-time OMM generation approach. No upgrade is needed for OMM specifically.

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

### 2.3 Load-Time OMM Generation — The Approach

- **Reference**: [zeux/niagara](https://github.com/zeux/niagara) by Arseny Kapoulkine (same author as meshoptimizer)
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

#### How It Works

OMM data is generated at **asset load time** rather than during an expensive offline baking pass:

1. For each alpha-masked triangle, compute its UV bounds and micro-triangle barycentrics
2. Sample the opacity texture at the center (or multiple points) of each micro-triangle
3. Compare sampled alpha against a threshold (e.g., `alpha < 0.5` → transparent)
4. Pack the results into the OMM data buffer (1 bit per micro-triangle)
5. Create the per-OMM descriptor and usage count histogram
6. Feed everything into `NVRHI::buildOpacityMicromap()`

**This is ~200-400 lines of C++** — zero external dependencies.

#### Minimal Implementation Outline

```cpp
struct OmmBakeInput {
    const float*  positions;      // vertex positions (float3)
    const float*  uvs;            // vertex UVs (float2)
    const uint32_t* indices;      // triangle indices
    uint32_t      triangleCount;
    const uint8_t* alphaTexture;  // RGBA8 or single-channel
    uint32_t      texWidth, texHeight;
    float         alphaThreshold;  // e.g., 0.5f
    uint32_t      subdivisionLevel; // N: 4^N micro-triangles
};

struct OmmBakeOutput {
    std::vector<uint8_t>  ommData;       // packed OMM bits
    std::vector<OmmTriangleDesc> ommDescs; // one per triangle
    std::vector<rt::OpacityMicromapUsageCount> usageCounts;
};

// For each triangle:
//   1. Compute micro-triangle sample points in UV space
//   2. Sample alpha texture at each point
//   3. Pack bits into ommData
//   4. Generate OmmTriangleDesc (subdivision level, format, data offset)

OmmBakeOutput BakeOmmData(const OmmBakeInput& input);
```

#### Performance

This load-time approach is **very fast** because:
- The work is trivial: sample N texture fetches per triangle and pack bits
- For a mesh with 10K alpha-masked triangles at subdiv level 3 (64 micro-triangles each): that's 640K texture samples — done in milliseconds on CPU
- Can be trivially parallelized with SIMD or worker threads
- No expensive optimization passes to slow things down

#### What You Give Up vs a Full Baker

Skipping the heavy optimization passes means:
- No near-duplicate detection → more OMM data, larger GPU memory footprint
- No per-triangle format optimization → manual format selection
- No Z-order optimization → potentially worse cache behavior during traversal

For most use cases (especially prototyping and small-to-medium content), the memory and performance cost is negligible. You can always add these optimizations incrementally if needed.

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
│     │   BakeOmmData() (~300 LOC)  │  ← Write this once              │
│     │   - Rasterize alpha texture │                                  │
│     │   - Pack micro-triangle bits│                                  │
│     └──────────┬──────────────────┘                                  │
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

### 4.1 DO: Implement Load-Time OMM Generation

Write a `BakeOmmData()` function as outlined in Section 2.3. This is the only new code you need — everything else (NVRHI runtime) is already in place.

**Steps:**
1. Write a `BakeOmmData()` function — ~300 lines of C++ doing texture rasterization + bit packing (see Section 2.3 for the API sketch)
2. For each alpha-masked mesh at load time, call it with the mesh geometry + opacity texture
3. Upload the resulting `ommData` and `ommDescs` to GPU buffers
4. Feed into `NVRHI::buildOpacityMicromap()` — your existing runtime path unchanged
5. Attach the built OMM array to your BLAS geometry via the existing `GeometryDesc::Triangles` fields

**Reference implementation**: [zeux/niagara](https://github.com/zeux/niagara) — see its `scenert.cpp` for the load-time OMM generation code.

**No new dependencies. No submodules. No offline baking pipeline.**

### 4.2 ALREADY HAVE: Runtime OMM in NVRHI

Your NVRHI already supports:
- Creating OMM arrays (`createOpacityMicromap`)
- Building OMMs on GPU (`buildOpacityMicromap`)
- Attaching OMMs to BLAS geometry (via `GeometryDesc::Triangles`)
- Both D3D12 (Agility SDK + NVAPI) and Vulkan backends

**What you still need to write** in your engine code:
- Asset loading code for OMM baked data
- Pipeline that: loads OMM data → uploads to GPU → calls `buildOpacityMicromap()` → attaches to BLAS → builds BLAS → uses in ray tracing
- Shader side: ensure your ray tracing hit shaders are aware of OMM (DXR/Vulkan handles this transparently for `D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE` geometry; for non-opaque geometry with OMM, the system automatically invokes AnyHit shaders only for unknown-state micro-triangles)

### 4.3 NOT NEEDED: Upgrade meshoptimizer for OMM

meshoptimizer does not have OMM baking and does not need to be upgraded for OMM purposes. However, upgrading to a newer version is still beneficial for:
- Better simplification (`meshopt_simplifyWithUpdate`, permissive mode)
- Improved meshlet clustering
- General bug fixes and performance improvements

---

## 5. Comparison Matrix

| Feature | Load-Time Gen (Niagara-style) | meshoptimizer | NVRHI (your version) |
|---|---|---|---|
| OMM Generation (CPU) | ✅ Minimal (~300 LOC) | ❌ None | ❌ None |
| OMM GPU Build | ❌ (uses NVRHI) | ❌ None | ✅ Full |
| OMM BLAS Attachment | ❌ (uses NVRHI) | ❌ None | ✅ Full |
| External Dependency | 🟢 None | 🟢 Already present | 🟢 Already present |
| Code Footprint | ~300 LOC | N/A | N/A |
| Generation Speed | 🟢 Very fast (ms) | N/A | N/A |
| Near-Duplicate Detection | ❌ (can add later) | ❌ | ❌ |
| Per-Triangle Format Sel. | ❌ (manual) | ❌ | ❌ |
| Budget Management | ❌ (manual) | ❌ | ❌ |
| Meshlet Clustering | ❌ | ✅ Full | ❌ N/A |
| Vertex Optimization | ❌ | ✅ Full | ❌ N/A |
| D3D12 Agility SDK OMM | ❌ | ❌ | ✅ |
| NVAPI OMM (Ada+) | ❌ | ❌ | ✅ |
| Vulkan OMM (VK_EXT) | ❌ | ❌ | ✅ |

---

## 6. Recommendations

1. **Write a ~300-line `BakeOmmData()` function** inspired by [zeux/niagara](https://github.com/zeux/niagara). This avoids any new submodule, is extremely fast, and gets you OMM working in hours.
2. **Keep your current meshoptimizer** — it's useful for meshlet clustering and vertex optimization to prepare geometry for OMM baking.
3. **Your NVRHI is ready** for runtime OMM — no changes needed.
4. **Use only `OC1_2_State` format** — 1 bit per micro-triangle (opaque/transparent), simplest to implement.
5. **For the load-time baker**, begin with a fixed subdivision level (e.g., N=3 → 64 micro-triangles) for all meshes, then consider per-mesh tuning later.

---

## 7. References

- [zeux/niagara — Vulkan renderer with load-time OMM generation](https://github.com/zeux/niagara)
- [meshoptimizer on GitHub](https://github.com/zeux/meshoptimizer)
- [NVIDIA Micro-Mesh Developer Page](https://developer.nvidia.com/rtx/ray-tracing/micro-mesh)
- NVRHI OMM implementation: `external/nvrhi/src/d3d12/d3d12-raytracing.cpp`
- NVRHI OMM interface: `external/nvrhi/include/nvrhi/nvrhi.h` (lines 1377-1445)
- NVRHI OMM backend: `external/nvrhi/src/d3d12/d3d12-backend.h` (lines 813-829)
