# Scene Cache Feature — Git History Analysis & Reimplementation Guide

> **Generated**: 2026-07-01  
> **Purpose**: Analyze the removed scene binary cache feature to extract the mesh-cooking portion for partial reimplementation.

---

## 1. Git History Summary

The scene cache feature lived across several commits and was ultimately removed:

| Commit | Description |
|--------|-------------|
| `c2c20a2` | **Moved cache logic from `Scene.cpp` into new `SceneCache.cpp`** (340 lines extracted, 345 lines added) |
| `ad3f993` | **Introduced meshopt quantization pipeline** — vertex normal (10-10-10-2 snorm), UV (half2), tangent (8-8 octahedral). Cache version bumped to 12. |
| `45181b8` | Bumped meshopt version; cache version → 13 |
| `de6ab57` | Bumped cache version → 27 for emissive strength |
| `e2591c6` | Minor debug UI fix |
| `78a02bc` | **Removed the entire scene cache feature** (deleted `SceneCache.cpp`, removed `LoadFromCache`/`SaveToCache`, removed `--skip-cache` CLI flag) |

### Key files in the removal commit (`78a02bc`):

| File | Change |
|------|--------|
| `src/SceneCache.cpp` | **Deleted** (432 lines) |
| `src/Scene.h` | Removed `LoadFromCache`/`SaveToCache` declarations |
| `src/Scene.cpp` | Simplified `LoadScene()` — now always loads via `SceneLoader` |
| `src/Config.cpp` / `src/Config.h` | Removed `m_SkipCache` / `--skip-cache` |
| `src/Tests/*` | Removed cache-related test parameters |

---

## 2. How the Original Cache Worked

### 2.1 Integration Flow (from removed `Scene::LoadScene()`)

```
┌─────────────────────────────────────────────────────┐
│                  Scene::LoadScene()                  │
├─────────────────────────────────────────────────────┤
│                                                     │
│  1. Check if <scene>_cooked.bin exists              │
│     AND is newer than source .gltf                  │
│                                                     │
│     ┌─── YES ──────────────────────────────────┐    │
│     │ LoadFromCache(cachePath, allIndices,      │    │
│     │               allVerticesQuantized)       │    │
│     │ → Restores ALL scene data from binary     │    │
│     └───────────────────────────────────────────┘    │
│                                                     │
│     ┌─── NO / FAIL ────────────────────────────┐    │
│     │ SceneLoader::LoadGLTFScene(...)           │    │
│     │ → Parses .gltf → ProcessMeshes()         │    │
│     │   (meshopt pipeline)                     │    │
│     │ SaveToCache(cachePath, allIndices,        │    │
│     │             allVerticesQuantized)         │    │
│     └───────────────────────────────────────────┘    │
│                                                     │
│  2. FinalizeLoadedScene()                           │
│  3. LoadTexturesFromImages()                        │
│  4. UpdateMaterialsAndCreateConstants()             │
│  5. CreateAndUploadGpuBuffers()                     │
│  6. CreateAndUploadLightBuffer()                    │
│  7. BuildAccelerationStructures()                   │
│                                                     │
└─────────────────────────────────────────────────────┘
```

### 2.2 What the Cache Stored (ALL-or-nothing)

The original cache serialized **the entire scene** — meshes, nodes, materials, textures, cameras, lights, animations, and all mesh data. There was **no partial caching**.

#### Cache Binary Format (version 30, magic `0x59464C52`):

```
[kSceneCacheMagic]       uint32_t  (4 bytes)
[kSceneCacheVersion]     uint32_t  (4 bytes)
[m_Meshes count + data]           // Mesh structs with Primitive offsets
[m_Nodes count + data]            // Hierarchy, transforms, bounds
[m_Materials count + data]        // All material properties
[m_Textures count + data]         // URIs + sampler types
[m_Cameras count + data]          // Projection, exposure
[m_Lights count + data]           // Type, color, intensity etc.
[m_MeshData]                      // LOD info per primitive
[m_Meshlets]                      // Meshlet descriptors
[m_MeshletVertices.size]          // Count
[m_MeshletTriangles.size]         // Count
[Compressed meshlet data]         // meshopt_encodeMeshlet per meshlet
[allIndices]                      // Index buffer
[m_VerticesQuantized]             // Quantized vertex buffer
[m_Animations count + data]       // Channels, samplers
```

---

## 3. The MeshOptimizer Pipeline (the expensive part to cache)

This is the core processing inside `SceneLoader::ProcessMeshes()` that you want to skip on subsequent loads. All of this runs **per primitive** in parallel via the task scheduler.

### 3.1 Step-by-step MeshOptimizer Call Chain

```cpp
// ── STEP 1: Read raw glTF data into Vertex struct ──
std::vector<srrhi::Vertex> rawVertices(vertCount);
// Fills: m_Pos, m_Normal, m_Uv, m_Tangent (float3, float3, float2, float4)
// Also applies RH→LH coordinate conversion (negate Z)

// ── STEP 1b: Filter degenerate/duplicate triangles (NEW in v1.2) ──
rawIndices.resize(meshopt_filterIndexBuffer(rawIndices.data(), rawIndices.data(),
    rawIndices.size(), &rawVertices[0].m_Pos.x, rawVertices.size(),
    sizeof(float) * 3, sizeof(srrhi::Vertex)));
// Removes triangles where any two vertices have the same position (degenerate)
// or triangles that are exact duplicates of earlier triangles (same winding + positions)

// ── STEP 1c: Generate tangents if not present in glTF (NEW in v1.2) ──
if (!tangAcc && normAcc && uvAcc)
{
    std::vector<float> tangents(rawIndices.size() * 4);
    meshopt_generateTangents(tangents.data(), rawIndices.data(), rawIndices.size(),
        &rawVertices[0].m_Pos.x, rawVertices.size(), sizeof(srrhi::Vertex),
        &rawVertices[0].m_Normal.x, sizeof(srrhi::Vertex),
        &rawVertices[0].m_Uv.x, sizeof(srrhi::Vertex), 0);

    // Per-corner tangents require vertex splitting at UV seams
    // (see meshoptimizer README: Tangent spaces section for the splitting algorithm)
    // ... vertex splitting loop using splits[] chain ...
}
// When tangents exist in glTF, they are read directly from the TANGENT accessor

// ── STEP 2: Generate vertex remap (deduplication) ──
std::vector<uint32_t> remap(rawIndices.size());
size_t uniqueVertices = meshopt_generateVertexRemap(
    remap.data(), rawIndices.data(), rawIndices.size(),
    rawVertices.data(), rawVertices.size(), sizeof(srrhi::Vertex));

// ── STEP 3: Remap vertex and index buffers ──
std::vector<srrhi::Vertex> optimizedVertices(uniqueVertices);
std::vector<uint32_t> localIndices(rawIndices.size());
meshopt_remapVertexBuffer(optimizedVertices.data(), rawVertices.data(),
    rawVertices.size(), sizeof(srrhi::Vertex), remap.data());
meshopt_remapIndexBuffer(localIndices.data(), rawIndices.data(),
    rawIndices.size(), remap.data());

// ── STEP 4: Optimize cache coherence & vertex fetch ──
meshopt_optimizeVertexCache(localIndices.data(), localIndices.data(),
    localIndices.size(), uniqueVertices);
meshopt_optimizeVertexFetch(optimizedVertices.data(), localIndices.data(),
    localIndices.size(), optimizedVertices.data(), uniqueVertices,
    sizeof(srrhi::Vertex));

// ── STEP 5: Quantize vertices ──
srrhi::VertexQuantized vq;
vq.m_Pos = v.m_Pos;           // float3 — kept as-is
vq.m_Normal =                  // 10-10-10-2 signed normalized (uint32_t)
    (meshopt_quantizeSnorm(v.m_Normal.x, 10) + 511) |
    ((meshopt_quantizeSnorm(v.m_Normal.y, 10) + 511) << 10) |
    ((meshopt_quantizeSnorm(v.m_Normal.z, 10) + 511) << 20);
vq.m_Normal |= (v.m_Tangent.w >= 0 ? 0 : 1) << 30;  // bit 30 = bitangent sign
vq.m_Uv =                       // two half-precision floats (uint32_t)
    (meshopt_quantizeHalf(v.m_Uv.x)) |
    ((meshopt_quantizeHalf(v.m_Uv.y)) << 16);
vq.m_Tangent =                  // 8-8 octahedral encoding (uint32_t)
    octahedral_encode(v.m_Tangent.xyz);   // custom math, not meshopt

// ── STEP 6: LOD generation (per LOD level) ──
for (uint32_t lod = 0; lod < MAX_LOD_COUNT; ++lod) {
    // LOD 0 = full detail; LOD 1+ = simplified
    const float simplifyScale = meshopt_simplifyScale(
        &optimizedVertices[0].m_Pos.x, uniqueVertices, sizeof(srrhi::Vertex));

    size_t new_index_count = meshopt_simplifyWithAttributes(
        lodIndices.data(),
        currentLodIndices.data(), prevIndexCount,
        &optimizedVertices[0].m_Pos.x, uniqueVertices, sizeof(srrhi::Vertex),
        &optimizedVertices[0].m_Normal.x, sizeof(srrhi::Vertex),
        attribute_weights, 3,
        nullptr, target_index_count, kMaxError,
        meshopt_SimplifySparse, &lodError);

    meshopt_optimizeVertexCache(lodIndices.data(), lodIndices.data(),
        lodIndices.size(), uniqueVertices);

    // ── STEP 7: Build meshlets for this LOD ──
    size_t max_meshlets = meshopt_buildMeshletsBound(
        lodIndices.size(), max_vertices, max_triangles);

    const size_t meshlet_count = meshopt_buildMeshlets(
        localMeshlets.data(), meshlet_vertices.data(), meshlet_triangles.data(),
        lodIndices.data(), lodIndices.size(),
        &optimizedVertices[0].m_Pos.x, uniqueVertices, sizeof(srrhi::Vertex),
        max_vertices, max_triangles, cone_weight);

    // ── STEP 8: Optimize & compute bounds per meshlet ──
    for each meshlet:
        meshopt_optimizeMeshlet(&meshlet_vertices[vo], &meshlet_triangles[to],
            triangle_count, vertex_count);

        meshopt_Bounds bounds = meshopt_computeMeshletBounds(
            &meshlet_vertices[vo], &meshlet_triangles[to],
            triangle_count, &optimizedVertices[0].m_Pos.x,
            uniqueVertices, sizeof(srrhi::Vertex));

        // Pack bounds into GPU-friendly format:
        gpuMeshlet.m_CenterRadius[0] = meshopt_quantizeHalf(bounds.center[0])
                                     | (meshopt_quantizeHalf(bounds.center[1]) << 16);
        gpuMeshlet.m_CenterRadius[1] = meshopt_quantizeHalf(bounds.center[2])
                                     | (meshopt_quantizeHalf(bounds.radius) << 16);
        // Cone axis: 8-bit per component, cutoff: 8-bit snorm
```

### 3.2 MeshOptimizer Functions Used (Summary)

| Function | Purpose |
|----------|---------|
| `meshopt_filterIndexBuffer` | Remove degenerate & duplicate triangles **(NEW in v1.2)** |
| `meshopt_generateTangents` | Generate tangent vectors from positions, normals, UVs **(NEW in v1.2)** |
| `meshopt_generateVertexRemap` | Deduplicate vertices by value |
| `meshopt_remapVertexBuffer` | Apply remap to vertex array |
| `meshopt_remapIndexBuffer` | Apply remap to index array |
| `meshopt_optimizeVertexCache` | Reorder indices for GPU vertex cache |
| `meshopt_optimizeVertexFetch` | Reorder vertices for memory locality |
| `meshopt_quantizeSnorm` | Quantize float to N-bit signed normalized int |
| `meshopt_quantizeHalf` | Quantize float to half-precision (float16) |
| `meshopt_simplifyScale` | Compute error scaling factor |
| `meshopt_simplifyWithAttributes` | Generate simplified LOD mesh |
| `meshopt_buildMeshlets` | Partition mesh into meshlets |
| `meshopt_buildMeshletsBound` | Upper bound on meshlet count |
| `meshopt_optimizeMeshlet` | Optimize meshlet vertex/triangle order |
| `meshopt_computeMeshletBounds` | Compute bounding sphere + normal cone |
| `meshopt_encodeMeshlet` | Compress meshlet data (used in old cache only) |
| `meshopt_decodeMeshlet` | Decompress meshlet data (used in old cache only) |

---

## 4. Data Structures for the "Cooked Mesh" Cache

### 4.1 What to Cache (Mesh-Only Data)

These are the final CPU arrays that go directly to GPU buffer creation — **no further processing needed**:

```
┌─────────────────────────────────────────────────────────────────┐
│  COOKED MESH FILE FORMAT (proposed)                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  [Header]                                                       │
│    uint32_t magic         = 0x59464C52 ("RLFY") or new magic    │
│    uint32_t version       = 1                                   │
│                                                                 │
│  [Mesh Primitives] — needed for correct offsets/indices         │
│    uint32_t meshCount                                           │
│    for each Mesh:                                               │
│      uint32_t primCount                                         │
│      for each Primitive:                                        │
│        uint32_t m_VertexOffset   (into allVerticesQuantized)    │
│        uint32_t m_VertexCount                                   │
│        int32_t  m_MaterialIndex  (-1 = none)                    │
│        uint32_t m_MeshDataIndex  (into m_MeshData)              │
│      Vector3 m_Center                                           │
│      float   m_Radius                                           │
│                                                                 │
│  [Mesh Data] — LOD info per primitive                           │
│    writeVector(m_MeshData)     // std::vector<srrhi::MeshData>  │
│                                                                 │
│  [Meshlets]                                                     │
│    writeVector(m_Meshlets)     // std::vector<srrhi::Meshlet>   │
│                                                                 │
│  [Meshlet Vertices]                                             │
│    writeVector(m_MeshletVertices)  // std::vector<uint32_t>     │
│                                                                 │
│  [Meshlet Triangles]                                            │
│    writeVector(m_MeshletTriangles) // std::vector<uint32_t>     │
│                                                                 │
│  [Indices]                                                      │
│    writeVector(allIndices)       // std::vector<uint32_t>       │
│                                                                 │
│  [Vertices Quantized]                                           │
│    writeVector(m_VerticesQuantized / allVerticesQuantized)      │
│    // std::vector<srrhi::VertexQuantized>                       │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 4.2 What to Load from glTF (Always)

These are loaded from the `.gltf`/`.glb` file every time:

- **Nodes** — hierarchy, world/local transforms, parent/child relationships, animation flags
- **Materials** — factors, texture indices, alpha modes, IOR, transmission, volume params
- **Textures** — URIs, sampler types
- **Cameras** — projection, exposure
- **Lights** — type, color, intensity, range, angles
- **Animations** — channels, samplers, keyframes
- **Node bounding spheres** — recalculated from mesh bounds after loading

---

## 5. Current (Post-Removal) Code Structure

### 5.1 `Scene::LoadScene()` — [Scene.cpp](d:\Workspace\HobbyRenderer\src\Scene.cpp)

```cpp
void Scene::LoadScene()
{
    std::vector<srrhi::VertexQuantized> allVerticesQuantized;
    std::vector<uint32_t> allIndices;

    if (bIsSceneJson)
        SceneLoader::LoadJSONScene(*this, scenePath, allVerticesQuantized, allIndices);
    else
        SceneLoader::LoadGLTFScene(*this, scenePath, allVerticesQuantized, allIndices, false);

    FinalizeLoadedScene();
    SceneLoader::LoadTexturesFromImages(*this, sceneDir);
    SceneLoader::UpdateMaterialsAndCreateConstants(*this);
    SceneLoader::CreateAndUploadGpuBuffers(*this, allVerticesQuantized, allIndices);
    SceneLoader::CreateAndUploadLightBuffer(*this);
    BuildAccelerationStructures();
}
```

### 5.2 `SceneLoader::LoadGLTFScene()` — [SceneLoader.cpp](d:\Workspace\HobbyRenderer\src\SceneLoader.cpp)

The flow inside `LoadGLTFScene` (via `ProcessParsedGLTF`):

```
cgltf_parse → decompressMeshopt → ProcessMaterialsAndImages
→ ProcessCameras → ProcessLights → ProcessAnimations
→ ProcessMeshes(data, scene, outVerticesQuantized, outIndices, offsets)  ← THE EXPENSIVE STEP
→ ProcessNodesAndHierarchy
```

### 5.3 GPU Buffer Creation — [SceneLoader.cpp](d:\Workspace\HobbyRenderer\src\SceneLoader.cpp)

`CreateAndUploadGpuBuffers()` takes `allVerticesQuantized` and `allIndices` plus scene members (`m_MeshData`, `m_Meshlets`, `m_MeshletVertices`, `m_MeshletTriangles`) and creates:

- `m_VertexBufferQuantized` — from `allVerticesQuantized`
- `m_IndexBuffer` — from `allIndices`
- `m_MeshDataBuffer` — from `m_MeshData`
- `m_MeshletBuffer` — from `m_Meshlets`
- `m_MeshletVerticesBuffer` — from `m_MeshletVertices`
- `m_MeshletTrianglesBuffer` — from `m_MeshletTriangles`

---

## 6. Proposed Reimplementation Strategy

### 6.1 New Partial Cooked-Mesh File

Create a new file (e.g., `<scene>_mesh.bin`) that caches **only** the mesh-related GPU data. Keep all other scene data flowing from glTF.

### 6.2 Modified `Scene::LoadScene()` Flow

```
┌──────────────────────────────────────────────────────────────┐
│  Scene::LoadScene()  (modified)                              │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  // Always parse glTF for materials, nodes, textures, etc.   │
│  SceneLoader::LoadGLTFScene_MaterialsNodesOnly(...)          │
│                                                              │
│  // Try cooked mesh cache for mesh data                      │
│  if (cooked_mesh_file exists && newer than gltf)             │
│      LoadCookedMesh(cookedPath, allIndices, allVertsQ,       │
│                     m_MeshData, m_Meshlets,                  │
│                     m_MeshletVertices, m_MeshletTriangles,   │
│                     m_Meshes)                                │
│  else                                                        │
│      SceneLoader::ProcessMeshesOnly(...)                     │
│      SaveCookedMesh(cookedPath, ...)                         │
│                                                              │
│  // Proceed as normal (GPU uploads need the cooked data)     │
│  FinalizeLoadedScene()                                       │
│  ...                                                         │
└──────────────────────────────────────────────────────────────┘
```

### 6.3 Data That Must Be Consistent

When caching only meshes, you must also cache `m_Meshes` (with their `Primitive` offsets) and `m_MeshData` because they contain cross-references into the vertex/index/meshlet arrays:

- `Primitive::m_VertexOffset` → index into `allVerticesQuantized`
- `Primitive::m_VertexCount` → count of vertices
- `Primitive::m_MeshDataIndex` → index into `m_MeshData`
- `MeshData::m_IndexOffsets[lod]` → offset into `allIndices`
- `MeshData::m_MeshletOffsets[lod]` → offset into `m_Meshlets`
- `Meshlet::m_VertexOffset` → offset into `m_MeshletVertices`
- `Meshlet::m_TriangleOffset` → offset into `m_MeshletTriangles`

If any of these mismatch, rendering will be corrupted.

### 6.4 Key Concern: Material Index Mapping

`Primitive::m_MaterialIndex` is an index into `m_Materials`. When loading materials from glTF separately, the material order/indices must be **identical** to when the mesh was cooked. If material order changes (e.g., due to different glTF processing), indices will be wrong. This means:

- The mesh cooking MUST happen after materials are processed (to know the correct indices)
- Or, the cooking must store the material index for each primitive

Since the current approach already processes materials before meshes (`ProcessMaterialsAndImages` runs before `ProcessMeshes` in `ProcessParsedGLTF`), this is already the case — the material index captured during cooking will match the glTF load as long as the glTF file hasn't changed.

### 6.5 Dependency & Build Integration

meshoptimizer is already linked via CMake:

```cmake
# CMakeLists.txt
add_subdirectory(external/meshoptimizer)
target_link_libraries(${PROJECT_NAME} PRIVATE meshoptimizer)
target_include_directories(${PROJECT_NAME} PRIVATE 
    "${CMAKE_SOURCE_DIR}/external/meshoptimizer/src")
```

Version: **meshoptimizer v1.2** (from `external/meshoptimizer/CMakeLists.txt`)

### 6.6 Validation Strategy

To ensure the cooked mesh is valid vs. the source glTF:

- **File timestamp check**: Compare `last_write_time` of `.gltf`/`.glb` vs cooked `.bin`
- **Optional hash check**: Store a hash of relevant glTF binary buffer data in the cooked file header
- **Version bump**: Use a `kCookedMeshVersion` constant; increment when ProcessMeshes logic changes

---

## 7. Summary of Key Decisions

| Decision | Recommendation |
|----------|---------------|
| Cache scope | Mesh-only: vertices, indices, meshlets, meshlet vertices/triangles, mesh data, primitive offsets |
| Always from glTF | Materials, nodes, textures, cameras, lights, animations |
| File naming | `<scene_stem>_cooked.bin` (or `_mesh.bin` to distinguish from old cache) |
| Validation | Timestamp comparison + version number |
| Meshlet compression | No need for `meshopt_encodeMeshlet` in new cache — store raw arrays directly (simpler, and the data goes straight to GPU anyway) |
| Where to hook in | In `Scene::LoadScene()`, between glTF load and `FinalizeLoadedScene()` |

---

## 8. Old Cache Serialization Helpers (Reference)

These templates from the deleted `SceneCache.cpp` are useful reference for binary I/O:

```cpp
template<typename T>
static void WritePOD(std::ostream& os, const T& value) {
    os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template<typename T>
static void ReadPOD(std::istream& is, T& value) {
    is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template<typename T>
static void WriteVector(std::ostream& os, const std::vector<T>& vec) {
    size_t size = vec.size();
    WritePOD(os, size);
    if (size > 0)
        os.write(reinterpret_cast<const char*>(vec.data()), size * sizeof(T));
}

template<typename T>
static void ReadVector(std::istream& is, std::vector<T>& vec) {
    size_t size;
    ReadPOD(is, size);
    vec.resize(size);
    if (size > 0)
        is.read(reinterpret_cast<char*>(vec.data()), size * sizeof(T));
}
```
