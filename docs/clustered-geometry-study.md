# Clustered geometry: study, analysis and integration plan

Study date: 2026-11-06.
Target: reduce the cost of the Opaque base pass in this renderer by adopting clustered
geometry (cluster LOD, cluster-level culling, geometry streaming) as implemented by two
reference codebases.

Sources of evidence used for this document:

| # | Source | What it gave |
|---|---|---|
| 1 | `D:\Workspace\DirectX-Graphics-Samples\Samples\Desktop\D3D12Raytracing\src\D3D12RaytracingClusteredGeometry` (read in full: readme, `D3D12RaytracingClusteredGeometry.cpp`, `Fill*.hlsl`, `Raytracing.hlsl`, `RaytracingHlslCompat.h`) | The D3D12 API shape for clustered geometry: CLAS, cluster BLAS, cluster templates, GPU-generated indirect args, `ClusterID()` |
| 2 | `D:\Workspace\vk_lod_clusters` (README, all of `docs/`, `shaders/traversal*.glsl`, `shaders/render_raster_clusters*.glsl`, `shaders/shaderio_*.h`, `shaders/culling.glsl`, `src/renderer_raster_clusters_lod.cpp`, `src/renderer_raytrace_clusters_lod.cpp`, `src/scene_cluster_lod.cpp`, `src/scene_streaming*.{hpp,cpp}`) | A complete, GPU-driven cluster-LOD renderer: LOD DAG construction, traversal, culling, streaming, CLAS allocation, BLAS sharing/caching/merging |
| 3 | DXR functional spec part 2 (`microsoft.github.io/DirectX-Specs/d3d/Raytracing2.html`, v0.38) | Normative limits and semantics for CLAS / Cluster BLAS / templates / PTLAS |
| 4 | meshoptimizer README (v1.3) + the copy already vendored in `external/meshoptimizer` | Clusterisation, meshlet bounds/cone culling, meshlet codec, position quantisation, DXR2 COMPRESSED1 helper |
| 5 | PIX capture of source 1 (`bin/captures/cg_sample_frame.wpix`, 30 frames) | What the sample actually submits per frame on this machine |
| 6 | PIX capture of *this* renderer (`bin/captures/bistrox-frame90.wpix`) re-analysed | The measured cost distribution of the current Opaque base pass |
| 7 | `vk_lod_clusters` built with Vulkan SDK 1.4.363.1 and run on this machine (raster + ray tracing), `bin/captures/vklc_profiler.png`, `vklc_profiler_rt.png` | Per-pass GPU timings, scene/traversal/CLAS-BLAS sizes for the reference cluster renderer |

---

## 0. Conclusion up front

1. **The two references solve different halves of the problem.**
   * `D3D12RaytracingClusteredGeometry` is a *ray tracing* sample: it shows how to build and
     maintain acceleration structures out of clusters (CLAS → cluster BLAS → TLAS), including
     per-frame animation via cluster templates. It contains **no rasterization of clusters at all**
     — no mesh shader, no cluster culling, no LOD selection. Its value to us is the API/indirection
     design and the "why" (per-frame BLAS cost), not a rendering technique.
   * `vk_lod_clusters` is a *rasterization + ray tracing* renderer built around cluster LOD: LOD
     DAG construction, GPU traversal with a screen-space error metric, two-pass occlusion culling
     with reject lists, mesh-shader cluster drawing, and a complete geometry streaming system with a
     GPU-driven CLAS allocator. Its value is the whole pipeline design; the API calls are Vulkan and
     have to be re-expressed in D3D12.

2. **DXR2 clustered geometry is not available on this machine** (and is a preview feature in general).
   The sample reports `ClustersAndPTLASSupported: NO` on the RTX 3080 (driver 32.0.16.1692, shader
   model 6.10) and falls back to classic DXR1 BLAS. Our renderer ships Agility SDK 1.620.1 with
   SM 6.8/6.9 shaders, which is below the 1.722-preview + SM 6.10 that CLAS requires. The Vulkan
   equivalent is *not* blocked — `vk_lod_clusters` builds **1 024 cluster BLAS per frame** on the same
   driver (§4.3) — so the obstacle is D3D12's preview-runtime gating, not the hardware. **The
   actionable path for the Opaque base pass is the cluster *rasterization* path (mesh shaders), not
   CLAS.** CLAS becomes a follow-up decision tied to a preview-SDK upgrade.

3. **Our measured bottleneck is the base pass itself, not culling.**
   In `bistrox-frame90.wpix` the `Opaque Renderer` scope is 4.38 (PIX-instrumented) time units, of
   which the `Base Pass Render (Phase 1) - Opaque` DispatchMesh is 4.21 — while all culling work
   (phase 1 + phase 2 + indirect-arg building) totals ~0.10, i.e. **~2.4 %**. 604 M shader
   instructions are executed by the base pass. Culling is already cheap; the geometry the base pass
   *processes* is what has to shrink. That is exactly what per-cluster LOD and cluster-level culling
   buy.

4. **The biggest single win available today is sub-object LOD.** Today LOD is chosen per *instance*
   (`GPUCulling.hlsl` picks one of up to 8 discrete mesh LODs for the whole instance from a
   bounding-sphere distance), and the amplification shader then culls *meshlets* within that LOD.
   A continuous cluster LOD DAG replaces "one LOD per object" with "one LOD per ~32-triangle region",
   which is what removes sub-pixel triangles at object silhouettes and at large-distance ranges.

5. **Recommended order of work** (details in §7): (a) offline cluster-LOD builder reusing the
   already-vendored `external/meshoptimizer` `clusterlod.h`; (b) GPU traversal + cluster culling that
   replaces the per-instance LOD pick; (c) mesh-shader cluster draw into the existing G-buffer;
   (d) two-pass culling with reject lists on the existing HZB; (e) geometry streaming; (f) RT story.

---

## 1. Reference 1 — `D3D12RaytracingClusteredGeometry`

### 1.1 What it is

A DXR sample with two selectable geometry paths for the same scene (`[T]` toggle):

* **clustered**: triangle clusters → CLAS → pooled cluster BLAS → TLAS;
* **traditional**: one classic BLAS per object (DXR1 baseline the sample compares against).

Scene: 8 static objects totalling **334 clusters** (16 384 + 9 216 + … triangles), plus an animated
deforming sphere of **768 clusters / 36 864 triangles**, plus *copies* driven by `[N]` — default
**+80 static and +20 animated** clones. This clone workload is the point of the sample: it makes
per-frame acceleration-structure maintenance the dominant cost, which is what clustered geometry is
supposed to fix.

### 1.2 Capability gate (and the result on this machine)

`QueryClusteredGeometrySupport()` asks for `D3D12_FEATURE_D3D12_OPTIONS_RAYTRACING2` and
`D3D_FEATURE_SHADER_MODEL` via `CheckFeatureSupport1`, and only enables the cluster path when
`ClustersAndPTLASSupported && HighestShaderModel >= 6.10`. Otherwise it locks the sample to the
traditional path and disables `[T]`.

Run on this machine (`%TEMP%\D3D12RaytracingClusteredGeometry.log`):

```
  driver: 32.0.16.1692   VID:10DE  PID:2206   dedicatedVRAM: 10051 MB
  D3D12_RAYTRACING_TIER: 0xC
  ClustersAndPTLASSupported: NO   (hr=0x00000000)
  TemplateInstancePrefersCOMPRESSED1: NO
  Shader Model: 6.10
  -> clustered rendering unavailable; falling back to SM 6.6 traditional BLAS, [T] toggle disabled.
...
[build wall-clock] CLAS=0.000 ms, BLAS=5.242 ms, TLAS=0.047 ms (total 7.563 ms)
[storage] CLAS: 0 bytes  BLAS: 1241344 bytes
[CLAS mem mode=implicit-dest + post-build compact] prebuild_max=0 ... sum_actual=0 ...
```

Consequences for this study:

* the CLAS code paths in the sample **cannot be executed here**; everything in §1.3–§1.5 is a
  source-level reading corroborated by the spec (§1.6), not a capture;
* the sample's PIX capture (§4.1) therefore shows the *traditional* path — which is still a useful
  baseline because it is precisely the cost model clustered geometry attacks.

### 1.3 The CLAS build: three allocation modes, one input description

`BuildSharedClusterTrianglesInputs()` (static helper in `D3D12RaytracingClusteredGeometry.cpp`)
fills the shared `D3D12_RTAS_CLUSTER_LIMITS` + `D3D12_RTAS_CLUSTER_TRIANGLES_INPUTS_DESC`:

| Limit | Value used by the sample | Spec maximum |
|---|---|---|
| `MaxArgCount` | cluster count | — |
| `MaxTriangleCountPerCluster` | per-scene max (32 in this scene) | 256 |
| `MaxUniqueVertexIndexCountPerCluster` | per-scene max | 256 |
| `MaxTotalTriangleCount` | scene total | — |
| `MaxSumOfUniqueVertexIndexCounts` | scene total | — |
| `MaxGeometryIndexValue` | material-region count − 1 | 2^24 − 1 |
| `MaxUniqueGeometryIndexAndFlagsCountPerCluster` | 1 | — |
| `MaxOpacityMicromapIndicesPerCluster` | 0 | — |

plus `VertexFormat` = `FLOAT32_3` or `COMPRESSED1`, `IndexFormat` = `UINT8`, and the union
`MinPositionTruncateBitCount` (float path, 0–23 meaningful) / `MaxCompressedClusterPositionsSize`
(compressed path).

The three modes (`BuildClasImplicit`, `BuildClasGetSizes`, `BuildClasCompact`) differ only in where
results go and how tightly they are packed:

| Mode | GPU work | Host stalls | Memory |
|---|---|---|---|
| `IMPLICIT_DESTINATIONS` | 1 batched `ExecuteIndirectRTASOperations`, driver lays out all CLAS in one buffer | one readback of `ResultSizeArray` (statistics only) | prebuild worst case |
| `GET_SIZES` then `EXPLICIT_DESTINATIONS` | 2 passes (size query + build) | one readback between passes | tight, per-CLAS exact size |
| `IMPLICIT` + `MOVE_CLUSTER_OBJECTS` (compact) | build then move | one readback | tightest possible (`final ≤ GET_SIZES ≤ prebuild`) |

The batched-operation struct is the interesting part; it is what makes this GPU-driven:

```cpp
D3D12_RTAS_BATCHED_OPERATION_DATA batched = {};
batched.BatchResultData          = m_clasResultBuffer->GetGPUVirtualAddress();
batched.BatchScratchData         = m_clasScratchBuffer->GetGPUVirtualAddress();
batched.ResultAddressArray       = {m_clasAddressArray->GetGPUVirtualAddress(), sizeof(D3D12_GPU_VIRTUAL_ADDRESS)};
batched.ResultSizeArray          = {m_clasSizeArray->GetGPUVirtualAddress(),    sizeof(UINT64)};
batched.IndirectArgumentArray    = {m_clasArgsArrayGPUVA, m_clasArgsStride};
```
`ResultAddressArray` / `ResultSizeArray` are *outputs* in implicit mode and *inputs* in explicit mode.

The 80-byte per-cluster argument struct is filled **on the GPU** by a compute shader
(`FillClasFromTrianglesArgs.hlsl`) from a 28-byte CPU-side metadata entry per cluster
(`{clusterID, triCount, vertCount, vbByteOffset, ibByteOffset, opaqueFlag, matRegionIdx}`). Note the
layout contract documented at the top of that file: `BaseGeometryIndexAndFlags` packs the 24-bit
material/geometry index in the low bits and `D3D12_RTAS_CLUSTERED_GEOMETRY_FLAG_OPAQUE` in bit 31 —
so opacity is per *cluster*, not per geometry, which DXR1 cannot express.

This "small CPU metadata → GPU expands to full args" pattern is the reusable idea: the host never
touches per-cluster data at runtime.

### 1.4 BLAS from CLAS: one pool, one barrier

`BuildBlasFromClasIndirect()` builds every object's cluster BLAS in **one** batched operation:

* per-object result size is memoised by *cluster count* (`blasSizeByClusterCount`), because BLAS size
  depends on how many CLAS it references, not on which;
* all objects' BLAS live in a single `m_clusterBlasPoolBuffer`; each object's `blasGPUVA` is a
  running aligned offset (`D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT` = 256);
* `Mode = EXPLICIT_DESTINATIONS`, so each per-object `BUILD_BLAS_FROM_CLAS_ARGS` carries its own
  destination — this "collapses 10 000 driver allocation calls into one";
* the args array itself is GPU-filled (`FillBlasFromClasArgs.hlsl`, 16 bytes/entry:
  `{ClasAddressCount, ClasAddressStride, ClasAddressArray}`), each entry pointing at a *slice* of the
  global CLAS address array;
* a single `UAV(m_clusterBlasPoolBuffer)` barrier covers every BLAS in the pool.

Take-away for D3D12: pool + explicit destinations + one barrier beats per-object resources by a
large factor, and the pattern generalises to any "many small GPU objects" resource.

### 1.5 Animated geometry: templates + per-frame instantiate

The animated sphere is the showcase for `BUILD_CLUSTER_TEMPLATES_FROM_TRIANGLES` and
`INSTANTIATE_CLUSTER_TEMPLATES`:

* **once, at setup**: build one cluster template per cluster of the rest-pose mesh
  (96-byte args: the 80-byte CLAS-from-triangles args + 8-byte `InstantiationBoundingBoxLimit` +
  8-byte format union). Templates contain topology and *no* positions (optional hint positions),
  so they are much smaller than a CLAS and are position-invariant;
* **every frame**:
  1. `AnimateBall.hlsl` (compute) writes deformed positions into `perFrameVertexBuffer`;
  2. `FillInstantiateArgs.hlsl` (compute) writes one 32-byte
     `D3D12_RTAS_OPERATION_INSTANTIATE_CLUSTER_TEMPLATES_ARGS` per cluster
     (`{GeometryIndexOffset, ClusterIdOffset, ClusterTemplateGVA, VertexBuffer{GVA,Stride}}`),
     reading the template GVAs that the *previous* pass wrote — no CPU address round-trip;
  3. `INSTANTIATE_CLUSTER_TEMPLATES` (implicit destinations) produces the frame's CLAS;
  4. `BUILD_BLAS_FROM_CLAS` (explicit destinations) builds the source BLAS **and** one BLAS per
     animated clone in the same batch (`MaxArgCount = 1 + N_clones`), each from the same CLAS array.

Barrier discipline, as written: UAV barrier + explicit state transitions around each op
(`UNORDERED_ACCESS → NON_PIXEL_SHADER_RESOURCE` for the vertex buffer and for the address arrays),
and the vertex buffer is flipped back to UAV immediately after instantiation so the next frame's
compute pass needs no mid-frame transition.

The clone fan-out is the counterpart of the rasterizer's "same mesh, N instances" problem: in
traditional mode each clone needs its own BLAS **rebuild every frame**; in cluster mode the CLAS is
built once per frame and cloned BLAS builds are cheap references. §4.1 measures the traditional
cost.

### 1.6 Shader side

* `ClusterID()` (SM 6.10, gated by `D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_CLUSTERED_GEOMETRY`, or
  `RAYQUERY_FLAG_ALLOW_CLUSTERED_GEOMETRY` for inline rays) returns the 32-bit id the app stamped
  into the CLAS; `PrimitiveIndex()` is then **cluster-local** (0..255).
* `GeometryIndex()` stays per triangle: the sample stamps a *material region* index into each
  cluster's `BaseGeometryIndexAndFlags`, so a hit group / material lookup still works without one
  geometry descriptor per cluster.
* The traditional path reconstructs the same `(clusterId, primitiveIndex)` pair from
  `GeometryIndex()` + `PrimitiveIndex()` through a per-region triangle→cluster lookup table
  (`BuildTradCidLookup`), so both paths share every downstream buffer (`g_clusterMeta`,
  `g_clusterNormals`, `g_clusterIndices`, `g_clusterOffsets`). That is a good pattern for us too:
  keep "which cluster am I shading" as an indirection, not a pipeline branch.

### 1.7 Limits and alignment to remember

| Constant | Value |
|---|---|
| Triangles per CLAS | 256 |
| Unique vertex indices per CLAS | 256 (numeric identity, not position equality) |
| CLAS byte alignment | 128 |
| Cluster template byte alignment | 32 |
| Instantiation bounding-box alignment | 32 |
| Acceleration-structure alignment (cluster BLAS) | 256 |
| `MaxGeometryIndexValue` | 2^24 − 1 |
| Vertex formats | `FLOAT32_2/3`, `FLOAT16_2/3`, `UNORM16_2/3`, `SNORM16_2/3`, `UNORM8_2/3`, `SNORM8_2/3`, `10_10_10_2_UNORM`, `COMPRESSED1` |
| Compiled cluster BVH alignment / no compaction | compaction does not exist for CLAS; use `MOVE_CLUSTER_OBJECTS` |

`COMPRESSED1`: 12-byte header (exponent + 24-bit anchor per axis + 1–16 bits/component) followed by
per-vertex deltas from the anchor; a shared exponent across the scene keeps neighbouring clusters
watertight (`EncodeCompressedClusters()` picks one scene-wide exponent from the maximum extent —
the same trick `meshopt_computePositionExponent` documents). `TemplateInstancePrefersCOMPRESSED1`
tells you whether the device prefers that stored format for template instances.

---

## 2. Reference 2 — `vk_lod_clusters`

### 2.1 What it is

A Vulkan sample (NV RTX Mega Geometry + `VK_NV_cluster_acceleration_structure` +
`VK_EXT/NV_mesh_shader`) implementing a Nanite-style continuous-LOD cluster renderer with a complete
geometry streaming system, for both rasterization and ray tracing. Scene default: a ~7 M-triangle
model grid, optionally with many copies (`--gridcopies`), optionally fully preloaded
(`--streaming 0`) or streamed per group. Built and run on this machine in both modes — the pass
order and timings quoted here are source-verified and runtime-verified (§4.3).

### 2.2 Offline: how the LOD DAG is built

`docs/lod_generation.md` + `src/scene_cluster_lod.cpp` + `src/meshopt_clusterlod.h`:

1. **clusterize** — `meshopt_buildMeshletsSpatial` / `meshopt_buildMeshletsFlex`; defaults
   `CLUSTER_VERTEX_COUNT = CLUSTER_TRIANGLE_COUNT = 32`, group size `GROUP_CLUSTER_COUNT = 32`
   (max 128 clusters/group, 256 tris/verts per cluster).
2. **group** — clusters of clusters, deliberately crossing the previous level's group borders
   (`meshopt_partitionClusters`), because a border locks its edges against decimation; crossing
   borders lets the *old* borders decimate on the next iteration.
3. **simplify within groups** — lock the vertices shared between groups, then `meshopt_simplify`
   each group to roughly half the triangles. The group that produced a cluster is recorded as its
   **generating group**.

Loop until one cluster remains → a **DAG** (not a tree: two groups can produce clusters that end up
in one new group). Stored per group: generating-group relation per cluster, bounding sphere,
accumulated **quadric error**, per-cluster bbox, per-cluster material/state bits.

Bounding spheres are deliberately **inflated** so that a coarser group's sphere is never *nearer*
the camera than its generating group's sphere. That is what makes an independent, parallel
per-cluster decision produce a watertight, non-overlapping cut: "render the cluster when my group's
error is above the threshold and my generating group's error is below it".

### 2.3 Runtime data layout (`shaders/shaderio_scene.h`)

* `Cluster` — 16 bytes: `triangleCountMinusOne`, `vertexCountMinusOne`, `lodLevel`,
  `groupChildIndex`, `attributeBits`, `localMaterialID`, `stateBits`, and **three 24-bit offsets**
  (positions / attributes / triangles) packed into 72 bits relative to the cluster header. Everything
  is `../../`-relative so a group can be memcpy'd anywhere.
* `Group` — `{residentID, clusterResidentID, lodLevel, stateBits, clusterCount,
  TraversalMetric{sphere xyz, radius, maxQuadricError}}`, immediately followed by
  `clusterCount` clusters, then the generating-group array, then per-cluster bboxes.
* `Node` — one `uint32` union: interior `{isGroup=0, childOffset:26, childCountMinusOne:5}`, leaf
  `{isGroup=1, groupIndex:23, groupClusterCountMinusOne:8}`.
* Per geometry: `nodes[]` (root has one child per LOD level; each level's node tree is spatial),
  `nodeBboxes[]`, `lodLevels[] {minBoundingSphereRadius, minMaxQuadricError}`, and
  `streamingGroupAddresses[]`.

`shaderio::Cluster_getVertexPositions/Normals/TexCoords/TriangleIndices` (`uint8` triangle indices,
palette materials) decode straight out of the group blob — no index buffer, no vertex buffer,
matching what a mesh shader wants.

### 2.4 The LOD metric (`shaders/traversal.glsl`)

```hlsl
float errorDistance     = max(nearPlane, sphereDistance - radius * uniformScale);
float errorOverDistance = maxQuadricError * uniformScale / errorDistance;
return errorOverDistance >= errorOverDistanceThreshold * errorScale;
```

`errorOverDistanceThreshold` is derived from one pixel's angular size, so the metric is "is this
group's geometric error smaller than a pixel at its nearest point". `classifyInstanceLod()` scans
the root's per-level children to produce a conservative `[lodLevelMin, lodLevelMax]` range: the
minimum uses the *maximum* accumulated sphere (first level coarse enough), the maximum uses a
synthetic worst-case sphere (`minBoundingSphereRadius`, `minMaxQuadricError`, pushed to the far side
of the instance sphere). That classification is what makes BLAS sharing/caching and discrete LOD
possible *without* a full traversal.

### 2.5 Pass structure — rasterization (`src/renderer_raster_clusters_lod.cpp: render()`)

```
frame constants / streaming cmdBeginFrame / cmdPreTraversal
for pass in (0 .. passCount-1):                      # passCount = 2 with two-pass culling
    Traversal Preparation:
        [optional] traversal_presort (+ radix sort of instances)
        traversal_init            (seed roots per instance, or the compacted reject list)
        build_setup               (clamp counters, build indirect dispatch args)
    Traversal Run:
        traversal_run             (nodes; multi-pass over tree depth, or persistent producer/consumer)
        build_setup
        traversal_run_groups      (per group: per-cluster metric + culling → render list)
        [pass 1] traversal_reject_clusters (re-test rejected clusters only)
        build_setup (_DRAW)       (build DispatchMesh / Dispatch indirect grid)
    Post traversal (streaming age filter on the last pass)
    Draw:
        [optional] compute SW raster
        HW mesh-shader draw: vkCmdDrawMeshTasksIndirect(NV/EXT), opaque + alpha lists
        overlays (cluster/instance bboxes)
    build HZB from this pass' depth (into the other HiZ layer)
```

`traversal_run_groups.comp.glsl` is the heart: one thread per cluster group; for each cluster it
takes the **generating group's** metric, negates the test, and appends the cluster to
`renderClusterInfos` (or the alpha / SW lists) with subgroup ballot + one atomic per subgroup. With
streaming, an unresident generating group means "this is the finest available" and the cluster is
drawn.

`render_raster_clusters.mesh.glsl` is a straightforward cluster mesh shader: workgroup = cluster,
`max_vertices/max_primitives` from the compile-time cluster limits, positions from the group blob,
optional NV primitive culling and two-sided handling, `clusterID/instanceID` passed flat to the
fragment shader. This is the piece that maps 1:1 onto D3D12 `DispatchMesh`.

Ray tracing (`renderer_raytrace_clusters_lod.cpp`) replaces "Draw" with:
`Instance Classify` → `Geometry BLAS Sharing` → `Traversal Run` → `BLAS Build Preparation` →
`BLAS Build` → `BLAS Copy` → `TLAS Preparation` → `TLAS Build` → `Render`.

### 2.6 Two-pass culling and reject lists (`docs/raster_twopass_culling.md`)

* pass 0 tests against last frame's HiZ and last frame's matrices; whatever it rejects is recorded;
* HiZ is rebuilt from pass 0's depth; pass 1 tests against *this* frame's HiZ;
* without reject lists, pass 1 repeats `traversal_init` over every instance and re-walks the DAG;
* with `USE_TWO_PASS_REJECT_LISTS` the first pass hands over: rejected instances (compacted list),
  rejected inner nodes, rejected group leaves, and a **per-cluster bitmask** per traversed group.
  Everything recorded is first filtered with `intersectFrustumOnly()` (clip-space corner test, no
  HiZ lookup, no divide) because geometry outside the current frustum cannot reappear.

Measured in that document: traversal cost is essentially unchanged, and the benefit scales with
instance count, because the saved work is `traversal_init` over all instances.

### 2.7 Discrete LOD (`docs/raster_discrete_lod.md`)

For distant instances the traversal is seeded *at a LOD level's node* and tagged
(`TRAVERSAL_DISCRETE_BIT` in the instance id) so every node below descends unconditionally and every
cluster of the touched groups is drawn. Eligibility: survives culling, `lodLevelMin` inside the last
`--discreteenabledlevels` levels, that level fully resident, and `lodLevelMax - lodLevelMin <
--discretelodrange`. The point is not raster speed but **agreement with the ray tracing path** (a
cached BLAS is built from exactly that level), i.e. it is a hybrid-renderer feature. Measured: seeded
traversal 111–118 µs (Caldera) / 116–122 µs (Zorah) versus a flat per-group kernel at 226–252 /
189–192 µs, and 115–119 / 124–127 µs with the technique off — so seeding the existing hierarchical
culling beats a dedicated flat kernel.

### 2.8 Streaming (`docs/streaming.md`, `src/scene_streaming*.cpp`, `shaders/stream_*.comp.glsl`)

Granularity: **group** (all clusters that were decimated together — guaranteed mutually watertight,
so any subset of levels is crack-free at group boundaries).

Per-frame flow:

1. **device**: traversal appends missing groups to `StreamingRequest::loadGeometryGroups` — encoded
   as "address ≥ `STREAMING_INVALID_ADDRESS_START`", with an `atomicMax` on the tagged
   `streamingGroupAddresses[groupIndex]` to make the request idempotent per frame; after traversal
   `stream_agefilter_groups` appends groups that have not been touched for N frames;
   `cmdEndFrame` reads the request back to the host.
2. **host**: `handleCompletedRequest` limits the number of loads (per-frame budget, memory budget),
   asks for storage upload and prepares a `StreamingUpdate` patch list; unloads free memory.
3. **device**: `stream_update_scene.comp.glsl` patches `streamingGroupAddresses`, fills the resident
   table (`resident.groups/clusters`), inserts into the compact `activeGroups` list, and — for ray
   tracing — emits one `ClasBuildInfo` per new cluster and **builds the CLAS into scratch first**.
4. **CLAS placement**: either simple compaction (move every resident CLAS on unload — incompatible
   with BLAS caching) or the persistent allocator (`docs/clas_allocation.md`): a bit array in units
   of 128 B, minimum allocation 32 units, gap search inside one subgroup over a sector of
   `32 << 10` bits, free gaps binned by size, worst-case-gap status written back so the host knows
   whether it may schedule more loads; the CLAS buffer is **sparse** and grows in 128 MiB chunks so
   addresses never change.

Ray-tracing specialisation: the **positions** of a streamed group are only needed while building its
CLAS, so they go to a per-frame scratch buffer instead of the persistent blob; the hit shader
reconstructs the triangle from `TriangleObjectPositions()` / `gl_HitTriangleVertexPositionsEXT`.
Rasterization keeps positions with the group.

### 2.9 BLAS sharing / caching / merging (`docs/blas_sharing.md`, `blas_caching.md`, `blas_merging.md`)

All three need the cheap `classifyInstanceLod()` range:

* **sharing** — an instance reuses another instance's BLAS when its `[min,max]` range is contained in
  the sharer's;
* **caching** — a BLAS built from one discrete LOD level is kept across frames (and across instances
  of the same geometry); `GeometryCachedInfo::cachedLevel` + the streaming age filter keep that
  level's groups resident;
* **merging** — one BLAS built at the highest detail is used by all instances up to a level
  (guaranteed compatible), trading trace performance for build count.

---

## 3. What a D3D12 renderer actually has to build

Synthesising the references and the public material (Nanite, `GPU-Driven Rendering Pipelines`, the
D3D12 mesh shader docs, meshoptimizer's clusterisation section):

### 3.1 Cluster rasterization in D3D12

* `DispatchMesh`/`DispatchMeshIndirect` replaces `DrawMeshTasksIndirectNV`; per-cluster workgroup =
  one visible cluster; `SetMeshOutputs(triCount, vertCount)`; per-primitive attributes via
  `nointerpolation` or `SV_PrimitiveID`.
* Hardware limits: 256 vertices and 256 primitives per mesh shader output (AMD favours 128/128,
  NVIDIA 64/126 or 64/96 in practice). The Vulkan sample's 32/32 default is a LOD-granularity choice,
  not a hardware one.
* D3D12 cannot byte-address load triangle indices; either use `Buffer<uint>` with
  `DXGI_FORMAT_R8_UINT` or pack 3×`uint8` into one `uint` (the pattern this renderer's
  `UnpackTriangle(packed)` already uses).
* Amplification shader vs compute pre-pass: the AS gives per-meshlet culling *inside* the same draw
  (this renderer's `ASMain`), a compute pass gives a flat, sortable cluster list across *all*
  instances with one indirect dispatch (the Vulkan sample). The Vulkan sample's measurement argues
  for keeping the hierarchical culling and only batching the leaves.
* Cone culling (`dot(normalize(apex - eye), axis) >= cutoff`) is cheap and effective for closed
  surfaces; backface culling of whole clusters is not available on AMD.

### 3.2 GPU-driven culling hierarchy

The consensus structure, present in both references and in this renderer:

```
instance culling (sphere vs frustum, sphere vs HiZ)      ← exists (GPUCulling.hlsl)
  → LOD/group selection per instance or per region       ← NEW (cluster DAG traversal)
    → cluster culling (bbox vs frustum, bbox vs HiZ)     ← partially exists (per-meshlet in ASMain)
      → triangle/primitive culling (optional)            ← exists (testTriangleHW in AS)
```

Two-phase occlusion + reject lists (Vulkan sample) and "test the previous frame's HiZ, rebuild HiZ,
test again" (this renderer) are the same idea; the reject lists are the refinement that makes the
second pass cheap.

### 3.3 LOD metric

Quadric-error-based object-space error, converted to an angular/screen-space error at the nearest
point of the bounding sphere, compared to a per-pixel threshold. Two invariants matter:
* error must be **monotonic** along the DAG (hence inflated spheres) or the cut can overlap;
* the threshold should come from the projection (a pixel's angular size), not a hand-tuned distance,
  or the same content pops differently at different FOV/resolution.

Discrete per-instance LOD (what this renderer has) is the degenerate case of one level per instance;
it pops at object silhouettes because a single choice serves the whole object.

### 3.4 Streaming in D3D12 terms

* Budget per frame (bytes + group count) on the host; the device only *requests* and *reports*.
* Upload via a staging ring + `CopyBufferRegion`; the destination must be a stable address, which
  means either a preallocated arena with free-list (the Vulkan allocator's equivalent) or a reserved
  (tiled) resource grown in chunks — D3D12 has `UpdateTileMappings`/reserved resources for exactly the
  sparse-growth trick.
* Idempotent per-frame requests need a "requested this frame" tag; the address-tag trick in the
  Vulkan sample (top bit = invalid, low bits = frame index) avoids a second buffer.
* CLAS/geometry that is only needed transiently (ray-tracing positions) should be a scratch ring, not
  part of the persistent blob.

### 3.5 Ray tracing in D3D12

* DXR1 (what we have): BLAS per LOD per object; per-frame animation means rebuild/refit; there is no
  way to express "same geometry, different clusters" cheaply.
* DXR2 (preview, SDK 1.722 + SM 6.10 + `ClustersAndPTLASSupported`): CLAS + cluster BLAS + templates
  + PTLAS. **Not available here** (§0.2). If we upgrade, the sample in §1 is a working recipe for
  exactly the streaming/LOD structure the Vulkan sample uses.

---

## 4. Captures and measurements

### 4.1 D3D12 sample — PIX capture (`bin/captures/cg_sample_frame.wpix`)

Taken with `pixtool launch … take-capture --frames=30 save-capture` against
`bin\x64\Release\D3D12RaytracingClusteredGeometry.exe --exit-after-frames 600`.
Note the sample emits **no PIX markers**, so the event list is a flat sequence of API calls rooted at
`-1`; per-pass attribution has to come from the API sequence.

Event list (`cg_events.csv`, 30 frames, 8976 rows):

| API | Count (30 frames) | Per frame |
|---|---|---|
| `BuildRaytracingAccelerationStructure` | 660 | **22** |
| `DispatchRays` | 30 | 1 |
| `Dispatch` (animate ball) | 30 | 1 |
| `DrawIndexedInstanced` (DirectXTK overlay) | 1914 | 63.8 |
| `CopyResource` | 30 | 1 |
| `Present` | 30 | 1 |

Frame sequence (first events of a frame, from `cg_events.csv`):

```
Signal, Reset,  1× Dispatch (AnimateBall),  21× BuildRaytracingAccelerationStructure (1 source + 20 animated clones),
1× BuildRaytracingAccelerationStructure (TLAS), DispatchRays, CopyResource → backbuffer, ~64 overlay sprite batches
```

Relative GPU cost over the 30 captured frames (`gpu__time_duration.sum`, PIX-instrumented — use for
ranking only, per `docs/pix-capture-debugging.md` §4.2):

| Event | Σ GPU time | Σ instructions | Σ DRAM bytes |
|---|---|---|---|
| `BuildRaytracingAccelerationStructure` | 12.34 | 15.24 G | 3.67 GB |
| `DispatchRays` | 2.28 | 4.84 G | 34.18 GB |
| `DrawIndexedInstanced` (UI) | 0.26 | 9.8 M | 66 MB |

So in the traditional path **acceleration-structure maintenance costs ~5.4× the ray traversal work**,
and it is 22 builds per frame for a scene of 8 objects + 21 animated copies. This is the cost model
clustered geometry exists to remove (one CLAS build per frame, N cheap BLAS-from-CLAS builds).

The sample's own GPU-timestamp numbers (uninstrumented, `--anim-paused`, 150 frames):

```
[per-frame GPU, EMA] INSTANTIATE=0.0190 ms  BLAS=9.0893 ms  TLAS=0.0758 ms  staticBLAS=0.0001 ms  staticCLAS=0.0000 ms  render=2.3901 ms
[build wall-clock]   CLAS=0.000 ms  BLAS=26.756 ms  TLAS=0.081 ms (total 52.512 ms)
[storage]            CLAS: 0 bytes  BLAS: 1241344 bytes
```

**9.09 ms/frame of BLAS rebuild for 2.39 ms of rendering** — and `staticCLAS/staticBLAS = 0` because
the cluster path never ran. `INSTANTIATE = 0.019 ms` is the (unused) template path's bracketed cost.

RenderDoc was also tried on this sample (`renderdoccmd capture`): the sample exits without producing
a capture, consistent with RenderDoc's limited DXR support; the PIX capture above is the usable one.

### 4.2 This renderer — re-analysis of `bistrox-frame90.wpix`

Same recipe as `docs/pix-capture-debugging.md` §4.2 (`save-event-list` with
`gpu__time_duration.sum`, `smsp__inst_executed.sum`, `l1tex__t_sectors.sum`, `dram__bytes.sum`).
PIX-instrumented absolute times are inflated; the shares are the finding.

| Scope | Σ GPU time (rel.) | Instructions | DRAM |
|---|---|---|---|
| `RTXDIRenderer` | 10.22 | 3.14 G | 4.64 GB |
| **`Opaque Renderer`** | **4.38** | 1.21 G | 1.01 GB |
| **`Base Pass Render (Phase 1) - Opaque`** (DispatchMesh) | **4.21** | **604.2 M** | **536 MB** |
| `NRD Denoise` | 2.63 | 0.75 G | 0.64 GB |
| `SHARCRenderer` | 0.99 | 65.3 M | 475 MB |
| `FSR3 TAA` | 0.77 | 348 M | 363 MB |
| `TransparentPass` (incl. `Base Pass Render (Phase 1) - Transparent` 0.37) | 0.48 | 34.7 M | 54 MB |
| `MaskedPass` (incl. `Base Pass Render (Phase 1) - Masked` 0.34) | 0.42 | 90.2 M | 93 MB |
| `TLAS Update` | 0.38 | 5.5 M | 22 MB |
| `HZBGeneratorPhase2` | 0.071 | 3.6 M | 19 MB |
| `Occlusion Culling Phase 1 - Opaque` | 0.048 | 31 660 | 0 MB |
| `Occlusion Culling Phase 2 - Opaque` | 0.038 | 18 840 | 0 MB |
| `Build Indirect Arguments` | 0.014 | 296 | 0 MB |
| `Base Pass Render (Phase 2) - Opaque (Occluded)` | 0.006 | 14 | 0 MB |

Reading of this table:

* **the base pass is the Opaque renderer** (4.21 of 4.38); culling is 2.4 % of it;
* phase 2 finds essentially nothing (5.95 of 4 208 relative units) — with a static camera the
  previous-frame HiZ already had everything, so the *two-phase* structure is not the lever here;
* 604 M instructions and 536 MB of DRAM in the base pass is vertex/primitive/shading work over the
  geometry that survived culling. Reducing *that* is the only way to move the number, i.e.
  per-cluster LOD and cluster-level culling.
* `RTXDIRenderer` is by far the largest consumer; any base-pass optimisation also relieves the G-buffer
  footprint that RTXDI/NRD read (1 G instruction, 1 GB DRAM in the base pass alone).

### 4.3 `vk_lod_clusters` — build, runtime and profiler evidence

#### Build (Vulkan SDK 1.4.363.1)

The earlier attempt failed on the SDK version gate (`nvpro_core2 requires Vulkan SDK 1.4.341 or
newer`) because the machine had 1.4.335. Everything tried for 1.4.335 — patching the gate in the
generated tree, Khronos 1.4.368 headers, a merged include tree, `VK_ENABLE_BETA_EXTENSIONS` — is
obsolete now and was discarded. With SDK **1.4.363.1** installed and `VULKAN_SDK` pointed at it, a
**clean** configure reports:

```
-- Found Vulkan: C:/VulkanSDK/1.4.363.1/Lib/vulkan-1.lib (found version "1.4.363")
--   Include Directory         : C:/VulkanSDK/1.4.363.1/include
```

One source change was still needed, caused by nvpro_core2 `main` having moved past the sample's
revision (sample HEAD 2026-10-06, nvpro_core2 HEAD 2026-11-05): `src/resources.cpp` uses
`nvvk::GBuffer` without including `nvvk/gbuffers.hpp` any more —

```
resources.cpp(1566,94): error C2039: 'GBuffer': is not a member of 'nvvk'
```

fixed by adding `#include <nvvk/gbuffers.hpp>`. After that the whole tree builds:
`_bin\Release\vk_lod_clusters.exe`.

#### Runtime (bunny_v2, preloaded)

```
geometries: 1 (instances: 1)
cluster triangles: 128   cluster vertices: 128   cluster group: 32
clusters:  1135   triangles: 139124   triangles/cluster: 122.58
hi clusters: 558   hi triangles: 69630
raytracer: BLAS build scratchsize 32249 KiB / TLAS build scratchsize 80 KiB
```

So **the VK_NV_cluster_acceleration_structure path runs on this driver** (BLAS + TLAS built, cluster
builds issued) even though the DXR2 equivalent is unavailable (§1.2). The default bunny scene
instances the model on a grid: **1024 instances / 1024 geometries, 71.3 M scene triangles,
571 K clusters**.

#### GPU profiler, rasterization (`--renderer 0 --streaming 0 --gridunique 0`, 1534×726 window,
3068×1452 render)

Screenshot: `bin/captures/vklc_profiler.png`.

| Section | GPU avg (ms) |
|---|---|
| Frame | 4.023 |
| Traversal Preparation | 0.811 |
| Traversal Run | 0.999 |
| Draw | 1.673 |
| HiZ | 0.831 |
| Post-process | 2.218 |

Traversal: 5.04 K tasks requested, **9.02 K clusters** requested, 0 BLAS builds. LOD pixel error 1.0.

#### GPU profiler, ray tracing (`--renderer 1`)

Screenshot: `bin/captures/vklc_profiler_rt.png`.

| Section | GPU avg (ms) |
|---|---|
| Frame | 2.847 |
| Traversal Preparation | 0.811 |
| Traversal Run | 0.951 |
| BLAS Build Preparation | 0.819 |
| BLAS Build | 0.889 |
| TLAS Preparation | 0.903 |
| TLAS Build | 0.922 |
| Render | 2.550 |
| HiZ | 0.937 |
| Post-process | 0.896 |

Traversal: 5.02 K tasks, **11.1 K clusters**, **1.02 K BLAS builds** (100 % of the reserved budget).
Memory: geometry **1.88 GiB**, CLAS **1.79 GiB**, BLAS **3.59 MiB**, TLAS 188 KiB, operations 77 MiB.

Three things worth reading out of those numbers:

1. in rasterization the **whole cluster pipeline (traversal preparation + traversal run + draw) is
   3.5 ms of a 4.0 ms frame** — the same shape this renderer has (base pass dominates), but here the
   geometry per frame is 71 M scene triangles cut down to ~9 K clusters;
2. in ray tracing **1 024 cluster BLAS builds cost 0.89 ms**, i.e. ~0.9 µs per BLAS — the D3D12 sample
   spends **9.09 ms on 21 BLAS rebuilds** (§4.1) because each is built from triangles;
3. **BLAS is 3.59 MiB while CLAS is 1.79 GiB**: a cluster BLAS only stores references to CLAS, which is
   the memory argument for the model in §1.4 — and the reason a CLAS allocator (§2.8) is the actual
   memory-management problem.

#### The GPU captures that were not obtainable

Asked for a RenderDoc/PIX capture of this sample, both tools failed for tool-side reasons, recorded
here so the attempt is not repeated:

* **RenderDoc 1.47** hooks the app (`Initialised capture layer in Vulkan instance`, `Adding Vulkan
  frame capturer`), then fails while serialising the pipeline create-info chain and never writes a
  capture:
  `vk_serialise.cpp(3891) - Error - Assertion failed: 'ser.IsReading() || el.sType ==
  VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO'` (repeated ~6×, then nothing; no trigger,
  no file). Tried: `renderdoccmd capture` with and without the SDK `Bin` on `PATH` (the first attempt
  let the app load the SDK loader instead of RenderDoc's shim), windowed instead of `--headless`
  (headless has no present to trigger on), and an explicit **F12** capture key sent to the focused
  window. None produced a `.rdc`.
* **PIX 2610.28** (`pixtool launch … take-capture`) terminates the process immediately:
  `PIXTOOL18 - Process terminated`. PIX's Vulkan capture is not usable for this app on this install.
* Even a successful Vulkan `.rdc` would not be readable by **`rdc-tools`**, whose chunk decoders are
  D3D12-only (`README` §1: Vulkan parses at container level). The tooling the study was asked to use
  is therefore the wrong half for Vulkan; the sample's own `nvutils::ProfilerGpuTimer` sections above
  are the equivalent, and they name the same passes the source analysis in §2.5 describes.

The raster/RT screenshots are the primary runtime evidence for §2; the pass *order* and barriers were
read from `renderer_raster_clusters_lod.cpp` / `renderer_raytrace_clusters_lod.cpp` directly.

---

## 5. Gap analysis — where this renderer stands

| Concern | `HobbyRenderer` today | Reference design | Gap |
|---|---|---|---|
| Draw granularity | mesh shader, one AS threadgroup per 32 meshlets, one `DispatchMesh` over a flat job list | one mesh shader workgroup per **cluster** from a flat visible-cluster list | job list is per *meshlet group* of an instance; no cross-instance cluster list |
| LOD | **discrete per instance**, up to 8 levels, chosen in `GPUCulling.hlsl` from bounding-sphere distance and `m_LODErrors[]` | continuous per-cluster cut of a LOD DAG, screen-space error threshold | LOD pops, sub-pixel triangles at silhouettes, and one level serves the whole object |
| Culling | instance frustum + 2-phase HiZ (sphere), then meshlet sphere/cone/HiZ in `ASMain` | instance → node → group → cluster, frustum + HiZ, with reject lists | already 4-level, but the *leaves* are meshlets of a pre-chosen LOD, not the optimal cluster set |
| Culling cost | ~2.4 % of the base pass (measured §4.2) | same order | **not** the bottleneck |
| Geometry compression | quantised vertex buffer (`VertexQuantized`, `UnpackVertex`), meshlet micro-indices | per-cluster bit-packed positions + meshlet codec (~9–12 bits/triangle) | higher bandwidth and cache footprint per triangle |
| Streaming | **textures only** (sampler feedback + RTXTS-TTM, `FeedbackManager`) | geometry groups streamed with per-frame budget, CLAS built on load | no geometry streaming at all |
| RT | DXR1 TLAS + per-mesh BLAS per LOD; TLAS patched per frame (`TLAS Update` 0.38) | CLAS + cluster BLAS; templates for animation; PTLAS | D3D12: blocked by SDK/SM (preview 1.722 + SM 6.10). Vulkan equivalent measured working (§4.3) |
| Tools | PIX markers per pass, pipeline statistics, `--pix-capture-*` | — | better than both references; keep |

The important negative result: **culling is not where the time is**. Adding cluster-level culling to a
pipeline that already culls 98 % correctly at instance + meshlet level buys little; the win must come
from **the amount and size of geometry the base pass shades**, which is the LOD decision.

---

## 6. Technique synthesis: what to adopt, and what not to

Adopt:

1. **Cluster LOD DAG with quadric error + inflated bounding spheres** (Vulkan sample §2.2/§2.4).
   Monotonic error plus inflated spheres is what turns a parallel per-cluster decision into a
   watertight, non-overlapping cut.
2. **Group as the streaming and coherency unit** — clusters decimated together are mutually
   watertight, so a group is the smallest thing worth loading/unloading and the natural unit for the
   residency check.
3. **Traversal as compute, leaves as a flat list** — nodes/groups traversed hierarchically (culling
   benefits), clusters emitted into one list, then a single indirect mesh-shader dispatch. This is
   also the Vulkan sample's measured conclusion (§2.7).
4. **Subgroup-compacted appends** (ballot + one atomic per subgroup) for the cluster list — the
   Vulkan `rasterBinning` pattern.
5. **GPU-side argument generation** (DX sample §1.3/§1.5): host writes small metadata, a compute
   shader expands it into the per-object argument structs. Reusable for `DispatchMeshIndirect` args,
   `BuildRaytracingAccelerationStructure` instance descriptors, or CLAS args.
6. **Meshlet compression** (`meshopt_encodeMeshlet`, ~3× on indices) and **per-cluster position
   quantisation with a shared scene exponent** — the latter matters for watertightness as much as
   for size.
7. **Two-pass culling with reject lists** — but only after LOD work, and only if the second pass
   actually finds geometry (§4.2 says it does not, with a static camera).

Do not adopt (yet):

* **CLAS/PTLAS** — blocked by SDK 1.620 vs preview 1.722 and SM 6.10; revisit as a separate project.
* **A dedicated flat per-group kernel for discrete LOD** — measured worse than seeding the existing
  traversal (§2.7).
* **Simple CLAS compaction** — moves every resident CLAS on unload; incompatible with any cached
  acceleration structure. If CLAS is adopted later, start with the persistent bit-array allocator.
* **SW rasterization** — the Vulkan sample itself notes it is not faster when clusters have larger
  than single-pixel triangles.

---

## 7. Integration plan for `HobbyRenderer`

Assumptions: static geometry only for the first pass; Opaque bucket first, Masked second,
Transparent last; the existing `RenderGraph`, `srrhi` codegen (`src/shaders/*.sr` → C++/HLSL),
`ShaderID` enum and `AddComputePass`/indirect-dispatch plumbing are reused unchanged.

### Phase 0 — offline cluster-LOD builder (CPU, no rendering change)

* Use `external/meshoptimizer` — it already contains `clusterizer.cpp`; add the `clusterlod.h`
  single header (the Vulkan sample's lightly modified copy is the reference implementation).
* Extend `SceneLoader`'s per-mesh LOD section (currently `meshopt_simplify` per level +
  `meshopt_buildMeshlets` per LOD, `src/SceneLoader.cpp` around the `m_LODErrors` write) with a
  second, cluster-LOD output: clusters, groups (generating-group array, bounding sphere, quadric
  error), per-level min/max sphere+error, spatial node tree.
* New `.sr` structs (`ClusterData.sr`, `ClusterGroup.sr`, `ClusterNode.sr`) so C++/HLSL stay in sync
  through the existing generator.
* Verify: a CPU unit test that walks the DAG and asserts (a) every cluster's group error is ≥ its
  generating group's, (b) raising the threshold monotonically eventually selects only the root
  cluster, (c) the selected set at any threshold tiles the mesh (no gaps/overlaps) for a test mesh.

### Phase 1 — traversal + cluster culling (compute, still rasterizing today's path)

* New pass after `OpaqueRenderer`'s instance culling: `ClusterTraversal` (nodes → groups) and
  `ClusterGroups` (per-cluster metric + culling → `m_VisibleClusters`), modelled on
  `traversal_run.comp.glsl` / `traversal_run_groups.comp.glsl`, reading the existing HZB and the
  frustum planes already uploaded for `GPUCulling.hlsl`.
* Keep the instance sphere test as the seed (it is already right); classification gives
  `[lodLevelMin, lodLevelMax]` per instance.
* Replace the per-instance LOD pick in `GPUCulling.hlsl` with "seed the traversal"; leave the
  existing meshlet path intact behind a switch so A/B is one flag.
* Verify: same image as today within a threshold (screenshot diff), visible-cluster count and
  `DispatchMesh` grid logged in the renderer's own stats, base-pass GPU time from PIX
  (`Base Pass Render (Phase 1) - Opaque`) as the A/B metric.

### Phase 2 — cluster mesh-shader draw

* New `BasePass` variant: one workgroup per visible cluster (`DispatchMeshIndirect`, grid = cluster
  count), positions/indices decoded from the cluster blob, `clusterID`/`instanceID` passed flat to
  the existing PS (which already takes `nointerpolation` ids).
* Reuse the existing G-buffer layout and the alpha-test variant; keep the stencil-write setup.
* Verify: image match with phase 1, and the counter triple (instructions, L1 sectors, DRAM) from the
  Bistro capture compared against §4.2.

### Phase 3 — two-pass culling + reject lists

* Only worth it once phases 1–2 make the base pass cheaper (the second pass currently finds ~0.1 % of
  the work). Implement with the existing two-phase structure: record rejects in pass 0, swap the
  traversal seeds in pass 1 (`setupSecondPass` equivalent).
* Verify: pass-1 traversal time and the reject counters; expect no image change.

### Phase 4 — geometry streaming

* Groups are the unit; per-frame load/unload budget; residency table + compact active list; the
  device requests, the host decides (mirror of `StreamingRequest/Resident/Update`).
* Use a preallocated arena with a free-list (simplest) or a reserved resource grown in chunks
  (`UpdateTileMappings`) if growth without address churn is needed; keep addresses stable so nothing
  has to be re-patched.
* Reuse the existing texture streaming infrastructure's *shape* (jobs, fences, per-frame budget) —
  `AsyncTileIO`/`FeedbackManager` are the in-repo precedent for "device asks, host serves".
* Verify: memory ceiling honoured, no visual holes, load latency under a budget, and cluster count
  per frame converging as the camera moves.

### Phase 5 — ray tracing story

* Short term (no SDK change): keep DXR1 BLAS, but derive the BLAS **per discrete LOD level** and cache
  them (the Vulkan sample's BLAS caching idea), instead of rebuilding per frame; keep animation on
  refit.
* Long term (requires Agility 1.722 preview + SM 6.10 + `ClustersAndPTLASSupported`): adopt CLAS +
  cluster BLAS + templates following §1, with the streaming allocator from §2.8. Gate it behind a
  capability check exactly like the sample's `QueryClusteredGeometrySupport()`.

---

## 8. Risks and open questions

| Risk | Mitigation |
|---|---|
| Cluster LOD build is expensive on large scenes (the Vulkan sample caches it in `.nvsngeo`) | build once offline, cache next to the asset; the renderer already has a scene cache (`SceneCache`) |
| LOD popping/watertightness bugs are subtle (inflated spheres, shared exponent) | CPU test from phase 0 before any GPU work; visual A/B at threshold sweeps |
| Mesh shader workgroup per cluster raises dispatch count (tens of thousands) | measured in the Vulkan sample as fine; this renderer already dispatches one grid over all meshlet jobs |
| Cluster-level culling may not pay for itself (measured: culling is 2.4 %) | phase 1/2 A/B on base-pass time, not on culling time |
| Streaming interacts with the existing sampler-feedback streaming (bandwidth, queues) | separate budgets and queues; measure together, not separately |
| Agility/SM upgrade for CLAS is a preview dependency | treat as a separate project with its own capability gate and fallback |

Open questions to settle before phase 1:

* Do we keep the current discrete per-mesh LODs as the "far" fallback (cheap, already built) and use
  cluster LOD only inside a distance band, or replace them outright?
* Which error threshold: one pixel, or a per-pass budget (the G-buffer is read by RTXDI/NRD, whose
  cost is dominated by *shading*, not geometry)?
* Does the cluster path need to serve Masked/Transparent buckets in phase 2, or Opaque only?
* Is `DispatchMeshIndirect` per cluster acceptable given the existing 22-bit `MeshletJob` design, or
  do we keep the "one job per N clusters" shape?

---

## 9. References

* `D:\Workspace\DirectX-Graphics-Samples\Samples\Desktop\D3D12Raytracing\src\D3D12RaytracingClusteredGeometry`
  — `readme.md` (controls, alloc modes, benchmark harness), `D3D12RaytracingClusteredGeometry.cpp`
  (`BuildClas*`, `BuildBlasFromClasIndirect`, `BuildAnimatedObjectSetup`,
  `UpdateAnimatedObjectPerFrame`, `BuildTlasClassic`), `Fill*.hlsl` (GPU argument layouts),
  `Raytracing.hlsl` (cluster/traditional hit paths).
* `D:\Workspace\vk_lod_clusters` — `README.md`; `docs/lod_generation.md`, `docs/streaming.md`,
  `docs/clas_allocation.md`, `docs/raster_twopass_culling.md`, `docs/raster_discrete_lod.md`,
  `docs/blas_sharing.md`, `docs/blas_caching.md`, `docs/blas_merging.md`;
  `shaders/traversal.glsl`, `traversal_run.comp.glsl`, `traversal_run_groups.comp.glsl`,
  `render_raster_clusters.mesh.glsl`, `culling.glsl`, `shaderio_scene.h`, `shaderio_building.h`,
  `shaderio_streaming.h`, `stream_update_scene.comp.glsl`;
  `src/renderer_raster_clusters_lod.cpp`, `src/renderer_raytrace_clusters_lod.cpp`,
  `src/scene_cluster_lod.cpp`.
* DirectX Raytracing functional spec, part 2 (clustered geometry, PTLAS, indirect RTAS operations),
  v0.38 — <https://microsoft.github.io/DirectX-Specs/d3d/Raytracing2.html>.
* meshoptimizer v1.3 README — clusterisation, meshlet bounds and cone culling, meshlet codec,
  cluster position quantisation, `Compressed1` helper.
  <https://github.com/zeux/meshoptimizer>
* Karis et al., *A Deep Dive into Nanite Virtualized Geometry*, SIGGRAPH 2021 Advances in Real-Time
  Rendering — <https://www.advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf>.
* Haar and Aaltonen, *GPU-Driven Rendering Pipelines*, SIGGRAPH 2015 — two-phase occlusion culling,
  cluster pipelines.
* `docs/pix-capture-debugging.md` (this repo) — capture switches, `pixtool` recipes, known issues.
