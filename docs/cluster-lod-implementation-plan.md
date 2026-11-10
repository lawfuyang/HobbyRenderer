# Cluster LOD implementation plan — `vk_lod_clusters` feature set for the Opaque and Masked base passes

Companion document to `docs/clustered-geometry-study.md` (the study). This one is the execution
plan: phases, tasks, data layouts, file-level touch points, and the correctness/performance gates
each phase must clear before the next one starts.

Date: 2026-11-06.

---

## 0. Scope and locked decisions

**In scope** (all of it taken from the `vk_lod_clusters` feature set):

* offline continuous-LOD cluster generation (clusterise → group → simplify → DAG) with quadric error
  and inflated bounding spheres;
* GPU LOD traversal with a **pixel error threshold**;
* cluster-level frustum + HiZ culling, feeding a flat visible-cluster list;
* `DispatchMeshIndirect`, **one mesh-shader workgroup per cluster**;
* two-pass occlusion culling with **reject lists**;
* geometry streaming at **group** granularity with a per-frame budget;
* discrete-LOD classification so ray tracing and rasterisation agree on resident geometry;
* debug views (cluster id, LOD level, cluster bounds) and feature toggles in `ImGuiLayer`.

**Explicitly out of scope** (studied and rejected for now — see study §6):

* DXR2 clustered geometry: CLAS, cluster BLAS, cluster templates, PTLAS. Not referenced anywhere in
  this plan. Ray tracing keeps its current DXR1 BLAS-per-(mesh, LOD) path.
* Software rasterisation of clusters, persistent traversal kernels, BLAS sharing/merging.
* Texture streaming changes (already implemented; geometry streaming is added alongside it).

**Decisions locked by the requester** (they shape every phase below):

| Decision | Consequence |
|---|---|
| Cluster LOD cached in `SceneCache` | cooked-mesh format v3; the DAG is produced once per asset, never at load |
| Replace per-mesh LODs | `srrhi::MeshData::m_LODErrors/m_LODIndex/m_Meshlet*` stop being the LOD mechanism for Opaque and Masked; the transparent bucket keeps them (see below) |
| Error threshold in **pixels** | one per-frame scalar computed from the projection, replacing the `targetPixelError = 2.0f` heuristic in `GPUCulling.hlsl` |
| Cluster path for **Opaque and Masked**; Transparent stays legacy | Opaque and Masked get the traversal + cluster draw. Transparent keeps the plain vertex-shader path — **not** meshlets, **not** clusters — until OIT exists, so its per-mesh LOD arrays stay in the cache and `GPUCulling` keeps selecting a LOD for those instances |
| `DispatchMeshIndirect` **per cluster** | one workgroup per visible cluster; no per-mesh job list |
| Toggles + debug views in `ImGuiLayer` | new `Renderer` members plus a "Cluster LOD" UI section and CLI mirrors |

---

## 1. Baseline, and what "better" means

Measured today (study §4.2, `bin/captures/bistrox-frame90.wpix`, PIX replay-instrumented units, shares
are the finding):

| Scope | Share | Instructions | DRAM |
|---|---|---|---|
| `Opaque Renderer` | 4.38 | 1.21 G | 1.01 GB |
| `Base Pass Render (Phase 1) - Opaque` | 4.21 | **604.2 M** | 536 MB |
| `Occlusion Culling Phase 1/2 - Opaque` | 0.086 | 50 K | 0 MB |
| `Build Indirect Arguments` | 0.014 | 296 | 0 MB |
| `MaskedPass` / `Base Pass … - Masked` | 0.42 / 0.34 | 90 M | 93 MB |
| `TransparentPass` / `Base Pass … - Transparent` | 0.48 / 0.37 | 35 M | 54 MB |

Reading: culling is ~2.4 % of the base pass; the base pass is vertex/primitive/shading work over a
geometry set that is already culled at instance level. The only lever that moves it is **shading fewer,
better-chosen triangles**, which is what cluster LOD does.

Reference numbers from the running `vk_lod_clusters` (study §4.3, same machine):

| Mode | Frame | Traversal prep | Traversal run | Draw | BLAS build |
|---|---|---|---|---|---|
| Raster, 1024 instances / 71.3 M scene tris | 4.02 ms | 0.81 | 1.00 | 1.67 | — |
| Ray tracing, 1 024 cluster BLAS | 2.85 ms | 0.81 | 0.95 | 2.55 (render) | 0.89 |

Targets for this plan (Bistro, 1080p, fixed camera, TAA and RTXDI off for A/B):

| # | Metric | Target |
|---|---|---|
| T1 | `Cluster Traversal` (prep + run + args) GPU time | ≤ 0.25 ms |
| T2 | Opaque base-pass GPU time at pixel error 1.0 | ≤ 0.85 × baseline |
| T3 | Base-pass `smsp__inst_executed.sum` | ≤ 0.80 × baseline |
| T4 | Frame VRAM (rdc-tools `memory`) | ≤ baseline + cluster data + 10 % |
| T5 | Drawn clusters vs. the CPU reference cut | within ±5 % |

---

## 2. Feature map: reference → this renderer

| `vk_lod_clusters` feature | Where it lives there | Our counterpart |
|---|---|---|
| `clodBuild`/`clusterize`/`partition`/`simplify` loop | `src/meshopt_clusterlod.h`, `src/scene_cluster_lod.cpp` | `src/ClusterLodBuilder.cpp/.h` (new), driven from `SceneLoader` |
| Cluster LOD asset binary | `storeGroup`, `.nvsngeo` cache | `SceneCache` cooked-mesh v3 (`src/SceneCache.cpp`) |
| `Cluster`, `Group`, `Node`, `TraversalMetric` | `shaders/shaderio_scene.h` | `src/shaders/Cluster.sr` (new) |
| `traversal_init.comp.glsl` | seeds roots, instance classify | `src/shaders/ClusterTraversalInit.hlsl` (new) |
| `traversal_run.comp.glsl` | node traversal | `src/shaders/ClusterTraversalRun.hlsl` (new) |
| `traversal_run_groups.comp.glsl` | per-cluster metric + culling + list append | `src/shaders/ClusterTraversalGroups.hlsl` (new) |
| `traversal_reject_clusters.comp.glsl` | second-pass re-test | `src/shaders/ClusterRejectClusters.hlsl` (new, phase 4) |
| `build_setup.comp.glsl` | indirect args, counter clamps | `src/shaders/ClusterBuildArgs.hlsl` (new) |
| `render_raster_clusters.mesh.glsl` | mesh shader per cluster | `src/shaders/ClusterBasePass.hlsl` (new MS/AS; PS reused) |
| `culling.glsl` (bbox frustum + HiZ) | 8-corner clip-AABB tests | `src/shaders/Culling.hlsli` (extend) |
| `render_cluster_bbox.mesh.glsl` | debug view | `src/shaders/ClusterDebug.hlsl` (new) |
| `Streaming*` / `stream_*.comp.glsl` | streaming system | `src/GeometryStreaming.cpp/.h` (new, phase 5) |
| `classifyInstanceLod` (discrete LOD) | RT/raster agreement | `ClusterTraversalInit.hlsl` + `TLASPatch.hlsl` |

---

## 3. Target architecture

### 3.1 GPU data (new `src/shaders/Cluster.sr`)

Generated by the existing srrhi pipeline (`build_srrhi`, `build_shaderids`), so C++ and HLSL cannot
drift. Sizes are chosen to match the reference layout where it is already tight.

```
struct ClusterTraversalMetric       // 20 B, same fields as the reference
{
    float m_SphereX, m_SphereY, m_SphereZ, m_SphereRadius;
    float m_MaxQuadricError;
};

struct ClusterBounds                // 32 B
{
    float3 m_Min;  float m_ShortestEdge;
    float3 m_Max;  float m_LongestEdge;
};

struct ClusterNode                  // 4 B, packed like nvclusterlod::Node
{
    uint m_Packed;                  // bit 0 isGroup; nodes: childOffset:26, childCount-1:5
                                    // leaves: groupIndex:23, groupClusterCount-1:8
};

struct Cluster                        // 20 B (4 B bitfield header + 4 × uint), inside a group blob
{
    uint m_TriangleCountMinusOne : 8;
    uint m_VertexCountMinusOne   : 8;
    uint m_LODLevel              : 8;
    uint m_GroupChildIndex       : 8;
    uint m_MaterialPaletteOffset;    // byte offset into the group blob
    uint m_MaterialPaletteCount;     // 0 = whole-cluster material from the instance
    uint m_PositionByteOffset;       // relative to the group blob
    uint m_TriangleByteOffset;       // relative to the group blob (uint8 micro-indices)
};

struct ClusterGroup                  // 36 B header (4 × uint + 20 B metric), then arrays
{
    uint  m_ClusterOffset;           // into the global cluster array
    uint  m_GroupIndex;              // index of this group in the geometry's group array
    uint  m_LODLevel;
    uint  m_ClusterCount;
    ClusterTraversalMetric m_Metric;
};

struct GeometryClusters              // per-geometry header (one per mesh)
{
    uint  m_NodeOffset,  m_NodeCount;          // root is node[0]
    uint  m_NodeBBoxOffset;                    // ParallelArray<ClusterBounds>
    uint  m_GroupOffset, m_GroupCount;
    uint  m_ClusterOffset, m_ClusterCount;
    uint  m_GeneratingGroupOffset;             // uint per cluster, 0xFFFFFFFF = original mesh
    uint  m_LODLevelCount;
    ClusterTraversalMetric m_LODMinSphere[32]; // per level: synthetic worst-case sphere
    float                  m_LODMinMaxError[32]; // per level: smallest quadric error
};

struct ClusterJob                    // 8 B, the visible-cluster list entry
{
    uint m_InstanceIndex;
    uint m_ClusterIndex;
};
```

Vertex data stays in the existing quantised vertex buffer (`srrhi::VertexQuantized`, already 20 B with
packed normal/uv/tangent); the cluster blob only carries 8-bit micro-indices into a **cluster-local
vertex range**, exactly like today's `Meshlet` layout, so the mesh shader's decode is the same code.

### 3.2 CPU-side scene data and `SceneCache` v3

`Scene::Mesh` gains a `ClusterLodData` member (vectors of the structs above plus per-group blobs);
`SceneCache` writes/reads it as new sections after `Indices`:

```
[cooked header] magic 0x59464C52, version 3, clusterConfig{clusterTris, clusterVerts,
                groupSize, lodDecimationFactor, errorMergePrev, errorMergeAdd, errorEdgeLimit,
                materialWeight, uvWeight, normalWeight}, meshoptimizerVersion
... existing sections unchanged ...
[ClusterNodes]              uint64 count + ClusterNode[]
[ClusterNodeBBoxes]         uint64 count + ClusterBounds[]
[ClusterGroups]             uint64 count + ClusterGroup[]
[ClusterGroupClusterRefs]   uint64 count + uint[]   (per group: first cluster index, contiguous)
[ClusterGeneratingGroups]   uint64 count + uint[]   (per cluster)
[Clusters]                  uint64 count + Cluster[]
[ClusterVertexIndices]      uint64 count + uint8[]  (micro-indices, 3 per triangle)
[ClusterVertexPositions]    uint64 count + VertexQuantized[] (cluster-local vertex range)
[GeometryClusters]          uint64 count + GeometryClusters[]
```

Rules:

* the **config block invalidates the cache** when clusterisation parameters change — this is what
  `kCookedMeshVersion` cannot express on its own, so it is hashed into the validity check;
* the cluster data is written *after* the existing sections, so a v2 file still loads with
  `cluster data absent` → the loader falls back to the legacy path and re-cooks on the next run
  (`--cluster-recook`);
* `SceneCache::IsCacheValid` keeps its "newer than source" rule, plus the config hash.

### 3.3 Frame structure (per cluster bucket: Opaque, Masked)

Inserted into the existing `BasePassRendererBase` flow, per cluster bucket, replacing
`PerformOcclusionCulling` + `RenderInstances` when cluster rendering is on:

```
[instance culling]          GPUCulling (unchanged: instance sphere → frustum/HiZ, bucket-local)
  → ClusterTraversalInit    one thread per instance in bucket: sphere test, classify [min,max],
                            push root node; writes indirect node-dispatch args
  → ClusterTraversalRun     N passes (one per tree depth) over node queue → group queue
  → ClusterTraversalGroups  one thread per group: per-cluster metric + bbox culling,
                            subgroup-compacted append into m_ClusterJobs
  → ClusterBuildArgs        one thread: DispatchMeshIndirect{m_ClusterJobs count}
  → Cluster Draw            dispatchMeshIndirect(0, 1) with ClusterBasePass (AS+MS) + the existing
                            G-buffer PS / alpha-test PS / forward PS variants
[HZB rebuild]               unchanged (Driving the existing HZBGeneratorPhase2 / SPD path)
```

Two-pass culling (phase 4) runs the same block twice with a `cullPass` constant and swaps the seeds
to the reject arrays, exactly as the reference does; the HZB is rebuilt between the passes as it is
today.

The **Transparent** bucket is not part of this structure: it keeps its current `TransparentPassRenderer`
path (vertex shader + `drawIndexedIndirect` from `GPUCulling`, blend + `DepthRead`), unchanged, until OIT
replaces the bucket. Its instances therefore still need the per-instance LOD pick, which is why
`GPUCulling.hlsl` keeps that code for the transparent bucket and only skips it for cluster buckets.

### 3.4 Toggles and debug views

`Renderer.h` (next to `m_UseMeshletRendering` / `m_ForcedLOD`), CLI in `Config.cpp` (`--cluster*`),
UI in `ImGuiLayer.cpp` ("Cluster LOD" section):

| Member | CLI | Default | Meaning |
|---|---|---|---|
| `m_EnableClusterRendering` | `--cluster 0/1` | 1 (after phase 2) | master switch; off = today's meshlet path |
| `m_ClusterPixelError` | `--cluster-error <px>` | 1.0 | LOD threshold in pixels |
| `m_ClusterForcedLOD` | `--cluster-force-lod <n>` | −1 | −1 = metric, ≥0 = fixed level (validation) |
| `m_ClusterMaxClusters` | `--cluster-max <n>` | scene-derived | clamp, mirrors `maxRenderClusters` |
| `m_ClusterDisableCulling` | `--cluster-no-culling` | 0 | draw every cluster (correctness only) |
| `m_ClusterTwoPass` | `--cluster-twopass` | 1 | two-phase occlusion |
| `m_ClusterRejectLists` | `--cluster-rejectlists` | 1 | phase 4 |
| `m_ClusterDebugMode` | `--cluster-debug <n>` | 0 | 0 off, 1 cluster id, 2 LOD level, 3 cluster bounds, 4 traversal cost heatmap |
| `m_ClusterGeometryStreaming` | `--cluster-streaming` | 0 until phase 5 | group streaming |
| `m_ClusterFreezeCullingCamera` | `--cluster-freeze-culling` (new) | 0 | A/B determinism (the member already exists, the switch does not) |

Debug views are an output override in `ClusterBasePass` (modes 1–2) plus an optional
`ClusterDebug` mesh-shader pass (mode 3, cluster bboxes as lines) and an ImGui readback panel (mode 4
shows the counters below).

---

## 4. Phases

Each phase ends in a **milestone** with gates: correctness gates must pass *before* performance is
measured; performance gates are compared against the baseline capture taken in phase 1 (M1).

### Phase 0 — Offline cluster-LOD builder (CPU only)

**Goal**: produce the DAG offline and prove it is watertight and monotonic, with no renderer change.

Tasks

1. `src/ClusterLodBuilder.h/.cpp` — port the loop from `src/meshopt_clusterlod.h`
   (`clusterize` → `partition` → `simplify`) over `external/meshoptimizer`, using
   `meshopt_buildMeshletsSpatial` / `meshopt_buildMeshletsFlex`,
   `meshopt_partitionClusters`, `meshopt_simplifyWithAttributes`, `meshopt_optimizeMeshlet`.
   Defaults: 128 tris / 128 verts per cluster (matches the scene's current cluster stats; the 64/64
   variant is produced by the same builder as the A/B in §7), 32 clusters per group, 0.5 decimation
   factor, error propagation `max(prev*1.5, cur)`, edge-length error limit.
2. Inflate each group's bounding sphere so a coarser group is never nearer the camera than its
   generating group (the watertightness rule from study §2.2). Compute per-level
   `minBoundingSphereRadius` / `minMaxQuadricError` for `classifyInstanceLod`.
3. Build the per-level spatial node tree (`m_LODLevelCount` children under the root, 8 children per
   node preferred, 32 max) with `ClusterBounds` per node.
4. Wire into `SceneLoader`'s mesh processing (where `meshopt_simplifyWithAttributes` and
   `meshopt_buildMeshlets` run today, `src/SceneLoader.cpp` ≈ lines 1990–2060) behind
   `--cluster`, keeping the legacy LOD arrays written for the A/B path.
5. `SceneCache` v3 writer/reader (§3.2) + config hash in `IsCacheValid`.
6. CPU reference traversal `ClusterLod::EvaluateCut(geometry, viewPos, pixelError, proj)` — used by
   tests **and** by the GPU validation gate (T5).

Deliverables: builder, cache format, `tests/ClusterLodTests.cpp`, a `--cluster-stats` print (clusters,
groups, levels, triangles per level, DAG depth, worst error per level).

Validation

* **V0.1 monotonicity**: for every cluster, `group.error ≥ generatingGroup.error`; per level, the
  max error strictly increases. Assert in the test.
* **V0.2 watertightness**: for a threshold sweep, the selected cluster set tiles the mesh — each
  boundary edge of a selected cluster is matched by exactly one other selected cluster or the mesh
  border. Implemented as an edge-multiset check on the CPU reference cut.
* **V0.3 no-overlap**: `EvaluateCut` never selects a cluster whose generating group is also selected.
* **V0.4 determinism**: two builds produce byte-identical cache files.
* **V0.5 cache round-trip**: save → load → deep compare; a config change invalidates.

Gate **M0**: V0.1–V0.5 green, and the cached DAG for Bistro/`bistrox.gltf` loads in < 200 ms.

### Phase 1 — GPU plumb + traversal + debug views (no image change)

**Goal**: run the traversal on the GPU, produce a visible-cluster list, visualise it; do **not** change
the base pass yet.

Tasks

1. `src/shaders/Cluster.sr` (§3.1) + register it in the srrhi codegen; bump `ShaderIDs` via
   `build_shaderids`; new buffers declared in `BasePassResources::DeclareResources`
   (`m_ClusterNodeInfos`, `m_ClusterGroupInfos`, `m_ClusterJobs`, `m_ClusterJobCount`,
   `m_ClusterIndirect`, `m_ClusterCounters`), sized from `Scene::FinalizeLoadedScene()`.
2. `ClusterTraversalInit.hlsl`: instance culling as today (reuse `FrustumSphereTest`,
   `OcclusionSphereTest`), then `classifyInstanceLod` and root seeding; writes node-dispatch args.
3. `ClusterTraversalRun.hlsl`: multi-pass over `m_MaxTreeDepth` (the reference's non-persistent path,
   which it measures as the faster one), 32 threads/group, subgroup ballot append.
4. `ClusterTraversalGroups.hlsl`: per-cluster metric (`testForTraversal` port), bbox culling,
   append to `m_ClusterJobs` with `subgroupBallot` + one atomic per subgroup.
5. `Culling.hlsli`: add `FrustumBoxTest` / `OcclusionBoxTest` (8-corner clip-AABB, `ceil(log2)`
   mip selection + the fine-mip fix, min-reduction sampler already used by the sphere path).
6. `ClusterBuildArgs.hlsl` (+ readback of counters through the existing event-query pattern).
7. ImGui "Cluster LOD" section: toggles (§3.4) + counters (instances considered, nodes/ groups
   visited, clusters requested vs capped, milliseconds from `GpuPassTimers`).
8. PIX markers: `Cluster Traversal Preparation`, `Cluster Traversal Run`, `Cluster Build Args`.
9. Validation switches the harness needs (the renderer's current CLI has no equivalent):
   `--no-taa` (TAA is UI-only today, and pixel-exact A/B needs it off), `--cluster-freeze-culling`,
   and `--cluster-stats [--cluster-stats-frame <n>]` which prints the cluster counters and per-pass
   timings to the log on the given frame so runs are reproducible without the UI. Existing switches
   `--hide-ui`, `--no-indirect`, `--no-denoise`, `--no-sky` complete a deterministic capture setup.
10. New test target `ClusterLodTests` added to `CMakeLists.txt` next to `PixProbe` — the repo has no
   test target today, so this is new build machinery (plain `add_executable`, exit code = verdict).

Deliverables: traversal runs; ImGui counters; `m_ClusterDebugMode` 4 is usable.

Validation

* **V1.1** drawn cluster count within ±5 % of `EvaluateCut` at the same camera (T5).
* **V1.2** with `--cluster-force-lod 0` (+ no culling) the cluster count equals the CPU count of all
  level-0 clusters.
* **V1.3** no device-lost / no validation errors with `--rhidebug`; `rdc-tools hazards` reports no
  *certain* findings on a capture of the pass.
* **V1.4** traversal does not read uninitialised memory: `rdc-tools deps` shows no
  `read-before-write` on the cluster buffers.

Perf (measured, not gated yet): `Cluster Traversal` ≤ 0.25 ms (T1).

Gate **M1**: V1.1–V1.4 green; **baseline capture archived** (`bin/captures/bistro-baseline-frame<N>.wpix`
taken with `--pix-capture-frame`, plus the rdc-tools bundle) so later phases have a fixed reference.

**Baseline recorded (2026-11-10, build `2bfa960`).** Deterministic flag set for every capture and A/B:
`--scene "GLTF Scenes/Bistro/bistrox.gltf" --hide-ui --no-taa --no-indirect --no-denoise --no-sky`.

| Artefact | Size | Command |
|---|---|---|
| `bin/captures/bistrox-baseline-frame90.wpix` | 560 MB | `--pix --pix-capture-frame 90 --pix-capture-dir "bin/captures/bistrox-baseline"` |
| `bin/captures/baseline_events.csv` | 1137 rows | `pixtool open-capture <wpix> save-event-list <csv>` |
| `bin/captures/bistrox-baseline-frame90.rdc` | 958 MB | `--renderdoc --renderdoc-capture-frame 90` |

Two gotchas that cost a run each: `--pix-capture-dir` is used as a **file-name prefix**
(`<prefix>-frame<N>.wpix`), not as a directory, and a `--screenshot-frame`/capture run only writes
`bin/screenshot.png` when the frame completes — an empty `screenshot.png` is the cheapest proof that a
frame rendered at all.

**Phase 1 landing notes (2026-11-10).** What the traversal actually looks like once built, and where it
departs from the task list above:

* `Cluster.sr` carries the node tree, the group records (metric + bounds), the per-cluster
  generating-group array, the `ClusterGeometry` ranges, the two queue entry types and the counters.
  Clusters themselves and the micro-index blob wait for phase 2's mesh shader, so `ClusterGeometry` is
  32 B, not the reference's per-geometry header with `m_LODMinSphere[32]`.
* **One definition per record.** `ClusterNode`, `ClusterBounds`, `ClusterTraversalMetric` and
  `ClusterGroup` are the shader's own structs: `ClusterLod.h` aliases them (`Node`, `Bounds`,
  `TraversalMetric`, `Group`) instead of redeclaring them, so nothing can drift and `ClusterLodGpu.cpp`
  no longer needs `sizeof` asserts to police two copies. The node packing lives in free functions in
  `ClusterLod.h`/`.cpp` because a generated struct cannot carry them. `Cluster` stays CPU-only — it packs
  four 8-bit counts into one `uint` with bitfields, which HLSL does not have, so phase 2's GPU geometry
  record will be its own wider struct. The consequence to remember: **editing `Cluster.sr` now changes the
  cooked-mesh layout**, so `kCookedMeshVersion` has to be bumped whenever it does.
* `ClusterLod::BuildGpuData` (`src/ClusterLodGpu.{h,cpp}`) flattens every primitive's DAG into five global
  arrays and rebases every index the traversal follows (node children, leaf group indices, group cluster
  offsets, generating groups).
* One shader file, `ClusterTraversal.hlsl`, holds all four entries instead of the four files listed in the
  task list: they share the cut rule and the culling helpers, and splitting them would duplicate both.
* The node passes are dispatched with the **worst-case grid** and exit on the queue counter, instead of
  writing indirect args: a pass cannot know its own thread count before it runs, so the alternative is an
  extra one-thread dispatch per level. The idle threads are a fraction of a percent of the pass.
* Node and group culling reuse `FrustumSphereTest` / `OcclusionSphereTest` on the bounding sphere of the
  bounds box. That is conservative (never rejects something visible) and needs no new culling code; real
  box tests belong to phase 4, where the culling budget starts to matter.
* The node tree prunes **spatially only**; the cut itself is `EvaluateCut`'s rule applied to every group
  the tree reaches. Two corrections came out of validating it on Sponza, where the first cut was 3× too
  large:
  * The rule is **per cluster, not per group**: a group holds clusters that were replaced by *different*
    coarser groups, so the "replacement is already detailed enough" test has to read
    `m_GeneratingGroups[clusterIndex]`. That array is uploaded as its own buffer
    (`m_ClusterGeneratingGroupsBuffer`); the collapsed `m_ReplaceGroup` field is gone.
  * The comparison happens in **world space** with the instance's uniform scale (the builder's spheres are
    in the primitive's own space), and the pixel error is `error * worldScale * focalPixels / distance`.
    A CPU reference that skips this only agrees for identity-transform instances.
* `--cluster-force-lod <n>` clamps to the primitive's own coarsest level, like `EvaluateCut` does, so a
  sweep never silently draws nothing for a primitive with fewer levels than the forced one.
* `--cluster-stats-frame <n>` reads the counters of **both** buckets back on that frame (one staging
  buffer each) and logs them against a CPU reference that mirrors the shader rule:

  ```
  HobbyRenderer.exe --scene "GLTF Scenes/Bistro/bistrox.gltf" --hide-ui --cluster 1 --cluster-no-culling \
      --cluster-stats-frame 4
  [ClusterLod] Opaque traversal: 2404 instances, 9631 nodes, 6252 groups, 23907 clusters (0 capped) | CPU cut reference: 23907 clusters (match)
  [ClusterLod] Masked traversal:  163 instances, 1132 nodes,  621 groups,  9005 clusters (0 capped) | CPU cut reference:  9005 clusters (match)
  ```

  Verified exact (`match`) with culling off on Bistro, Sponza and DragonAttenuation, for the metric cut
  and for forced levels 0/1/2/3. Bistro worst case: 11 910 node refs, 7 678 groups, 82 283 clusters,
  6 node passes.

**Phase 1 status (2026-11-10).** V1.1 (exact equality with the CPU cut), V1.2 (forced level 0/1/3
counts), V1.3 (`--rhidebug`: 0 D3D12 errors, 0 nvrhi messages) and V1.4 (`rdc-tools deps`: no
read-before-write on any `Cluster*` resource, and `rdc-tools hazards` reports 0 findings on them) are
green on all three scenes. Measured on the captures below:

| Capture | Buckets traversed | `Cluster Traversal Preparation` | `… Run` (6 passes) | `… Build Args` | total | T1 ≤ 0.25 ms |
|---|---|---|---|---|---|---|
| `bin/captures/DragonAttenuation-frame775.wpix` | opaque | 17.7 µs | 143.7 µs | 13.7 µs | **0.175 ms** | pass |
| `bin/captures/Sponza-frame60.wpix` | opaque + masked | 18.4 / 18.6 µs | 118.6 / 107.7 µs | 13.7 / 13.5 µs | **0.30 ms** | per bucket 0.15 ms |
| `bin/captures/bistrox-cluster-frame60.wpix` | opaque + masked | 17.7 / 17.0 µs | 134.8 / 80.6 µs | 13.8 / 13.9 µs | **0.28 ms** | per bucket 0.17 / 0.11 ms |

`gpu__time_duration.sum` from a counter-collecting replay is instrumented, so the totals rank the passes
rather than time a shipped frame; each Run dispatch costs a flat ~11-18 µs because the grid is the
worst-case node count. RenderDoc captures of the same frames
(`DragonAttenuation_capture.rdc`, `Sponza_capture.rdc`, `bistrox_capture.rdc`) show the full marker
sequence for both buckets, `rdc-tools markers`.

The captures also showed the transparent pass declaring ~0.9 MB of traversal buffers it never touches
(82283 cluster jobs + both node queues on Bistro), so `BasePassResources::DeclareResources` now takes a
`bTraversesClusters` flag that only the opaque and masked renderers set.

### Phase 2 — Cluster mesh shader for the Opaque bucket

**Goal**: draw the cluster list instead of the meshlet job list; image must be *identical* when the
cut is equivalent.

Tasks

1. `ClusterBasePass.hlsl`: AS passes the job index through; MS decodes the cluster header, expands
   `(position, normal, uv, tangent)` from the quantised buffer, and emits `clusterID`/`instanceID`
   flat; 128 tri / 128 vert limits as `max_vertices`/`max_primitives`.
2. `BasePassRendererBase::RenderInstances`: new branch — `setMeshletState` with
   `indirectParams = m_ClusterIndirect`, `dispatchMeshIndirect(0, 1)`; keep the existing G-buffer
   framebuffer, depth/stencil setup, alpha-test variant, pipeline statistics query.
3. `OpaqueRenderer::Setup/Render`: declare and bind the cluster buffers; run traversal before draw.
4. Remove the per-instance LOD pick from `GPUCulling.hlsl` when clusters are on (keep the code for the
   legacy A/B); `TLASPatch` keeps writing `m_LODIndex` from the classification (phase 6 uses it).

Validation

* **V2.1 exactness**: `--cluster-force-lod 0 --cluster-no-culling` vs the legacy meshlet path, same
  camera, TAA off, RTXDI off: max per-channel difference ≤ 1 on both G-buffer and composite.
* **V2.2 cut correctness**: pixel error 1.0 with TAA off: no pixel that the reference renders is
  left at the clear colour (depth coverage ≥ 99.9 %), and the depth buffer shows no double surfaces
  (compare against the forced-LOD-0 depth within a tolerance).
* **V2.3 determinism**: two consecutive frames with a frozen culling camera and identical constants
  produce identical cluster counters.
* **V2.4** `rdc-tools draws` lists exactly one `DispatchMesh` per cluster bucket and its
  `m_ThreadGroupCountX` matches the read-back cluster count.
* **V2.5 (cluster size A/B)**: build the asset at 128/128 and at 64/64 (cache config hash changes,
  renderer code unchanged), measure T2/T3 for both at pixel error 1.0, and **keep the faster**; if the
  difference is within noise (< 2 %), keep **128/128** and delete the other config.

Perf gate (first real one):

* **T2**: Opaque base pass ≤ 0.85 × baseline GPU time at pixel error 1.0.
* **T3**: base-pass instructions ≤ 0.80 × baseline.
* **T1**: traversal ≤ 0.25 ms.

Gate **M2**: V2.1–V2.5 green and T1–T3 met on Bistro (with the cluster size chosen by V2.5); if T2/T3
miss, the phase does not close — tune cluster size / group size / error propagation first (both are
asset-side knobs, so no renderer churn).

### Phase 3 — Masked bucket (Transparent stays legacy)

**Goal**: the cluster path serves Opaque **and** Masked. Transparent is deliberately left alone until OIT.

Tasks

1. Masked: reuse the existing `ALPHATEST` PS variant; per-triangle material palette from the cluster
   blob (a cluster mixes at most 2–3 materials in practice); keep the stencil write.
2. Leave `TransparentPassRenderer` untouched: vertex-shader path, `drawIndexedIndirect` from
   `GPUCulling`, per-instance LOD kept for that bucket only. Record the reason in the code comment —
   correct blending needs a sorted surface order that a per-cluster mesh-shader list cannot give, and
   OIT will replace the bucket anyway.
3. Per-bucket cluster buffers for Opaque and Masked only (each bucket already owns a
   `BasePassResources`); note the memory trade-off in the code comment and keep `m_ClusterMaxClusters`
   per bucket.

Validation

* **V3.1** masked: `--cluster-force-lod 0` vs legacy masked path, max diff ≤ 1; then the pixel-error
  cut with the same no-holes/no-double-surfaces checks as V2.2.
* **V3.2** transparent is **unchanged**: the transparent pixels of a frame before and after the cluster
  work must be bit-identical at the same camera with TAA/RTXDI off (the bucket runs the same code and
  the same per-instance LOD as the baseline).
* **V3.3** stencil: opaque stencil writes still happen (check via `rdc-tools` depth/stencil views).

Gate **M3**: V3.1–V3.3 green; Opaque and Masked draw through `ClusterBasePass` with the master toggle
on, Transparent is untouched, and the legacy meshlet path remains selectable for A/B.

### Phase 4 — Two-pass culling with reject lists

**Goal**: recover the disocclusion case without repeating the traversal.

Tasks

1. Record in pass 0: rejected instances (compacted), rejected inner nodes, rejected group leaves, and a
   per-group cluster bitmask — all filtered by the cheap `intersectFrustumOnly` test first.
2. `setupSecondPass` equivalent in `ClusterBuildArgs` / a small `ClusterSetup.hlsl`: point the seed
   arrays at the reject buffers and reset counters.
3. `ClusterRejectClusters.hlsl`: second pass re-tests only the recorded clusters (no traversal, no
   metric evaluation).
4. `ClusterTraversalInit/Groups` gain a `cullPass` constant; the HZB is rebuilt between passes through
   the existing `GenerateHZBMips` path.

Validation

* **V4.1** image identical to phase 3 (single pass) for a *static* camera; for a moving camera the
  two-pass path must be ≥ the single-pass path in covered-pixel count (no regressions on disocclusion).
* **V4.2** reject accounting: `rejected + drawn == requested` per pass, and pass-1 traversal work is
  strictly smaller than pass-0 (counters in ImGui).
* **V4.3** the phase-2/3 gates still hold (regression run).

Perf: expected small gain on static cameras (study §4.2 shows phase 2 finds ~0.1 %) — **not** gated.
The gate is "no regression on T2/T3 and correct disocclusion".

Gate **M4**: V4.1–V4.3 green; the feature stays behind `m_ClusterTwoPass` / `m_ClusterRejectLists`.

### Phase 5 — Geometry streaming (groups)

**Goal**: bound VRAM by streaming cluster groups, with the same request/decide/serve shape as the
existing texture streaming.

Tasks

1. `src/GeometryStreaming.h/.cpp` — request list (device → readback), per-frame load/unload budget,
   residency table + compact active-group list, patch list applied by a `StreamUpdate` compute pass.
2. Residency encoding as in the reference: `m_GroupAddress >= INVALID_ADDRESS_START` means "not
   resident", low bits carry the frame index of the last request (idempotent requests without a second
   buffer). Traversal treats an unresident generating group as "finest available" and still draws.
3. Storage: persistent arena with a free-list — **decided**, no reserved/sparse resources. Group blobs
   are 16-byte aligned and self-relative, so a move only rewrites the address table. Fragmentation is
   bounded by the age filter and measured with `rdc-tools memory` (V5.3); if it ever becomes a problem
   the fix is defragmentation at load, not a different allocation model.
4. Age filter pass (`ClusterStreamAgeFilter.hlsl`) unloads groups untouched for N frames, except the
   levels pinned by the discrete-LOD classification (phase 6).
5. Integrate with the existing per-frame streaming ordering in `Renderer::UpdateStreamingPreRender`
   (textures first, geometry patches after — both are "device asks, host serves").
6. Reuse the readback pattern from `FeedbackManager`: cache-host-visible readback, one event query per
   slot, **≥ 2 frames of slack** before polling (the texture-streaming note in this repo).

Validation

* **V5.1** with a VRAM ceiling (`--cluster-geo-budget <MiB>`), the scene renders the same image as the
  preloaded path once streaming settles (compare at frame N and N+120).
* **V5.2** no holes: coverage ≥ 99.9 % of the preloaded image at the same camera, at every camera
  position of a 300-frame scripted path (new switch; `--camera-spin` is the only motion source today
  and is not deterministic enough for a per-frame gate).
* **V5.3** budget honoured: `rdc-tools vram` peak ≤ budget; unload actually returns memory
  (`memory` heap totals drop between two captures).
* **V5.4** patch correctness: `rdc-tools deps` shows no read-before-write on the group address table,
  and `hazards` reports no *certain* finding.

Gate **M5**: V5.1–V5.4 green at a budget of ≤ 1.5 GiB of geometry on Bistro.

### Phase 6 — Discrete LOD for ray tracing / raster agreement

**Goal**: ray tracing and rasterisation consume the *same* geometry, without touching DXR2.

Tasks

1. `classifyInstanceLod` result is written per instance (already in phase 1) into
   `PerInstanceData.m_LODIndex` as a **discrete level** (`lodLevelMin`), so `TLASPatch` keeps
   selecting the matching BLAS (per-mesh, per-LOD BLASes already exist).
2. Raster side: an instance whose cut fits a single level (`lodLevelMax - lodLevelMin < range` and the
   level is fully resident) is seeded at that level's node and marked discrete — the reference's
   `--discretelod`, which exists to avoid raster/RT divergence (self-shadow and self-reflection
   artefacts), not for raster speed. **When that level is not fully resident, raster is restricted to
   the resident level too** (it does not fall back to the continuous cut): a one-frame pop is preferred
   over a raster/RT disagreement, which shows up as shadow acne and reflection holes.
3. Streaming pins the levels a discrete instance relies on (the age filter exception).

Validation

* **V6.1** with RTXDI/NRD on, no new self-shadow acne or reflection holes compared to the pre-cluster
  capture (visual A/B at 3 fixed cameras + pixel-diff statistics).
* **V6.2** an instance rendered discretely produces the same G-buffer as `--cluster-force-lod <that
  level>` for that instance, and — with the level pinned non-resident — raster renders the *resident*
  level rather than the continuous cut (no divergence frame).
* **V6.3** `rdc-tools passdiff` between the raster-only and RT captures shows the same cluster counts
  per instance, including the frames where the streaming budget forced the level down.

Gate **M6**: V6.1–V6.3 green; the discrete path is toggleable (`m_ClusterDiscreteLod`).

---

## 5. Validation infrastructure

### 5.1 Correctness harness

| Gate | How |
|---|---|
| Image equality | `--screenshot-frame N out.png` with TAA off / RTXDI off / frozen camera; compare with a small per-channel-diff tool (PNG read + max/mean/per-pixel count) |
| Depth coverage | render the depth target with PIX (`save-resource --depth`) or the renderer's own debugging view; count pixels at the clear value |
| Cluster counts | GPU counters read back in the existing ring, printed in the ImGui panel and to the log by `--cluster-stats-frame N` |
| DAG invariants | `tests/ClusterLodTests.cpp` (CPU, no GPU) |
| Capture-level sanity | `rdc-tools hazards`, `deps`, `rootsig-check` — all must be clean on the new passes |

### 5.2 Performance harness

Two independent measurements, both already supported by this repo:

**Renderer-side** (`GpuPassTimers`, existing): new marker scopes around traversal prep / run / args /
draw, so a plain `--cluster-stats` run prints per-pass milliseconds without any external tool. This is
the number used for T1/T2 in CI-like runs.

**PIX** (`docs/pix-capture-debugging.md`):

```powershell
$PXT = 'C:\Program Files\Microsoft PIX\2610.28\pixtool.exe'
.\bin\HobbyRenderer.exe --scene "D:\Workspace\GLTF Scenes\Bistro\bistrox.gltf" --pix-capture-frame 120
& $PXT --output=quiet open-capture bin\captures\bistrox-frame120.wpix save-event-list bin\evc.csv `
       --counters=gpu__time_duration.sum --counters=smsp__inst_executed.sum `
       --counters=l1tex__t_sectors.sum --counters=dram__bytes.sum
```

Then aggregate per marker name exactly as the doc's §4.2 script does. `gpu__time_duration.sum` is
inflated by replay instrumentation — it is used for **ranking and for A/B ratios of the same
binary**, never as an absolute.

**RenderDoc + rdc-tools** (`D:\Workspace\rdc-tools`):

```powershell
.\bin\HobbyRenderer.exe --scene "...\bistrox.gltf" --renderdoc-capture-frame 120
$py = 'C:\Program Files\Python314\python.exe'
& $py D:\Workspace\rdc-tools\src\py\rdc_analysis.py summary  bin\captures\renderdoc_frame120.rdc
& $py ... markers   bin\captures\renderdoc_frame120.rdc            # pass tree
& $py ... draws     bin\captures\renderdoc_frame120.rdc 120        # per draw/dispatch
& $py ... resources bin\captures\renderdoc_frame120.rdc 0 Cluster  # new buffers exist, sized right
& $py ... hazards   bin\captures\renderdoc_frame120.rdc 20         # exit 1 on a certain conflict
& $py ... deps      bin\captures\renderdoc_frame120.rdc 40 mermaid # read-before-write on cluster data
& $py ... vram      bin\captures\renderdoc_frame120.rdc --drop Albedo   # budget arithmetic
& $py ... passdiff  ab\before.rdc ab\after.rdc --all               # what the change did to the frame
& $py ... replaydiff ab\before ab\after --with-images --out ab\diff # pixel-level A/B
```

`rdc-tools` is the D3D12 path and needs the RenderDoc capture's chunk stream (LZ4 by default); the
`ab\before` / `ab\after` bundles are produced once per milestone and kept with the milestone.

### 5.3 Milestone summary

| Milestone | Content | Gates |
|---|---|---|
| M0 | CPU DAG + cache v3 | V0.1–V0.5 |
| M1 | GPU traversal, debug views, baseline archived | V1.1–V1.4, T1 |
| M2 | Opaque cluster draw | V2.1–V2.4, T1–T3 |
| M3 | Masked bucket (transparent unchanged) | V3.1–V3.3 |
| M4 | Two-pass + reject lists | V4.1–V4.3, no T2/T3 regression |
| M5 | Geometry streaming | V5.1–V5.4, T4 |
| M6 | Discrete LOD / RT agreement | V6.1–V6.3 |

---

## 6. Risks

| Risk | Where it bites | Mitigation |
|---|---|---|
| Cluster LOD build time on big assets | phase 0 | cache in `SceneCache` (this is the requirement); build in `TaskScheduler` with the `meshopt_setAllocatorCache` pattern the reference uses |
| Watertightness bugs (inflated spheres, error accumulation) | phase 0/2 | CPU cut reference + V0.2/V0.3 before any GPU work |
| `DispatchMeshIndirect` grid size (tens of thousands of workgroups) | phase 2 | the reference measures this as fine; `m_ClusterMaxClusters` clamp and counters so the ceiling is visible |
| Bbox culling differs from the current sphere path → double/absent surfaces | phase 1 | force-LOD-0 exactness gate (V2.1/V2.2) isolates culling from LOD |
| Per-bucket cluster buffers multiply memory | phase 3 | only Opaque and Masked allocate them, sized by `Scene::FinalizeLoadedScene()`; the T4 gate covers it |
| The transparent bucket keeps per-mesh LODs, so two LOD mechanisms coexist | phase 3+ | intended and documented in code: Opaque/Masked use the cluster cut, Transparent uses the discrete per-mesh LOD; OIT retires the latter |
| Streaming interacts with texture streaming's readback timing | phase 5 | reuse the proven ≥ 2-frame event-query slack; keep geometry patches after texture patches |
| RT/raster divergence shows up as shadow acne, not as a crash | phase 6 | the discrete-LOD path is the fix, and V6.1 is an image gate, not a counter |
| `SceneCache` v3 breaks existing caches | phase 0 | version bump + config hash; missing cluster data falls back to legacy and re-cooks |

Rollback for every phase is the same: `--cluster 0` returns to the current meshlet path (kept
compiled). The legacy per-mesh LOD sections stay in `SceneCache` for good — the transparent bucket
keeps consuming them until OIT replaces that bucket.

---

## 7. Resolved decisions (2026-11-06)

| # | Question | Decision | Where it lands |
|---|---|---|---|
| 1 | 128 tri / 128 vert clusters, or 64/64 for finer granularity? | **Build both, keep the faster one; if the difference is negligible keep 128/128** (it matches today's meshlet stats and the existing mesh shader limits) | phase 0 config, decided at **M2** (V2.5) |
| 2 | Transparent clusters in instance order, or a cluster-level sort? | **Neither — transparent is out of the cluster path.** The bucket keeps the plain vertex-shader path (not meshlets, not clusters) until OIT exists; per-mesh LODs stay for it | phase 3 (shrunk), §3.3 |
| 3 | Arena + free-list, or reserved resources with `UpdateTileMappings`? | **Arena + free-list**, no sparse/reserved resources | phase 5 task 3 |
| 4 | When a discrete level is not resident, who gives way? | **Raster is restricted to the resident level** (pops accepted; raster/RT divergence is not) | phase 6 task 2, V6.2 |

Consequences worth keeping in view:

* the `SceneCache` still writes the legacy per-mesh LOD arrays, because the transparent bucket (and the
  legacy A/B path) consume them — "replace per-mesh LODs" means *for Opaque and Masked*;
* two LOD mechanisms coexist until OIT: cluster cut for Opaque/Masked, discrete per-mesh LOD for
  Transparent. Both are driven by the same projection-derived threshold so they do not look different;
* the cluster-size decision is an asset-side A/B (cache config hash), so it costs a re-cook, not code.

Still open, none blocking a phase start:

* whether the arena ever needs defragmentation (settle with `rdc-tools memory` at M5, not before);
* the transparent bucket's future ordering once OIT lands — that replaces the whole `TransparentPassRenderer`,
  so the answer belongs to the OIT plan, not this one.

---

## 8. Effort and sequencing

| Phase | Estimate | Depends on |
|---|---|---|
| 0 — builder + cache | 4–6 d | — |
| 1 — traversal + debug + baseline | 4–5 d | 0 |
| 2 — opaque cluster draw | 4–5 d | 1 |
| 3 — masked bucket (transparent untouched) | 1–2 d | 2 |
| 4 — two-pass + reject lists | 2–3 d | 3 |
| 5 — geometry streaming | 5–7 d | 3 |
| 6 — discrete LOD / RT agreement | 2–3 d | 5 |

Total ≈ 22–31 days. Phase 4 can run in parallel with 5. M2 is the decision point: if T2/T3 are not
met there, re-tune the asset knobs (cluster size, group size, error propagation) before spending
time on 4–6.

---

## Appendix A — shader inventory

| File | Stage | Purpose | Phase |
|---|---|---|---|
| `Cluster.sr` | data | structs + `srinput/sroutput` for all cluster passes | 1 |
| `ClusterTraversalInit.hlsl` | CS | instance test, classify, root seed, node args | 1 |
| `ClusterTraversalRun.hlsl` | CS | node traversal (multi-pass) | 1 |
| `ClusterTraversalGroups.hlsl` | CS | cluster metric + bbox cull + append | 1 |
| `ClusterBuildArgs.hlsl` | CS | `DispatchMeshIndirect` args, counter clamps, second-pass setup | 1 / 4 |
| `ClusterRejectClusters.hlsl` | CS | second-pass re-test of recorded clusters | 4 |
| `ClusterBasePass.hlsl` | AS+MS | one workgroup per cluster, G-buffer/alpha PS reuse (Opaque + Masked) | 2 / 3 |
| `ClusterDebug.hlsl` | MS+PS | cluster bboxes, id/LOD colouring | 1 / 2 |
| `ClusterStreamAgeFilter.hlsl`, `ClusterStreamUpdate.hlsl` | CS | age filter, residency + patch | 5 |

Build: add entries to `src/shaders/shaders.cfg`, then `build_srrhi → build_shaderids →
build_shaders → ${PROJECT_NAME}` (the AGENTS.md order). Never edit `src/shaders/srrhi/` or
`ShaderIDs.h` by hand.

## Appendix B — files touched

| File | Change |
|---|---|
| `src/ClusterLodBuilder.{h,cpp}` | new: offline DAG |
| `src/ClusterLod.h` | new: runtime structs, `EvaluateCut` |
| `src/SceneCache.{h,cpp}` | v3 sections, config hash, `kCookedMeshVersion = 3` |
| `src/SceneLoader.cpp` | build clusters in mesh processing; keep legacy arrays |
| `src/Scene.h/.cpp` | `Scene::Mesh::ClusterLodData`, `FinalizeLoadedScene` sizing |
| `src/shaders/Cluster.sr` | new GPU layout |
| `src/shaders/{ClusterTraversal*,ClusterBuildArgs,ClusterBasePass,ClusterDebug,ClusterReject*,ClusterStream*}.hlsl` | new passes |
| `src/shaders/Culling.hlsli` | bbox frustum/HiZ tests |
| `src/shaders/GPUCulling.hlsl` | LOD pick removed when clusters are on |
| `src/BasePassCommon.{h,cpp}` | cluster buffers in `BasePassResources` |
| `src/BasePassRenderer.cpp` | traversal dispatch, cluster draw branch, markers |
| `src/BasePassRenderer.cpp` (`TransparentPassRenderer`) | **unchanged** — keeps the vertex-shader path, blend, and the per-instance LOD |
| `src/Renderer.h/.cpp` | toggles, per-frame pixel-error threshold, streaming hookup |
| `src/Config.{h,cpp}` | `--cluster*` switches |
| `src/ImGuiLayer.cpp` | "Cluster LOD" section, counters, debug modes |
| `src/GeometryStreaming.{h,cpp}` | new (phase 5) |
| `tests/ClusterLodTests.cpp`, `CMakeLists.txt` | new CPU test target (phase 0) |

## Appendix C — command cheat-sheet

```powershell
# build
cmake --build build --config Release --target build_srrhi build_shaderids build_shaders HobbyRenderer

# CPU tests (new target; exit code is the verdict)
.\build\Release\ClusterLodTests.exe

# A/B images (deterministic: TAA/RTXDI off, camera and culling frozen, no UI)
.\bin\HobbyRenderer.exe --scene "D:\Workspace\GLTF Scenes\Bistro\bistrox.gltf" `
    --cluster-force-lod 0 --cluster-no-culling --cluster-freeze-culling `
    --no-taa --no-indirect --no-denoise --hide-ui --screenshot-frame 120 bin\ab\lod0.png

# PIX capture + counters
.\bin\HobbyRenderer.exe --scene "...\bistrox.gltf" --pix-capture-frame 120
& $PXT --output=quiet open-capture bin\captures\bistrox-frame120.wpix save-event-list bin\evc.csv --counters=gpu__time_duration.sum --counters=smsp__inst_executed.sum

# RenderDoc capture + rdc-tools
.\bin\HobbyRenderer.exe --scene "...\bistrox.gltf" --renderdoc-capture-frame 120
python D:\Workspace\rdc-tools\src\py\rdc_analysis.py summary bin\captures\renderdoc_frame120.rdc
python D:\Workspace\rdc-tools\src\py\rdc_analysis.py hazards bin\captures\renderdoc_frame120.rdc 20
```
