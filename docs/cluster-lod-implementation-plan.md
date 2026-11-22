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
UI in `ImGuiLayer.cpp` ("Level of Detail" section):

| Member | CLI | Default | Meaning |
|---|---|---|---|
| `m_EnableClusterRendering` | `--cluster-draw 0/1` | 1 | which path draws; the panel's "Enable Cluster LOD" checkbox owns it at runtime |
| `Config::m_EnableClusterLod` | `--cluster 0/1` | 1 | load-time half: cooks the cluster DAG into the cooked mesh. A scene cooked with `--cluster 0` has no cluster data, so the panel's checkbox is disabled for it |
| `m_ClusterPixelError` | `--cluster-error <px>` | 1.0 | LOD threshold in pixels |
| `m_ClusterForcedLOD` | `--cluster-force-lod <n>` | −1 | −1 = metric, ≥0 = fixed level (validation) |
| `m_ClusterMaxClusters` | `--cluster-max <n>` | scene-derived | clamp, mirrors `maxRenderClusters` |
| `m_ClusterDisableCulling` | `--cluster-no-culling` | 0 | draw every cluster (correctness only) |
| `m_ClusterTwoPass` | `--cluster-twopass` | 1 | two-phase occlusion |
| `m_ClusterRejectLists` | `--cluster-rejectlists` | 1 | phase 4 |
| `m_ClusterDebugMode` | `--cluster-debug <n>` | 0 | 4 = traversal counters; 1-3 select Debug Modes 12-14 |
| `m_ClusterGeometryStreaming` | `--cluster-streaming` | 0 until phase 5 | group streaming |
| `m_ClusterFreezeCullingCamera` | `--cluster-freeze-culling` (new) | 0 | A/B determinism (the member already exists, the switch does not) |

Debug views are an output override in `ClusterBasePass` (modes 12–13, colouring the clusters the pass draws),
a `ClusterDebug` line-topology mesh pass (mode 14, cluster bboxes over the shaded image) and an ImGui
readback panel (`m_ClusterDebugMode` 4 shows the counters). `--cluster-draw 0` keeps the legacy meshlet draw while the
cluster data stays cooked, so an A/B of the two paths runs on one cache; `--force-lod` and `--debug` mirror
the panel's LOD and debug-view controls for scripted runs.

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
7. ImGui "Level of Detail" section: toggles (§3.4) + counters (instances considered, nodes/ groups
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

Both are in place: the property grid has a **Cluster LOD** section (`ImGuiLayer.cpp`) carrying the §3.4
switches (pixel error, forced level, cluster cap, no-culling, freeze-culling) and the per-bucket counters,
and `--cluster-debug 4` opens it and keeps it live without the mouse. The counters are copied every frame
only while the section is open and read one frame later through `SignalEventAfterFrame`, so nothing stalls;
the `--cluster-stats-frame` path keeps its wait-for-idle readback because that one is a one-shot
measurement compared against the CPU cut. Modes 1-3 stay open until phase 2 draws clusters.

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

**Phase 2 landing notes (2026-11-12).** What the draw actually looks like, and the two bugs the exactness
gates found:

* `Cluster.sr` gained `ClusterRecord` (24 B: triangle/vertex counts, LOD level, group index, and element
  offsets into the two new shared arrays) plus, on the CPU side, `GpuData::m_Records`, `m_Triangles` (one
  packed micro-index triple per triangle) and `m_Vertices` (scene-wide vertex indices: the cooked clusters
  index their primitive's own vertex block, so `Scene::Mesh::m_Primitives[pi].m_VertexOffset` is added once,
  at load). None of them are cooked-mesh records — they are built from the cooked `Cluster` in
  `ClusterLodGpu.cpp`, so `kCookedMeshVersion` is untouched.
* `ClusterBasePass.hlsl` is AS + MS only; the G-buffer / alpha-test pixel shaders are the base pass's own.
  The vertex output (`VSOut`, `PrepareVSOut`, the per-frame CB and the instance/vertex buffers) moved to
  `BasePassCommon.hlsli` so the vertex path, the meshlet path and the cluster path share one definition of
  it — a new interpolant (the `clusterID` the debug views read) lands in one place.
* `BasePassInputs` carries the five cluster buffers (jobs, records, triangles, vertices, groups). They are
  bound to dummies when the cluster path is not drawing, which keeps one binding layout for every base-pass
  pipeline instead of two.
* The job list now allocates **one slot per kept cluster**. Reserving a whole group's range up front (what
  phase 1 did) left holes where the cut rule skipped a cluster, and phase 1 only *counted* the list, so
  nothing noticed; the draw reads the list by index, so a hole is a garbage cluster. The counter and the
  list length now always agree.
* The second culling phase still runs when clusters draw; only its draw is skipped. It is what refreshes the
  per-instance LOD that `TLASPatch` patches the ray-tracing structures with, and leaving it out makes the
  raster and ray-traced paths disagree (the plan's phase 6 topic, visible immediately as different shadow
  acne).
* **The traversal transformed the instance sphere by the instance's world matrix.** `PerInstanceData`'s
  `m_Center`/`m_Radius` are already world space — `GPUCulling.hlsl` tests them against the view matrix alone
  — so every instance that is not near the origin was culled, and on Bistro that was 66 % of the frame.
  Sponza and the dragon hid it because their instance transforms are close to identity, and phase 1 only
  validated cuts with `--cluster-no-culling`, which skips the test entirely. The sphere test is now split
  into `SphereVisibleWorld` (instance, already world space) and `SphereVisible` (node/group bounds, in the
  primitive's space). The CPU reference in `CountReferenceCutClusters` deliberately tests no culling at all,
  so `--cluster-stats-frame` is only meaningful together with `--cluster-no-culling`.
* **The builder split a finer group across several coarser groups, and the cut rule cannot survive that.**
  `meshopt_partitionClusters` partitions individual clusters, so the decimated output of one level-L group
  routinely landed in two to six level-(L+1) groups. `m_GeneratingGroups` can only record one replacement per
  cluster, so the cut dropped a finer group as soon as *that* group was detailed enough - while the parts that
  went to the other coarser groups were still too coarse and were skipped too. The result was bands of missing
  surface that appeared and moved as the camera changed the cut, worst at medium distance (66 % of a Bistro
  frame disappeared at one point for a different reason, see the sphere fix above, and the DragonAttenuation
  floor lost stripes at the `captures/DragonAttenuation_capture {near,medium,far}.rdc` cameras). Phase 0 could
  not see it: `ValidateCut` walks the *selected* clusters' edges, so a region that no cluster covers has no
  edges to check.
  Fix: `AssignGroups` merges the partitions that share a finer group (union-find keyed by `m_SourceGroup`) and
  chunks whole finer groups, so a group's replacement is a function. `ValidateDag` now refuses a DAG where one
  group's clusters point at different replacements, and `kCookedMeshVersion` is 4 so every cooked cache re-cooks
  once. The side effect is fewer, larger groups (DragonAttenuation: 251 groups → 74) and a slightly coarser cut
  (Bistro: 23 907 → 24 439 clusters at the same camera).
* Debug views: they are `DEBUG_MODE_CLUSTERS` / `DEBUG_MODE_CLUSTER_LOD` / `DEBUG_MODE_CLUSTER_BOUNDS`
  (12-14) in `Common.sr`, i.e. entries of the general Debug Mode combo, so `Renderer::HandleDebugModeSettings`
  applies to them the way it does to every other view (bloom and auto exposure off, exposure locked) and the
  deferred pass knows to pass them through unlit. `--cluster-debug 1|2|3` still selects them, it now maps onto
  those Debug Modes; `--cluster-debug 4` stays the traversal's counter readback, which is not a view.
  Mode 12 hashes the cluster index into the albedo target, 13 ramps over the cluster's LOD level (32 levels,
  not the 8 meshlet colours), 14 draws the group bounds the cut and culling used as wireframe boxes through a
  second, line-topology mesh pipeline in the same framebuffer (depth test read-only, so the HZB built from the
  same depth stays honest) and colours them into the emissive target, which the deferred pass adds back for
  that mode alone. 15 is the per-triangle view: a mesh shader writes each vertex once and indexes it from its
  primitives, and a pixel shader in a mesh pipeline is not given `SV_PrimitiveID`, so the triangle index cannot
  ride the base pass; instead a mesh-shader overlay re-emits every visible cluster's triangles with each corner
  written separately, two meshes per cluster so a 128-triangle cluster stays inside the 256-vertex output limit,
  and hashes the triangle index into the albedo target with the depth test read-equal and the depth write off.
* Validation harness: `--debug <n>` selects a base-pass debug view from the command line, `--force-lod <n>`
  forces the legacy meshlet path's LOD, `--cluster-draw <0|1>` keeps the cooked cache while switching which
  path draws, and `--no-rt-shadows` isolates the draw path from the ray-traced shadow noise. A pixel A/B must
  not use `--cluster-stats-frame`: its wait-for-idle readback changes the frame timing enough that texture
  streaming and the auto-exposure history diverge, and the same configuration then differs by up to 219.

**Phase 2 status (2026-11-12).** Measured on the Debug build, 1080p, TAA off, `--no-indirect --no-denoise
--no-sky`, frame 40, compared against `--cluster-draw 0 --force-lod 0`:

| Scene | V2.1 albedo G-buffer, forced level 0 | V2.2 metric cut: coverage / holes |
|---|---|---|
| DragonAttenuation | max 0 on every channel | 100.000 % / 0 |
| Sponza | max 0 on every channel | 100.000 % / 0 |
| Bistro | 205 px differ (0.010 %), max 133 | 99.999 % / 14 px (0.001 %), 16 px extra |

Dragon and Sponza are bit-identical at forced level 0, and so is the *composite* (max per-channel diff 2,
which is the run-to-run noise floor of this renderer: the same configuration twice also differs by 2 on
~70 % of the pixels). Bistro's residual is the alpha-tested bucket: the meshlet path cone-culls a meshlet
that faces away, the cluster path draws it, and where an alpha test discards the front face the back face
shows through — 205 pixels in the albedo view and 14 single-pixel holes on the silhouette. Everything else
(opaque, and the masked bucket where the leaf faces the camera) matches exactly.

The cut is hole-free across distance as well: with the DragonAttenuation camera moved to 35 %, 65 % and 100 %
of its saved ray (`bin/captures/DragonAttenuation_capture {near,medium,far}.rdc`), the cluster path and the
meshlet path at LOD 0 cover the same pixels (808 797 / 240 677 / 95 143 lit on both) with 0 interior holes,
and differ on 0.014 % / 0.027 % / 0.001 % of the screen — LOD-boundary edge pixels, not missing surface.

### LOD generation is meshoptimizer's, and a triangle soup gets a chain

The builder (`ClusterLodBuilder.cpp`) forms its working vertex buffer the way meshoptimizer's own
`clusterlod` example does: `meshopt_generateVertexRemapMulti` (positions + normals + uvs) or, for a primitive
without attributes, `meshopt_generateVertexRemap` on the positions, followed by `meshopt_remapVertexBuffer`
and `meshopt_remapIndexBuffer`. That pass drops the vertices the index buffer never references and merges the
ones whose attributes are all identical, and it is what makes level 0 share edges at all. The inverse of the
remap is kept because the emitted clusters index the primitive's own vertex block.

Simplification is `meshopt_simplifyWithAttributes` with `Sparse | ErrorAbsolute | ErrorClamped | Permissive |
LockBorder | Regularize`. Permissive mode is load-bearing: the simplifier classifies a vertex that sits on an
attribute discontinuity as a seam and refuses to collapse it, so a primitive whose every corner carries its own
normal has nothing collapsible at all and its chain stops at level 0 - which is exactly what a scanned mesh like
`GLTF Scenes/bunny/scene.gltf` (208 353 vertices, 34 834 distinct positions, 69 451 distinct normals) is.
Attribute seams are **not** protected (`meshopt_SimplifyVertex_Protect` stays clear): marking every uv seam
protected on a per-corner-uv primitive protects nearly every vertex, which turned permissive mode back into a
no-op and pushed 19 of 25 groups at level 1 into the sloppy path. The attribute weights in the error metric are
what keep uv distortion down instead.

A group that keeps more than `Config::m_SimplifyThreshold` (0.85) of its triangles is **stuck**: it stays a
terminal group, its clusters keep `kNoGroup` as their replacement, and the cut draws them at any distance. That
is the reference's own rule (`simplified.size() > merged.size() * simplify_threshold` -> terminal group with
`error = FLT_MAX`), and it is what keeps the coarse end honest: forcing every group to keep halving is what fed
the topology-free `meshopt_simplifySloppy` fallback, and sloppy is topology-blind, so it produced the spikes and
the blobby silhouette the bunny showed at levels 3-5. The sloppy fallback now exists behind
`Config::m_bSloppyFallback` and is **off**.

`meshopt_SimplifyRegularize` is on: with the group borders locked, the interior collapses tend to fan into
slivers, and even density is worth the small triangle-count cost. On the bunny it also lengthened the chain from
5 to 7 levels and lowered the error of the middle levels (level 3: 0.039 -> 0.013) - the bunny now runs 1 654
clusters / 69 451 triangles down to 22 clusters / 1 499 triangles in a single group.

`--cluster-force-lod <n>` is a **level comparison**, not an equality test: a cluster is drawn when its group is
at or under level `n` and no coarser group at or above `n` replaced it. An equality test leaves holes once a
group is terminal, because the forced level then does not exist for that region (that is what the triangle view
showed first: black wedges at forced level 4).

The bunny's cut passes the edge check at every level and follows the camera: 1 654 clusters at the saved camera
(all level 0), 259 at 8x the distance and 160 at 32x, matching the CPU cut in all three.

`rdc-tools` on a RenderDoc capture of the Bistro frame (`bin/captures/bistrox_capture.rdc`): the marker tree
carries `Cluster Traversal Preparation` / `Run` / `Build Args` for both buckets followed by
`Base Pass Render (Phase 1)` with a single dispatch, `deps` reports no read-before-write on any
`*_Cluster*` resource, and `hazards` reports 8 findings, all of them the pre-existing HZB and
RTXDI/OpaqueColor ones (none on a cluster buffer). PIX emits "Invalid/Uninitialized descriptor used in command
list event" on this scene (the capturer's sampler-feedback blind spot, see `docs/pix-capture-debugging.md` §6)
but the capture is still written and replays cleanly, so it is usable.

What the traversal costs, from the panel's own GPU timer queries at the Bistro camera above: **0.31 ms** for
both buckets (opaque 14 279 clusters, masked 5 421, culling on the previous frame's HZB). Frame rate on the
Debug build at the same camera is 57.5 fps with clusters against 68.0 fps on the legacy meshlet path — but
Debug is CPU-bound (17.4 ms CPU per frame), and the cluster path deliberately drops the meshlet path's cone
culling and its per-meshlet sort, so this is a shape check, not T2. A trustworthy T2/T3 needs the PIX capture
above or a Release run with the GPU timers, which is the first item of phase 3's follow-up work.

### Phase 3 — Masked bucket (Transparent stays legacy) — **landed**

**Goal**: the cluster path serves Opaque **and** Masked. Transparent is deliberately left alone until OIT.

Tasks

1. Masked: reuse the existing `ALPHATEST` PS variant and keep the stencil write. The per-triangle material
   palette the task originally called for turned out to be unnecessary: a cluster belongs to exactly one
   instance, and the base pass already resolves that instance's material per draw, so a mixed-material cluster
   cannot arise. `RenderInstances` picks `psID = ALPHATEST` when `ALPHA_MODE_MASK` and feeds the same
   `renderState` (stencil replace, ref 1) to `clusterPipelineDesc` as to the legacy pipeline.
2. `TransparentPassRenderer` is untouched apart from the comment that records why: vertex-shader path,
   `drawIndexedIndirect` from `GPUCulling`, per-instance LOD kept for that bucket only, because correct
   blending needs a sorted surface order that a per-cluster mesh-shader list cannot give and OIT will replace
   the bucket anyway. `bDrawClusters` is `g_Renderer.m_EnableClusterRendering && !bUseAlphaBlend`, so the
   cluster block is dead for `ALPHA_MODE_BLEND`.
3. Per-bucket cluster buffers. `Scene` carries `m_MaxClusterJobCount` / `m_MaxClusterGroupCount` /
   `m_MaxClusterNodeCount` **per bucket** (`kClusterBucketOpaque`, `kClusterBucketMasked`), computed in
   `FinalizeLoadedScene()` by walking both bucket infos and clamped with `std::max(count, 1u)`. Each bucket
   sizes its own structured buffers from its own worst case, so a bucket cannot borrow another's headroom - the
   trade-off is more total memory for exact capacity. `m_ClusterMaxClusters` therefore stays a *global debug
   clamp* rather than a capacity: each bucket's job buffer is already exact. `DeclareResources` takes the
   bucket index (`-1` = legacy/no cluster buffers) and `PerformClusterTraversal` derives it once from
   `args.m_AlphaMode`.

Validation (Debug build, 1080p, TAA/RTXDI/denoise/sky off, frame 40, `ab_scene.ps1`)

* **V3.1** masked, forced level 0 against the legacy path: Sponza **0 px differ** (max 0); Bistro
  **205 px = 0.0099 %** of 2.07 M, max channel delta 133; FlyingWorld (5 BLEND materials) 23 px = 0.0011 %,
  max 177. The residue is on masked foliage edges where the two paths' triangle order differs under
  `ALPHATEST`, not a missing or doubled surface.
* **V3.1b** counters agree per bucket: Sponza opaque 1 911 / masked 303 and Bistro opaque 24 929 /
  masked 9 210, GPU and CPU cut both `(match)`.
* **V3.2** transparent is unchanged. It cannot be measured as a pixel test in this harness (the shared
  lighting noise floor is ±1 on ~10 % of pixels even with both runs forced to level 0, and the opaque
  G-buffer feeds the transparent depth test). The defensible evidence: `git diff` touches
  `TransparentPassRenderer` only in a comment; `bDrawClusters` is false for `ALPHA_MODE_BLEND`; and `deps` on
  the cluster capture shows the traversal writing only `*_Cluster*` buffers - `TransparentPass_VisibleIndirect`
  is written by `GPUCulling` alone (5 times) and `Scene_InstanceLODBuffer` is not written by any cluster
  dispatch.
* **V3.3** stencil. `rdc-tools` cannot read depth-stencil state (`psos` prints kind/shader hashes only, and the
  PSO descriptors are not in the chunk stream it decodes), so this closes on consumer evidence instead: the
  deferred lighting pass draws with `stencilEnable`/`StencilFunc::Equal`/`refValue = 1`, i.e. a masked pixel
  whose stencil was not written renders *unlit*, and Bistro's 9 210 masked clusters land in the 0.0099 % of
  V3.1. `stencilEnable` is also part of the PSO cache hash, so a cluster PSO with and without the stencil
  write can never alias.

PIX, `bin/captures/bistro-frame40.wpix` (520 MB, `--pix-capture-frame 40`, this build): replays with exit 0;
the event list holds `Cluster Traversal Preparation` / `Run` / `Build Args` for **both** buckets, then
`Base Pass Render (Phase 1) - Opaque`, `- Masked` and `- Transparent`. Hardware counters put the whole
cluster traversal (all four dispatch kinds, both buckets, Bistro) at **0.289 ms** of a 37.6 ms timed frame:
`Run` 0.172, `Groups` 0.071, `Init` 0.027, `BuildArgs` 0.020. For scale in the same frame: `Opaque Renderer`
6.43 ms, `Base Pass Render (Phase 1) - Opaque` 2.92 ms, `MaskedPass` 0.92 ms, `TransparentPass` 0.58 ms.

Two `pixtool` gotchas found while doing this, both now in `docs/pix-capture-debugging.md` §6: an unsupported
counter name in the request (e.g. `gpu__cycles_elapsed.avg.per_second`) silently empties **every** counter
cell of the CSV, and PIX writes a leading space in every header and cell, which `Import-Csv` tolerates and
Python's `csv` does not.

Gate **M3**: green. Opaque and Masked both draw through the cluster path with the master toggle on,
Transparent is untouched, and the legacy meshlet path is still selectable for A/B.

### Phase 4 — Two-pass culling with reject lists — **landed**

**Goal**: recover the disocclusion case without repeating the traversal.

Tasks — as built

1. The first pass records what occlusion (and only occlusion) rejected: instance roots, inner nodes and
   group leaves, into `RejectNodes` / `RejectGroups` with their own counters. The frustum test is evaluated
   first and a frustum rejection is never recorded, because it can only be rejected again next pass.
   `SphereVisible`/`SphereVisibleWorld` became `SphereTests`/`SphereTestsWorld`, which report the two
   verdicts separately.
2. The second pass seeds the node queue from `RejectNodes` inside `ClusterTraversalInit` under
   `m_CullPass != 0` — no `ClusterSetup.hlsl`, and the kernels are shared rather than duplicated into
   `ClusterRejectClusters.hlsl`. A rejected group needs no seeding: in pass 1 the group queue *is* the
   reject group list, and the node passes append to the same list, which the group pass then walks.
3. `ClusterTraversalConstants` gained `m_CullPass` and `m_EnableRejectLists`; `ClusterCounters` traded its
   three pad words for `m_RejectNodeCount`, `m_RejectGroupCount` and `m_SecondPassClusterCount`. The HZB is
   rebuilt by the existing `GenerateHZBMips` call, which already sat between the two passes.
4. Each pass appends to its own job list, so the first pass' `DispatchMesh` is free to consume what it wrote
   while the second pass fills `ClusterJobsSecond`. `ClusterIndirectArgs` grew from one grid to three: slot 0
   the first pass' mesh dispatch, slot 1 the second pass' seed grid, slot 2 its mesh dispatch. `ClusterBuildArgs`
   writes slot 0 and slot 1 in pass 0 and slot 2 in pass 1.
5. One switch, `m_ClusterTwoPass` (`--cluster-two-pass <0|1>`, panel: **Two-pass culling**), not the plan's two:
   the reject lists have no meaning without the second pass. Default off — it changes no pixels, so it stays a
   tool until something needs it.

Validation

* **V4.1** static camera: forced level 0, two-pass against single pass, albedo G-buffer is **0 differing
  pixels** on Sponza and Bistro. Moving camera (`--camera-spin`, 15°/frame, frame 46): 0 differing pixels,
  0 holes, 0 extra — the second pass adds clusters (196 on Bistro) that this frame's depth still hides, which
  is the property that matters: it can add surface, never shade one twice.
* **V4.2** at that spin: pass 0 rejected 643 nodes, the second pass re-tested 730 nodes and 80 groups
  (against 1 904 nodes / 1 257 groups in pass 0, i.e. 38 % and 6 % of the work) and revived 196 clusters of
  8 368 drawn. Static camera: 0 rejected, 0 revived, and the second pass costs one indirect dispatch with an
  empty grid.
* **V4.3** the phase-2/3 gates hold on the phase-4 build: Sponza 0 px, Bistro 4 px (0.0002 %), 100 % cut
  coverage with 0 holes on both, and every bucket's counters still `(match)` the CPU cut.

Two harness rules learned the hard way, both encoded in `bin/ab_phase4.ps1`:

* `bin/camera_state.json` restores the last pose per scene, so a spinning A/B run starts wherever the last
  run ended. Delete it before each shot, or the two runs are not looking at the same view. `--camera-spin` now
  also steps by a fixed 1/60 s instead of the measured frame time, so the same frame means the same pose.
* On a streamed scene the mip state follows the frame time, so a slower build shows a texture difference that
  looks like a draw-path bug: Bistro at forced level 0 differed on 28 % of pixels until
  `--disable-sampler-feedback` pinned the mips, after which it is exactly 0.

Perf: expected small gain on static cameras (study §4.2 shows phase 2 finds ~0.1 %) — **not** gated.
The gate is "no regression on T2/T3 and correct disocclusion".

What it costs, from PIX hardware counters (Bistro, frame 60, same camera, Debug build): the second pass adds
**0.165 ms** of traversal plus a 0.031 ms mesh dispatch, i.e. 0.14 ms on a 39.3 ms frame (~0.4 %). The
single-pass traversal is 0.275 ms.

### Where the cluster path stands against the legacy meshlet path

Measured, not assumed: the cluster path is **not yet faster** than the meshlet path, and the reason is
visible in the counters.

| Bistro, forced level 0 (same triangles, same image ±205 px) | legacy meshlet | cluster, 1 pass | cluster, 2 pass |
|---|---|---|---|
| own frame | 24.90 ms | 29.04 ms | 30.14 ms |
| `Base Pass Render (Phase 1) - Opaque` | 1.93 ms | 2.41 ms | 2.41 ms |
| `Opaque Renderer` (inclusive) | 4.18 ms | 5.47 ms | 5.79 ms |
| texture sectors, base pass | 98 962 k | 113 939 k | 113 939 k |
| instructions, base pass | 301 M | 355 M | 355 M |

At its own metric cut the cluster path draws more detail than the meshlet path's per-instance LOD, so the
end-to-end gap there is larger (38.2 ms against 33.6 ms).

### Why the cluster path is slower: measured

Both suspects are wrong, and the counters say so. `--cluster-stats-frame` now also prints the opaque
renderer's pipeline statistics (the query is armed on that renderer for a scripted run), so one line per run
says how much geometry each draw path actually pushed. Bistro, the saved camera, frame 40:

| | meshlet primitives | mesh-shader threads | pixel invocations | `Opaque Renderer` |
|---|---|---|---|---|
| meshlet, automatic LOD | 90 311 | 158 304 | 8 448 851 | 4.18 ms |
| meshlet, forced LOD 0 | 96 896 | 166 752 | 8 412 428 | 4.18 ms |
| cluster, automatic cut | 395 054 | 500 608 | 9 213 939 | 5.47 ms |
| cluster, forced level 0 | 405 128 | 511 232 | 9 214 053 | 5.47 ms |
| cluster, all culling off | 3 092 047 | 3 778 304 | 9 255 404 | — |

1. **It is not the extra dispatches.** The whole traversal is 0.275 ms of a 1.29 ms gap, and phases 4's second
   pass adds 0.165 ms on top. Even a free traversal would leave the cluster path slower.
2. **It is not the meshlet path's LOD being more aggressive.** Forcing *both* paths to their finest level
   changes each path's primitive count by 2-7 % and the gap by nothing: the meshlet path draws 96 896
   triangles at LOD 0, the cluster path 405 128, for the same image (4 px apart).
3. **It is how much geometry survives culling.** The cluster path launches 3.2× the mesh-shader workgroups and
   pushes **4.2× the triangles** for a 9 % difference in shaded pixels: the extra triangles are behind other
   surfaces and die in the depth test, so they cost the geometry stage and buy nothing. The traversal rejects
   87 % of the scene's 3.09 M triangles, but what it keeps is 13 % against the meshlet path's 3 %.

The reason is granularity. `ClusterTraversalGroups` tested one bounding sphere per *group* — 561 groups over
3 911 clusters, i.e. ~7 clusters and ~700 triangles per sphere — while the meshlet path's amplification shader
tests every *meshlet* (~73 triangles). A sphere that has to bound 700 triangles is rarely occluded, so whole
groups of hidden clusters were queued and drawn.

### Per-cluster occlusion test — landed

The fix is the shape `vk_lod_clusters` uses in `traversal_reject_clusters.comp.glsl`:

1. **Cook**: each cluster stores `meshopt_computeClusterBounds`'s sphere (`m_CenterRadius[2]`, halves) in the
   cooked `Cluster` record and in the shader's `ClusterRecord`. `kCookedMeshVersion` 6 → 7.
2. **Pass 0 groups pass**: after a group passes its own sphere test, each cluster of it is tested on its own
   sphere against the HZB. A hidden cluster is dropped, and — when a second pass will run — its bit is left in
   `ClusterRejectMasks` (one bit per cluster, indexed by global cluster index, so a group needs no list of its
   own) and counted in `m_RejectClusterCount`. The test is tighter than the group's, never looser, so it cannot
   create holes.
3. **Pass 1** gained `ClusterRejectClusters_CSMain`: one thread per group the first pass walked (its queue is
   left intact for this), skipping groups whose mask words are all clear, re-testing only the deferred clusters
   against the depth the first pass just wrote and appending survivors to the second job list. Its grid is
   indirect slot 3, written by `ClusterBuildArgs`; the indirect buffer grew from three grids to four.

Bistro, same camera and flags, against the same build without it:

| | clusters drawn | mesh primitives | pixel invocations | base pass | `Opaque Renderer` |
|---|---|---|---|---|---|
| before | 3 911 | 395 054 | 9 213 939 | 2.40 ms | 5.45 ms |
| after | 2 043 | 201 925 | 8 559 116 | 2.06 ms | 4.88 ms |

Half the drawn geometry, −14 % on the base pass and −10 % on the whole opaque renderer. Two-pass against
single-pass stays **0 differing pixels** with 1 928 clusters deferred and 21 revived, and the phase-2/3 gates
still pass (Sponza 0 px, Bistro 4 px, 100 % cut coverage, 0 holes, counters `(match)`).

### What is still missing, measured

Root cause of the geometry gap: **unit size**. Our clusters were built with `m_MaxTriangles = 128`,
`m_MaxVertices = 128` and a partition target of 32, while the meshlet path builds 96-triangle / 64-vertex
units (`kMaxMeshletTriangles` / `kMaxMeshletVertices`). A cluster's bounding sphere is what the HZB test sees,
and a sphere that has to cover 128 triangles pokes past an occluder that a 96-triangle meshlet's does not, so
the traversal kept twice the geometry for the same image. Matching the meshlet path's granularity — and the
reference's other defaults (`min_triangles = max/3`, partition target 16, error-merge 1.0) — fixed it:

| Bistro, same camera, same flags | meshlet | cluster 128-tri units | cluster 96-tri units |
|---|---|---|---|
| mesh primitives | 96 896 | 201 925 | **120 418** |
| `Base Pass Render (Phase 1) - Opaque` | 1.928 ms | 2.056 ms | **2.014 ms** |
| instructions, base pass | 301.2 M | 311.9 M | **303.2 M** |
| `Opaque Renderer` | 4.179 ms | 4.826 ms | **4.724 ms** |

The draw is now at parity: +0.7 % instructions against the meshlet path for the same image. The gates also
improved (Bistro forced-level-0 difference 4 px → 2 px, Sponza still 0 px, coverage 100 %, 0 holes, counters
`(match)` on both buckets).

What is left is the traversal itself: `Opaque Renderer` is still +0.55 ms, and the draw accounts for only
0.09 ms of it. That residue is **dispatch-count bound**: 18 compute dispatches per frame (two buckets × init +
6 node passes + groups + build args) at ~18 µs each, for 2 074 visited nodes. Each grid is only ~160-330
groups, so idle threads are not the cost; the per-dispatch overhead is. Cutting it means either dispatching
node passes indirectly so empty queue levels cost nothing, or merging the two buckets' traversals into one set
of dispatches (a unified bucket buffer layout).

The numbers above come from the Debug build because **Release crashes at HEAD already**: it exits with
`0xC0000005` on the first frame of a scene, with the cluster path on or off, so it is not a phase-4
regression. No Release performance claim can be made until that is fixed.

Gate **M4**: V4.1–V4.3 green; the feature stays behind `m_ClusterTwoPass`.

### Phase 5 — Geometry streaming (groups) — **landed**

**Goal**: bound VRAM by streaming cluster groups, with the same request/decide/serve shape as the
existing texture streaming.

Tasks — as built

1. `src/GeometryStreaming.{h,cpp}` owns everything: the arena, the residency table, the request lists, the
   patch list and three readback slots. `Renderer::UpdateStreamingPreRender` runs it after the texture phases
   (on its own command list - the texture phases close the one they were given), and
   `Renderer::UpdateStreamingPostRender` copies this frame's requests into a slot and arms its event.
2. Residency encoding as planned, in 32-bit form: `ClusterGroupState::m_Address` is an element offset into
   `ClusterStreamData` below `ClusterStream::kInvalidAddressStart` (`1 << 31`); at or above it the group is
   missing and the low bits hold the frame index of its last load request, which is what makes a second
   request in the same frame free (`InterlockedMax` plus a compare). The traversal requests a group it cannot
   draw, and a cluster whose *replacement* is missing is drawn itself, so a missing level shows finer
   triangles for a few frames instead of a hole.
3. Storage: one persistent arena with a host-side first-fit free list, blobs 16-byte aligned. A group blob is
   its triangle words followed by its vertex words, and the cooked `ClusterRecord` element offsets are
   rewritten to be **group-local** at load (`ClusterLodGpu.cpp`), so a group's address is the only thing that
   moves. The cluster records themselves stay resident (1.9 MB on Bistro): the traversal needs every level's
   per-cluster sphere before it knows whether that level is worth loading.
4. `ClusterStreaming.hlsl` holds the two passes: `ClusterStreamUpdate_CSMain` (one thread per patch: publish
   an address or clear one; it runs before the uploads, so freed space is never reused while the old address
   is still in the table) and `ClusterStreamAgeFilter_CSMain` (one thread per resident group: age it, and ask
   for it back at the threshold). The active list is host-maintained, so a swap-with-last removal has no race.
5. Integration is `Renderer::UpdateStreamingPreRender` / `PostRender`, as above.
6. Readback follows `FeedbackManager`: a cache-host-visible staging buffer per slot, one event query armed
   with `SignalEventAfterFrame`, and `pollEventQuery` two or more frames later - no frame ever waits.
7. Streaming is **always on**. There is no geometry-only budget any more: the arena is bounded by
   `Renderer::GetStreamingBudgetBytes()`, the same VRAM budget the texture tile heaps spend, which defaults to
   the OS budget for this application (9 283 MB of the 10 051 MB card here) and is overridden by `--stream-budget`
   or by the panel's "Streaming VRAM" slider. Anything else is automatic: the arena holds every group when the
   budget allows it, and a lower budget re-packs it (the coarsest levels go first) and hands the memory back.
   `--cluster-stream-age <frames>` and `--cluster-stream-stats-frame <n>` stay as diagnostics. A level the cut
   picks is "used"; a level it skips ages out, so with a tight budget the arena converges on what is on screen.
   The sweep is only dispatched while the arena is **smaller than the scene** (`m_ArenaElements < m_TotalElements`,
   i.e. the budget really did force a trim): when every blob fits, the preload has already made the whole scene
   resident and evicting what this frame's cut skipped only guarantees the next camera move asks for it again.

Validation (Debug build, 1080p, Bistro restored camera, TAA/indirect/auto-exposure/RT shadows off)

* **V5.1** the settled streamed image matches the preloaded one: frame 200 at a **4 MiB** budget differs from
  the preloaded frame by max 35, mean 0.148, 1 482 px over threshold - *below* the renderer's own run-to-run
  noise floor measured the same way (preloaded frame 150 vs 300: max 37, mean 0.156, 1 751 px). The
  traversal is the stronger evidence: at frame 150 both runs report the identical
  `16 instances, 492 nodes, 196 groups, 659 clusters (11 above level 0)` for opaque and
  `4 / 52 / 10 / 95` for masked, i.e. the cut and the draw list are unchanged once streaming settles
  (204 of 5 378 groups resident, 1.10 MB of the 4 MiB arena).
* **V5.2** no holes at a budget that holds the working set: at 4 MiB the frame after a `--camera-spin` run
  is still the scene. Under deliberate pressure (1 MiB arena, spinning camera) the arena runs at
  229/5 378 groups with 95 requests a frame and the image stays coherent (mean luminance 79.9, 98 % of
  pixels non-black) - that is the "finest available" trade-off, not a renderer failure.
* **V5.3** budget honoured: `rdc-tools resources` shows `Scene_ClusterStreamData` at exactly the 4 MiB the
  switch asked for (`4 194 304 B`), and the residency line never exceeds it (1.10-1.39 MB in these runs).
  `rdc-tools vram` puts the whole frame at 355 MB. A *fixed reservation* cannot show heap totals shrinking,
  because the plan's decision #3 is an arena with a free list rather than sparse resources; what returns to
  the arena is the resident blobs (204 of 5 378 groups), not the VA range.
* **V5.4** `deps` on `bin/captures/bistrox_capture.rdc` lists `Scene_ClusterGroupStates` with 19 writes and
  22 reads and **no** `read-before-write` flag, and the request and counter buffers likewise. `hazards`
  reports 8 distinct conflicts, all in pre-existing passes (`HZB` read as an SRV while in
  `UnorderedAccess` in the culling pass, the two RTXDI light-PDF textures and `OpaqueColorTexture_RG`); none
  involve the streaming buffers. `--rhidebug` on the streaming path: 0 D3D12 errors, 0 nvrhi messages.
* Perf, `bin/captures/bistrox-frame150.wpix` (settled 4 MiB budget): `Cluster Streaming` 23.1 us, of which
  the age filter is 16.3 us (dispatch overhead over 204 groups) and the request readback copy 5.2 us. For
  scale in the same frame: `Cluster Traversal` 403 us for both buckets, `Base Pass Render (Phase 1) -
  Opaque` 646 us, `Opaque Renderer` 2.03 ms. The churn frame (frame 60, uploads in flight) measures the
  update pass at 23 us and the whole streaming step at ~41 us.
* Cut exactness is unchanged with the indirection in place: `--cluster-no-culling` on Bistro gives
  Opaque 37 236 and Masked 17 513 clusters against the CPU cut reference, both `(match)`; bunny 1 009
  `(match)`.

### Unconditional sweeping, and what it did to a turning camera (2026-11-21)

The age filter used to run every frame regardless of budget. On a 9 283 MB budget for a 25.57 MB arena that
is pure loss: a `--camera-spin 300` run from the Bistro street pose had **3 454 of 5 378 groups resident**,
128 loads and 47 unloads a frame, and 10 339 dropped requests, so a quarter of the frame was missing its
props (the scooter, the planters, the awnings) and half-covered surfaces read as degenerate geometry at the
screen edges. With the sweep gated on pressure the same frame is **5 378 of 5 378 resident, 0 loads, 0
unloads**, and the image matches the all-resident reference to the run-to-run noise floor (13 764 px over 8 of
2 073 600, against 946 588 px - 428 k of them over 32 - before the gate). A tight budget still sweeps: at
`--stream-budget 4` the arena stays at 4.00 MB with 1 321 groups and evictions, as designed. Every bucket's
cut is still `(match)` on the CPU reference with `--cluster-no-culling` (41 822 opaque, 17 852 masked), and
`--rhidebug` reports 0 D3D12 errors on the turning path.

Generation was measured before it was changed, and it is not the cause: at the Bistro street pose with
`--camera-spin 300` the metric cut draws 3 117 of 17 302 opaque clusters above level 0 (18 %, and those are
props - railings, lamps, chairs - while buildings and pavement stay at level 0), forcing level 0 changes the
same frame by 8 205 px over 8 out of 2 073 600, and the debug ramp (mode 14) is level 0 across every surface.
`m_DecimationFactor` and `m_ErrorMergePrev` were left alone for that reason: a conservative chain would only
spend arena and triangles on geometry the cut already declines to coarsen.

Fixed on the way: `Scene_ClusterGeometryBuffer` was being created from the *vertex* buffer's descriptor
(stride 4) and then re-created a second time in the same block; it is now created once, with its own stride.

Gate **M5**: green at a 4 MiB budget on Bistro (V5.1-V5.4), with the caveat recorded above that a bounded
arena trades transient holes for memory. Streaming now runs by default against the whole-VRAM budget (the same
number the texture tile heaps spend, driven by the "Streaming VRAM" slider); the numbers above are what the same
code does when that budget is turned down to 4 MiB with `--stream-budget`.

#### Budget, and what the default looks like (2026-11-21)

`--stream-budget <MiB>` / the panel slider is one cap for both streaming systems, and the two share it in both
directions: the arena is sized to `min(scene blobs, budget - what the tile heaps already hold)` when the budget
changes, and every `HeapAllocator::AllocateHeap` is refused past `budget - arena`
(`Renderer::GetTileStreamingBudgetBytes`). Whichever system grows last yields, so the sum stays under the cap;
the one exception is deliberate and is the arena's floor - a budget below the scene's own coarsest cover (3.07 MB
on Bistro) still gets that cover, because coarse geometry beats no geometry.

Measured on Bistro (frame 100, `--hitch-log 5`, arena from the streaming line, heaps from the hitch line):

| Budget | Texture heaps | Geometry arena | Geometry residency |
|---|---|---|---|
| OS budget (9 283 MB) | 160 MB | 25.57 MB (all groups) | 5 378 / 5 378, 0 requests |
| 96 MB | 64.0 MB in 4 heaps | 25.57 MB (all groups) | 5 378 / 5 378, 0 requests |
| 48 MB | 32.0 MB in 2 heaps | 16.00 MB | 2 012 / 5 378, 128 loads a frame |
| 25 MB | 16.0 MB in 1 heap | 9.00 MB | 2 010 / 5 378, 128 loads a frame |
| 16 MB | 16.0 MB in 1 heap | 3.07 MB (the coarsest-cover floor) | 905 / 5 378 |
| 4 MB | none - a 16 MB heap does not fit | 4.00 MB | 1 046 / 5 378 |

Two consequences worth knowing before reading the table as a regression: the heaps load their resident tiles
*before* the arena is sized, so at 48 MB they hold 32 MB and geometry gets the remaining 16 MB (a strict
geometry-first split would need the tile heaps to evict resident tiles, which the MinMip path does not do), and a
budget that cannot fit even one heap leaves every texture at its packed mips (4 MB row).

A runtime change is a re-pack, not a reload: shrinking to 8 MB kept 1 305 groups and grew back to 25.57 MB and
1 717 groups when the budget was raised again, with the drawn cluster count unchanged (19 266).

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

**Phase 6 — as built (2026-11-22).** The classification is exact rather than bracketed, because the
reference's per-level spheres are not sound here.

* **The trap the first attempt fell into.** The reference classifies with one enclosing sphere plus the
  level's smallest sphere and smallest/largest error, and treats "no group of this level is over the error
  budget" as "this whole level is fine". On Bistro that lost **46 clusters** from five instances, and the
  reason was not the sphere approximation: the cut also draws clusters *below* the drawn level whose
  replacement is over the budget, and — the actual cause — **clusters no coarser group replaced at all**
  (terminal/stuck groups keep `kNoGroup`, so they are drawn at any distance). Nothing in a level-wide
  metric expresses that.
* **What the shader does instead** (`ClassifyInstanceLod` in `ClusterTraversal.hlsl`): one pass over the
  instance's own groups builds a bit mask of "this level has a group over the budget" and "… under it".
  The first level in the coarse mask is where coarsening starts (`lodLevelMin`), the level below it is
  the one drawn. The cut is that single level exactly when **every** group at `lodLevelMin` is over the
  budget **and** `ClusterGeometry::m_UnreplacedLevels` has no bit below the drawn level. The cost is the
  same set of group records the traversal is about to read anyway (~12 k per frame on Bistro, both
  buckets), and it is a *per-instance* classification, so two instances of one primitive can disagree.
* So a discrete instance seeds the traversal at its level's node with `ClusterRefFlags::kDiscrete` carried
  on the node and group refs; the groups pass skips the metric and the replacement test for it and draws
  the whole level (culling unchanged). The flag also rides the reject lists, so the second pass keeps the
  same decision. The resulting image is **bit-identical** to the continuous cut (V6.2's first half) —
  that is the property the exact test buys. The check is two runs of
  `--cluster-no-culling --cluster-stats-frame <n>` (counters) and two of `--screenshot-frame <n>` with
  `--cluster-discrete-lod 0` and `1` (image), on Bistro, Sponza and DragonAttenuation.
* **RT agreement**: the traversal now owns `Scene_InstanceLODBuffer`, writing it for **every** instance of
  the bucket before any culling, because a shadow ray reaches instances the camera does not see; the
  culling pass leaves it alone when the cluster path draws that bucket (`CullingConstants::
  m_UseClusterRendering`). The buffer is zeroed at load: `TLASPatch` runs before the first base pass, so
  its first read is of whatever the load wrote. The level written is `m_LevelMeshLODs[drawLevel]`, the
  per-mesh LOD whose quadric error is closest (log space) to that cluster level's worst group — the two
  chains reduce by different factors (0.5× vs 0.6×), so the indices are not interchangeable and the
  stored map is what keeps the BLAS at the same detail the raster cluster has.
* **Residency** is a per-geometry table (`Scene_ClusterDiscreteLevels`, one uint: the first level of the
  fully resident suffix), derived from the host residency in `GeometryStreaming::PreRender` — the mirror
  of the reference's `Geometry::discreteLodLevel`, which is what makes "all levels from here on are
  resident" one comparison. A discrete instance is clamped to `max(drawLevel, residentLevel)`: a pop to a
  coarser level, never a hole and never a raster/RT mismatch. The levels such an instance draws are then
  aged as used by the groups pass, which is the "pinning" task — no separate exception was needed.
* **Gates** (Debug build): cut totals and discrete instance/cluster counts both `(match)` the CPU
  reference (`CountReferenceDiscreteLod`) with `--cluster-no-culling` — Bistro 45 553 / 2 285 / 6 934
  (opaque) and 18 106 / 135 / 9 215 (masked), Sponza 2 928 / 85 / 2 668 and 498 / 14 / 498, dragon
  2 421 / 2 / 2 421. Discrete on vs off is **0 differing pixels** on all three once `--no-auto-exposure
  --disable-sampler-feedback` pin the exposure and mip history (without them the ±1 noise reads as
  0.2–20 % of pixels). At `--stream-budget 4` the clamp still matches the reference exactly
  (2 285 / 6 934), which is the V6.2 non-resident half.
* **Cost and captures** (Bistro, frame 30, same camera, PIX hardware counters): `Cluster Traversal`
  526 µs with discrete on against 529 µs with it off, and the opaque base pass is 303.3 M instructions /
  2.014 ms in both — the classification is free and the draw is untouched. RenderDoc
  (`bin/captures/bistrox_capture.rdc`) lists the new buffers (`Scene_ClusterLevelMeshLODBuffer` 6 808 B,
  `Scene_ClusterDiscreteLevels` 2 204 B, `Scene_ClusterGeometryBuffer` 22 040 B = 551 × 40 B, node queues
  19 809 × 12 B), `hazards` still reports the same 8 pre-existing findings (HZB, RTXDI light PDFs,
  OpaqueColor) and none on a cluster resource, and `--rhidebug` reports 0 D3D12 errors.
* **V6.1 caveat.** Only "the raster is unchanged" is measured exactly. A per-pixel comparison against a
  pre-phase-6 capture is not reproducible on this machine any more: `bin/camera_state.json` is not tracked
  and was overwritten, and the RT shadow path is stochastic without NRD, so the honest evidence is the
  dragon composite (coherent cast shadow, no acne streaks) plus the fact that the BLAS level and the
  raster level now come from the same classification.

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
| `ClusterDebug.hlsl` | AS+MS+PS | cluster bboxes as line-topology mesh output | 2 |
| `BasePassCommon.hlsli` | hlsli | `VSOut`/`PrepareVSOut` + base-pass bindings, shared by the vertex, meshlet and cluster paths | 2 |
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
| `src/ImGuiLayer.cpp` | "Level of Detail" section, counters, debug modes |
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
