# Upstream review: `vk_lod_clusters` after `a6ba49a`

Review date: 2026-11-23.
Upstream: `D:\Workspace\vk_lod_clusters`, `a6ba49a^..04e5b44` (8 commits).
Local: `src/ClusterLod*`, `src/GeometryStreaming*`, `src/shaders/Cluster*.{sr,hlsl}`.

Purpose: decide which of the upstream changes since the previous study
(`docs/clustered-geometry-study.md`) are worth adopting, changing or consciously skipping in
our cluster-LOD feature. Sections 0-13 are that review; §14 (disk-backed streaming plan and task
list) and §15 (how culled geometry streams today) are local follow-ups added afterwards.

## 0. TL;DR

| Upstream change | Commit | Applies to us? | Verdict |
|---|---|---|---|
| Hit-based streaming (ray hits drive residency + LOD) | `44f32db`, `7a02fc9` | Only with a cluster-LOD ray tracing path, which we do not have | **Defer**; adopt the pieces that are renderer-agnostic (overlap table, coarse→fine load order) |
| Bidirectional ±1-LOD group overlap table | `44f32db` | Yes | **Adopt** when load ordering / prefetch / hit streaming needs it (cooked-format change) |
| Metric-gated traversal (no request / no queue for groups the cut can never draw) | existing behaviour, emphasised by hit streaming | Yes | **Fix now** — we have the opposite behaviour today |
| Coarse→fine load ordering, fine→coarse unload order | `44f32db` (limitation note) | Yes | **Adopt** (needs the overlap table) |
| Cached-BLAS *levels* chain, push/sticky levels, defer/pin/retire | `a6ba49a`, `44f32db` | No — needs cluster BLAS (D3D12 preview) | Note for the CLAS/PTLAS follow-up |
| Culled-group LOD (occluded parts of a visible instance get a coarser cut) | `c3e5510` | RT only; our raster path already culls per cluster | Skip |
| Instance pre-cull by world-space bounding sphere | `c3e5510` | We already do this | No action |
| Persistent-traversal task-counter fix | `44f32db` | Not used by us, but the protocol matters if we add one | Note |
| EXT mesh shader simplification, NV variant split out, compute-raster tuning | `2f89d6c` | No triangle culling in our shader; no SW raster | Skip |
| LOD-root node validation at scene load | `c3e5510` | Yes (cheap) | **Optional** assert |
| Live-tunable cull thresholds, residency-reason histogram | `c3e5510`, `7a02fc9` | Yes | Optional QoL |
| PTLAS, partitions early-out, hybrid raster/PT, BLAS dedup | `a6ba49a`, `3965745` | Blocked (D3D12 SDK/SM) or out of scope | Skip |

The single actionable finding is in **§3**: our node pass requests stream-in for every group the
spatial tree reaches, while upstream only descends where the LOD metric says refinement is needed.
Under a tight streaming budget that is the difference between an arena that converges on the drawn
set and one that holds the whole visible region at every LOD level. It is a small, image-neutral
change and it is verifiable with our existing `--cluster-no-culling` cut-equality gate.

## 1. Hit-based streaming (the headline feature)

Docs: `docs/hitbased_streaming.md`. Flags: `--hitstreaming`, `--hitstreaminglod` (on by default),
`--hitstreaminglodfactor`, `--hitstreamingshadowdepth`, `--hitstreamingcamerascale`,
`--streampropagation` (0..3), `--persistenthitdemand`.

Motivation: rasterization streams what survives occlusion culling; ray tracing cannot use that
test because shadow, AO and reflection rays reach geometry outside the primary view. Upstream
replaces the visibility prediction with feedback from the rays themselves: which resident groups
were hit, and how wide the ray footprint was when it got there.

Pipeline (`44f32db`, stats corrected in `7a02fc9`):

1. **Hit tagging** — every closest-hit shader (`render_raytrace_clusters_hit.glsl`,
   `render_pathtrace_clusters_hit.glsl`, the AO miss/hit variant, `render_rayquery.glsl`, the
   hybrid primary-hit path) updates one `uint32` per resident group with a single `atomicMax`:
   bit 31 "no finer group resident", bits 30..16 cyclic 15-bit frame, bits 15..0 inverted-log2 ray
   cone width. One atomic yields "newest frame, then narrowest cone".
2. **Propagation** — `stream_hit_propagation.comp.glsl` runs over all resident groups, has each
   group inherit its resident finer groups' newest hit (one LOD transition per frame), maintains
   the "no finer resident" bit, smooths the width (`groupHitWidths`), records the per-geometry
   finest demanded level, and seeds the demand pass.
3. **Stream-in** — a per-frame demand flood writes a persistent `Geometry::groupDemand` for the
   *non-resident* finer groups through the new ±1-LOD overlap table. The traversal tests one
   `uint32` per group (`streamingTestOverlapHits`) instead of waiting for each intermediate level
   to become resident. Four propagation modes trade reach for cost
   (`shaderio.h: STREAM_PROPAGATION_*`).
4. **Stream-out** — `streamingAgeFilter` also unloads a group whose last hit is older than
   `ageThreshold + 32` frames, but only when no finer group is resident, so unloads run
   fine→coarse and the inheritance chain cannot break.
5. **LOD adjustment** — with `--hitstreaminglod`, `streamingDemandsFiner` clamps the cut: a group
   is opened only while the camera metric wants it *and* a recent hit's smoothed cone width is
   finer than the group's quadric error. The same test is applied to the group node and to each
   cluster's generating group so both halves of the cut agree. Cone widths follow the ray-cone
   framework (primary = camera metric exactly, planar mirror = distance only, curved mirror widens,
   shadow rays continue the surface's cone).
6. **Per-instance demand** — `instance_hit_scale.comp.glsl` turns a magnifier/focusing-mirror
   footprint into an `errorScale` below 1 for that instance only (clamped at 1/128), combined with
   `min()`; it also overrides the coarser `culledErrorScale` of a mirror-only-visible instance.
7. **Camera floor** — `--hitstreamingcamerascale K` opens a group that the camera metric at `K`
   times the error wants, so disoccluded regions are not stuck at the coarsest level until hints
   arrive.

Measured (upstream doc, path tracer, 3 scenes): residency −14 %…−44 %, frame +0.10…+0.68 ms,
"Hit Propagation" + "Hit Demand" 0.12–0.21 ms. **Most of the saving comes from the LoD
adjustment**: without it residency drops only 1–4 % and the frame gets slower.

Known limitations (theirs, relevant to any adoption): feedback lags a frame plus a streaming round
trip; hit data is per geometry, so the most demanding instance sets the detail for all instances
of that geometry; loads are not ordered coarse→fine, so an intermediate level arriving late can
render a surface twice for a frame or two; overlap indices are 20-bit per geometry.

### What it means for us

Our cluster path is raster-only: `ClusterTraversal.hlsl` writes `g_InstanceLOD` for the TLAS patch,
but no ray-traced geometry is cluster-resolved (`src/shaders` has no cluster closest-hit path), so
there is no hit signal to feed. **Adopting hit-based streaming is a follow-up of a cluster-LOD ray
tracing path, not an independent task** — and on D3D12 that path is currently blocked by the
cluster-BLAS/CLAS preview requirement documented in `docs/clustered-geometry-study.md` §0.2.

The parts that do not need rays, and that are worth having regardless:

* the **±1-LOD overlap table** (§2),
* **load ordering** coarse→fine and the **fine→coarse unload guard** (§4),
* the **cyclic-frame + inverted-log2-width packing** trick (`STREAMING_HITFRAME_*` in
  `shaders/shaderio_streaming.h`) if we ever need per-group recency in one word,
* the **demand BFS implementation**: it reuses the traversal node-queue memory as its queue and
  processes each group's children warp-wide instead of looping per thread
  (`stream_hit_demand_tree.comp.glsl`).

## 2. Bidirectional overlap table

Upstream: `Scene::buildGeometryLodOverlap` (`src/scene_cluster_lod.cpp:1024`) derives, per group,
the distinct groups one LOD level coarser and one level finer, from the generating-group DAG.
Storage is `GroupOverlapInfo {countCoarse, countFine, offset}` (8 B/group) plus a flat array of
20-bit group indices, so ~2.5 B per link; `decode20Bit` reads it in the shader. It is persistent
per geometry (available before residency) and part of the scene cache.

We have: `PrimitiveClusters::m_GeneratingGroups`, one uint per **cluster** (finer→coarser only).
The generation code in the builder already deduplicates the coarser direction per group, so the
transpose is a few lines and a sort.

When we need it:

* ordering stream-in requests coarse→fine (or at least tagging them by level) — with only the
  child→parent link we cannot find a group's children without scanning every cluster;
* speculative prefetch of the finer neighbours of a drawn group;
* any future hit-based or demand-based streaming.

Cost: a `kCookedMeshVersion` bump, ~2.5 B/link in the cooked mesh and at load. For reference
scale (Bistro: 5378 groups, 82 k clusters) this is a few tens of KB per primitive. Verdict:
**adopt when §4 is implemented, not before.**

## 3. Metric-gated traversal (our real gap)

Upstream node pass (`traversal_run.comp.glsl`): every child node is evaluated with
`testForTraversal(...)`; `traverseNode = isValid && traverse` decides whether it is enqueued, and a
group node is only requested when the metric still says "needs refinement". The group pass then
applies the cluster-level test.

Ours (`src/shaders/ClusterTraversal.hlsl`):

* `ClusterTraversalRun_CSMain` (line 438) descends **purely spatially** — frustum and HiZ only,
  no error test, because `ClusterNode` carries no metric (`src/shaders/Cluster.sr:40`, 4 B packed).
* On a leaf, `if (!GroupIsResident(groupIndex)) { RequestGroup(groupIndex); return; }` (line 508)
  happens **before** any cut test.
* The cut test itself, `GroupTooCoarse` (line 233/560), only runs in the groups pass, after the
  group is already queued (or already requested).

Consequence: the traversal requests every leaf group inside the frustum that survives HiZ, at
every LOD level, although the cut will never draw the coarser ones.
With the arena at full budget this is invisible; under memory pressure it is the same
"arena holds visited-but-unused levels" failure mode that upstream's hit-streaming demand gate
removes for ray tracing (they can already rely on metric pruning at the node level instead), and
it is also wasted group-queue traffic and wasted metric reads every frame.

### Recommended change (image-neutral, no format change)

Two independent, small edits:

1. **Gate requests on the cut.** In the leaf branch, before `RequestGroup`:
   * forced-LOD mode: request when `group.m_LODLevel <= forcedLevel`;
   * discrete instance (flag carried in `ClusterNodeRef::m_Flags`): always request — the whole
     seeded level is drawn by definition;
   * otherwise: request only when `!GroupTooCoarse(groupIndex, instance.m_World)`.

   The group record (metric + bounds) is always resident — only the triangle/vertex blob streams
   (`src/ClusterLodGpu.h`, `GpuData::m_Groups` vs `m_StreamGroups`) — so the test is available for
   a non-resident group.

2. **Skip level roots the cut cannot use.** `ClassifyInstanceLod` already computes the
   per-level `coarseLevels` / `fineLevels` bit sets (lines ~289). Return `fineLevels` (levels with
   at least one group under the error budget) in the classification and seed the node pass at
   those level roots instead of the primitive root. Level roots are nodes
   `childOffset + level` of node 0, which the discrete seeding path already relies on. No new
   buffer, no cooked-format change.

Optional follow-up: put the level's *minimum* max-error (`PrimitiveClusters::m_LODMinMaxError[]`,
already computed, matching upstream `LodLevel::minMaxQuadricError`) into `ClusterGeometry` so the
skip decision is a compare per level rather than the bit-set walk.

### Why this is the right first move

* It is the upstream tree-walk behaviour expressed with the data we already have, and it makes our
  streaming budget behave like the reference's: the arena converges on what is drawn.
* It cannot change the image. Skipping a too-coarse group removes exactly what the groups pass
  would have rejected (`GroupTooCoarse` at line 560 returns before any cluster is emitted).
* It is verifiable with the existing gate: `--cluster 1 --cluster-no-culling --cluster-stats-frame n`
  must still report `(match)` against `ClusterLodScene`'s CPU cut reference, and
  `--cluster-force-lod 0..n` must still match.
* It is also visible in existing counters (`--cluster-stats` `groups queued`, streaming `requests`,
  `dropped loads`) and in the streaming residency line, without new instrumentation.

Effort: small. Risk: low, but the forced-LOD and discrete paths must keep their current request
semantics (a discrete seed must be able to load its whole level).

## 4. Load ordering and unload order

Upstream documents both as limitations/requirements of hit streaming:

* loads should arrive **coarse→fine**; a fine group that lands before the levels between renders
  its surface twice for a frame or two (a finer group draws when its generating group is missing —
  our rule at `ClusterTraversal.hlsl:610` is the same "finest available" fallback);
* unloads must run **fine→coarse** or the coarser groups lose their (hit) inheritance chain.

Our host side (`src/GeometryStreaming.cpp`) appends requests in traversal order
(`m_PendingLoads.insert(...)`, line 461) with first-fit allocation and no priority or sort, and
the age filter (`src/shaders/ClusterStreaming.hlsl`) ages every active group independently.

Recommendation, when §2 lands: order the per-frame pending loads by LOD level (coarsest first) and
gate unloads with "no finer group resident" — the raster equivalent is a cheap check against the
residency table and keeps the cut covered during transitions. The reference's no-finer bit is
maintained by the propagation pass; we can derive it in the age-filter kernel from the group's
finer overlaps (needs the §2 table).

What actually happens today to a fully culled instance (does it stream out, and where does the
data come back from) is answered in §15.

## 5. Cached BLAS: levels, push/sticky, defer/pin/retire

Upstream (`a6ba49a` makes caching default on; `44f32db` and header comments in
`src/scene_streaming.hpp`):

* `--cachinglevels N` keeps a **chain** of immutable cached BLAS per geometry, one per coarsest
  tail level, refined one level per frame (`handleBlasCachingLevels`,
  `GEOMETRY_CACHED_BLAS_STRIDE 16`, slot = level counted from the coarsest — derivable from the
  level alone);
* `--cachingpushlevels` lets an instance give up N extra levels to use a cached BLAS,
  `--cachingstickylevels` grants extra levels to stay on it; `pickCachedLevel` picks the own level,
  else the closest coarser within tolerance, else the closest finer;
* `blasCacheDeferFrames` holds a replaced allocation (the partition early-out only refreshes an
  instance when its partition is processed), `blasCachePinGroups` refuses to unload the groups a
  cached BLAS was built from, `blasCacheRetire` steps a pinned BLAS down a level instead of
  pinning forever;
* `geometry_cached_retain.comp.glsl` keeps the cycle-minimum cached level for the age filter.

These all operate on **cluster BLAS**, which D3D12 does not expose on this machine (preview
Agility 1.722 + SM 6.10 + `ClustersAndPTLASSupported`). Our RT story is already the study's
"short-term" recommendation: a per-mesh-LOD BLAS built at load, with `g_InstanceLOD` from the
cluster classification patching the TLAS. The push/sticky tolerance idea is the only piece that
could be transplanted now (choose among the existing per-mesh LOD BLAS with a tolerance instead of
an exact level), but it would only move the RT cut coarser — skip until the RT cut is
cluster-resolved.

## 6. Culled-group LOD (occluded parts get a coarser cut)

`USE_CULLED_GROUP_LOD` (default off; `renderer_raytrace_clusters_lod.cpp:85`): for a *primary
visible* instance in ray tracing, a node/group whose metric sphere failed last frame's HiZ gets
`culledErrorScale`, inherited by its children (`queryMetricOccluded` in
`shaders/traversal.glsl`), and the group pass mirrors the test so nodes and generating groups agree.

Ray tracing only, because rasterization already has per-cluster HiZ culling at exactly the
granularity that matters. We do: `ClusterTooSmall` + `ClusterOccluded` per cluster in the groups
pass of `ClusterTraversal.hlsl`. Skip.

## 7. Instance pre-cull by world-space bounding sphere

`USE_INSTANCE_PRECULL` (`shaders/culling.glsl:155`, spheres uploaded per instance from
`src/scene.cpp`): one sphere-vs-frustum test before the instance/geometry loads, so instances
outside every frustum this pass tests cost nothing.

We already have this: `ClusterTraversalInit_CSMain` calls `SphereTestsWorld(instance.m_Center,
instance.m_Radius, ...)` before writing the seed, and `GPUCulling.hlsl` culls instances for the
meshlet path. No action.

## 8. Persistent traversal: the task-counter fix

`traversal_run.comp.glsl` changed `run_persistent()` to return whether **this subgroup took the
global task counter to zero** (`tasksBefore == numRunnable`), and to add only the *stored* nodes
(`min(countNodes, capacity - offset)`) so the counter still reaches zero when the queue array
overflows; only then does one lane publish the group-pass dispatch.

We do not run a persistent traversal kernel (node passes are host-dispatched over the worst-case
grid, per the phase-1 design). If we ever adopt one, start from this protocol — the naive
"counter == 0 then dispatch" races with subgroups still enqueueing and can under-dispatch the group
pass.

## 9. Raster-path changes in `2f89d6c`

* EXT mesh shader variant simplified: **triangle (primitive) culling removed** from the EXT path
  and kept only in the new `render_raster_clusters_nv.mesh.glsl`. Reason visible in the code:
  EXT mesh-shader outputs cannot be read back, so per-triangle culling needs a `shared` copy of the
  vertex positions plus a barrier; upstream keeps it only for the NV variant.
* Compute (software) raster: `SW_RASTER_CLUSTERS_PER_WORKGROUP 2`, i.e. two clusters per 64-thread
  workgroup, 32 threads each. Upstream's comment: one cluster per workgroup is too few to hide the
  loads, four makes the warps wait longer for each other at the barrier than two.

We do neither (our `ClusterBasePass.hlsl` MS decodes the cluster and writes all its triangles; no
triangle culling, no SW raster — deliberately, see the study §6 "do not adopt: SW rasterization").
No action.

## 10. Small items worth taking

| Item | Upstream | Ours | Action |
|---|---|---|---|
| LOD-root validation at load | `src/scene.cpp` checks node 0 is an interior node whose children are exactly the level roots, else counts the geometry invalid; `LOD_LEVEL_NODE_OFFSET 1` names the layout | `ClusterLod::ValidateDag` covers the DAG in tests; the GPU layout assumes the same shape | Optional: one assert/validation in `ClusterLodGpu::BuildGpuData` (AGENTS.md prefers asserting structural invariants) |
| Live-tunable cull thresholds | `CULL_MIN_PIXEL_SIZE` and `swRasterThreshold` moved into frame constants (`view.cullMinPixelSize`) | `kMinClusterPixelSize` is a compile-time constant (`ClusterTraversal.hlsl:78`); the pixel-error threshold is already a per-frame constant | Optional QoL; no perf effect |
| Max-loads boundary | `offsetRequested <= maxLoads` → `<` (off-by-one: last slot read stale data) | `slot < g_Traversal.m_MaxLoads` (line 184) | Already correct |
| Residency histogram by reason | `STREAM_DEBUG_RESIDENCY` bins resident groups: hit ≤2 frames / hit ≤32 / hit older / unused / kept by caching | we plot per-level resident counts | Optional; add a "kept by the cut / otherwise idle" split when diagnosing churn |
| LOD level node offset | `LOD_LEVEL_NODE_OFFSET` names root→level-roots explicitly (no root-node read) | `NodeGetChildOffset(g_Nodes[geometry.m_NodeOffset].m_Packed) + level` | Same layout, no change needed |

## 11. Recommended order of work for us

```
P1  metric-gated requests + level-root seeding (§3)
      -> verify: --cluster-no-culling cut match (all buckets), forced-LOD sweep match,
         streaming residency line converges on the drawn set at a tight --stream-budget
P2  ±1-LOD overlap table in the cooked data (§2) + coarse->fine load ordering,
    fine->coarse unload order (§4)          [kCookedMeshVersion bump + cache rebuild]
      -> verify: same cut match; churn counters at a tight budget; camera-spin run shows
         no re-request storm
P3  (needs a cluster-resolved RT path, CLAS/PTLAS capability) hit-based streaming (§1)
      -> verify: upstream's own gates - cut match, image-neutral with hit LOD off,
         residency drop with hit LOD on
```

Rationale for P1 over anything else: it is the only change in this release that pays off in our
current configuration (raster cluster LOD with a bounded shared streaming budget), it needs no new
data or format, and the traversal code path that makes it necessary is fully local to
`ClusterTraversal.hlsl`. Everything else is either blocked by D3D12 capabilities or a prerequisite
of a feature we cannot use yet.

## 12. Deliberately not adopted

* **PTLAS + partition early-out** (`docs/partitions_and_ptlas.md`): `VK_NV_partitioned_acceleration_structure`
  and the per-instance early-out target scenes with millions of instances. Blocked on D3D12
  (preview runtime) and irrelevant at Bistro/Sponza scale.
* **Hybrid raster/path tracing** (`docs/raster_hybrid.md`, `--renderer 3`): requires the RT cut and
  the raster cut to agree bit for bit, i.e. a cluster-resolved BLAS. Orthogonal to the raster path.
* **BLAS dedup** (`docs/blas_dedup.md`): dedups cluster-BLAS builds by an order-invariant hash of
  CLAS addresses. No CLAS, no dedup target.
* **Video encoder, camera paths, HDRI environment, HBAO rewrite, sky after opaque**: renderer
  features unrelated to cluster LOD.

## 13. Assumptions

* Our RT path stays non-clustered until the D3D12 cluster-BLAS capability is available on the
  target machine; if that changes, §1 and §5 become the top of the list, in that order.
* A tight streaming budget remains a supported mode (the shared VRAM cap) — otherwise P1's benefit
  is limited to queue traffic.
* The cooked mesh cache may be invalidated (P2), but not the renderer's scene structure.
* Scale target added below: cooked geometry in the O(10 GB) range with ~10^9 vertices, which the
  current load path cannot hold in VRAM or system RAM. See §14.

## 14. Disk-backed geometry streaming — design and task list

This section is a separate feature from the upstream review above. It exists because the target
scene size (single-digit-GB cooked geometry, billions of vertices) breaks two assumptions the
current streaming makes: that **all group blobs are already in system RAM**, and that **all vertex
attributes are in VRAM**.

### 14.1 Where the bytes are today (verified)

| Cost | Where | Size at current scale (Bistro) | Scale behaviour |
|---|---|---|---|
| Group blobs (triangles + vertex indices) | **cooked mesh file**, mapped read-only by `GeometryStreaming`; `ClusterLod::GroupBlobRef` locates each group | 25.57 MB of blobs in a 144.93 MB cache (Bistro) | ~6.7 B/tri; not in system RAM since v8 |
| Cooked cluster CPU copy | `Scene::Mesh::m_Clusters` (`SceneLoader.cpp:2211`), kept for the CPU cut reference | dominates load RAM | `Cluster` 20 B + `Group` 64 B + node/bounds + generating groups 4 B per cluster |
| Global quantised vertex attributes | `Scene::m_VertexBufferQuantized`, one buffer, uploaded in `CreateAndUploadGpuBuffers` (`SceneLoader.cpp:2366`), `VertexQuantized` = 24 B (3 floats + 3 packed words) | MBs | **linear in the whole scene's vertex count** — 10^9 verts ≈ 24 GB |
| Global index buffer + meshlet arrays | same load path / cooked cache | MBs | linear in triangles |
| Cooked cache on disk | one file, v8: index record (metadata only) + per-group blob table + 4 KiB-aligned blob section | 144.93 MB (Bistro, legacy arrays included) | blobs read on demand from the mapping |

Hard limits already present: the arena address is a **31-bit element offset**
(`ClusterStream::kInvalidAddressStart = 1 << 31`, `ClusterLodGpu`/`GeometryStreaming`), i.e. ≤ 8 GB
of arena; `ClusterRecord::m_VertexOffset` and the scene-wide vertex indices are 32-bit element
indices; the request budget is 512 groups/frame **plus** a host byte budget
(`--cluster-io-mb-per-frame`, default 16 MB) with 8 readback slots.

So "streaming" today is VRAM-only. Making it disk-backed is two separate unlocks:

1. **System RAM:** blobs, and later per-primitive metadata, come from the file on demand instead of
   from resident `std::vector`s.
2. **VRAM:** vertex attributes move out of the one global buffer into what is streamed, otherwise
   the 40 GB-equivalent vertex buffer dominates no matter what the arena does.

### 14.2 Decisions to make first (with the default recommendation)

| Decision | Options | Recommendation |
|---|---|---|
| D-A IO mechanism | (a) memory-map the cache and `memcpy` from it; (b) overlapped `ReadFile` into our own bounded staging ring; (c) DirectStorage (batched, optional GPU decompression) | **(a) first** — `MemoryMappedDataReader` already exists for texture tiles; the OS page cache bounds RAM by trimming. Add `PrefetchVirtualMemory` for the next blob ranges (async read-ahead) and `VirtualUnlock`/`DiscardVirtualMemory` when the cache needs pages back. Move to (b) only if we need a *hard* RAM cap or sustained multi-GB/s. Keep (c) as an option behind a capability check, not a v1 dependency |
| D-B where vertex attributes live | (a) keep the global buffer and page it with an index→page table; (b) pack attributes into the group blob (dedup within a group), drop the global buffer for the cluster path | **(b)** — removes the dominant VRAM allocation and matches the upstream layout; (a) is less invasive but keeps a global address space the scene cannot fit and adds an indirection per vertex |
| D-C blob compression | raw; meshoptimizer meshlet codec for micro indices; 16-bit quantised positions/normals/UVs per group | Measure, then adopt both if the numbers hold (raw 4 B/tri vs ~1–2 B/tri) |
| D-D metadata residency | keep all `ClusterGeometry`/`ClusterGroup`/`ClusterRecord`/nodes resident (today) vs page per primitive | **Per-primitive paging** — at 15M clusters the GPU metadata alone is ~1 GB, at 3x that it does not fit |
| D-E RAM cache policy | OS page cache only (mmap) vs explicit LRU of decompressed blobs | OS cache with mmap; add the explicit LRU only when decompression is on (a decompressed blob is not cheap to redo) |
| D-F prefetch | none; one level finer than the cut (needs the §2 overlap table); distance-ordered | **One finer level**, budgeted and cancelled on view change; ordering first (P2) |

### 14.3 Task list

Each task is independently shippable and carries its own verification. Scale estimates: S ≈ a day,
M ≈ a few days, L ≈ a week-plus.

**S0 — Baseline, so the rest can be judged**

- [x] **T1. Instrument the memory breakdown (S).** Log at load and per frame: cooked `m_Clusters`
  vectors, `GpuData` copies, `GeometryStreaming` source arrays, `m_VertexBufferQuantized`,
  index/meshlet arrays, arena reservation/resident, blob-RAM ring, mmap working set. Add the same
  to the streaming stats node.
  *Verify:* one table per scene (Bistro, dragon, the synthetic §T2 scene) that the later tasks must
  move.
- [x] **T2. Scale scene (S).** Done with a real scanned scene instead of a generator:
  `GLTF Scenes\threedscans_animals\animals.gltf` — 5 primitives, 3.94 M optimized vertices,
  56.1 M indices, **18.7 M triangles**. Numbers in §16.
  *Verify:* the cook, the metadata sizing and the arena pressure are all measured on it.
- [ ] **T3. Budgets as first-class config (S).** `--cluster-blob-ram <MiB>`,
  `--cluster-io-mb-per-frame <MiB>`, `--cluster-metadata-ram <MiB>`; debug asserts when a budget is
  exceeded, and stats in the panel. Keep the existing shared VRAM cap untouched.
  *Verify:* setting them very low degrades residency but never crashes or silently over-allocates.

**S1 — Disk-addressable cooked data (RAM unlock)**

- [x] **T4. Split the cache into index + blob sections (M).** Header/meshes/primitives and the
  per-primitive metadata (nodes, node bounds, groups, clusters, generating groups, per-level
  tables) stay in the index section; all per-group `m_Triangles` / `m_VertexIndices` words move to
  a blob section, with a per-group `{uint64 fileOffset, uint32 byteSize}` table, each blob aligned
  to 4 KiB (64 KiB optional for large reads). Bump `SceneCache::kCookedMeshVersion`; document the
  format in the header comment as it is today.
  *Verify:* a `--verify-cache` path re-reads every blob by offset and compares it to the cooked
  arrays for one full Bistro/dragon cook (byte equality), then rendering is unchanged.
- [x] **T5. Stream/map the loader (M).** `LoadCookedMesh` stops `ReadVector`ing blob data; cluster
  blobs become mapped ranges (or are released immediately after `BuildGpuData`). Free the
  `PrimitiveClusters` blob vectors once the GPU-side data exists; keep the metadata needed by
  `ClusterLodScene`'s CPU cut reference or reconstruct it on demand.
  *Verify:* T1's table shows the blob RAM gone, rendering bit-identical, and the CPU cut reference
  still matches (it is the gate for every later change).
- [x] **T6. `BlobSource` abstraction in `GeometryStreaming` (M).** Replace `m_SourceTriangles` /
  `m_SourceVertices` with a source that resolves `group → {mapped pointer, byte size}`;
  `UploadLoadedBlobs` memcpys from the mapping (or from the decompressed cache, §T12).
  *Verify:* same image with the cache mmapped, no full-scene blob vector alive, and a counter for
  bytes read per frame.

**S2 — Vertex attributes (VRAM unlock; the "billions of vertices" enabler)**

- [ ] **T7. Layout spike with numbers (M).** Measure on Bistro/dragon and the synthetic scene:
  (a) current 24 B global vertex buffer; (b) per-group packed attributes with intra-group vertex
  dedup, compressed; (c) paged global buffer with an index→page table. Report blob bytes/triangle,
  per-vertex bytes fetched, MS instruction count, and what each does to the 10^9-vertex case.
  *Verify:* a decision recorded in this section with the numbers, not an opinion.
- [ ] **T8. Implement the chosen layout for the raster cluster path (L).** `ClusterBasePass.hlsl`
  reads attributes from the blob; `ClusterRecord`/`StreamGroup` carry the attribute range; cook
  writes it. Behind `--cluster-vertex-format`.
  *Verify:* image bit-identical against today at a budget that holds everything; blob MB reported;
  the global vertex buffer is no longer required by the cluster path.
- [ ] **T9. Cluster-only cook/load (M).** For scenes rendered by the cluster path, do not cook, load
  or upload `Meshlets`/`MletVerts`/`MletTris` and (if no other pass needs them) the global
  `Indices`/`VerticesQ`. Gate the legacy paths with an explicit switch.
  *Verify:* T1's table shows the legacy arrays gone; the meshlet path is disabled for such scenes
  with a clear UI/log message.
- [ ] **T10. Ray tracing scope decision (S).** The BLAS geometry reads the global vertex buffer
  (`Scene.cpp:143`), so paging it breaks RT as it stands. Decide and document: GB-scale scenes run
  raster-only until a cluster-resolved BLAS exists (the §1/§5 follow-up), or page per-mesh BLAS by
  resident vertex pages.
  *Verify:* a capability gate with a user-visible message instead of a black frame or a crash.

**S3 — Disk → VRAM pipeline**

- [ ] **T11. Geometry IO executor (M).** A dedicated queue (same shape as `AsyncTileIO`, not the
  texture queue) with a few workers that read a blob range into the staging ring, completion
  callbacks fired on the main thread, a PIX marker, and `PrefetchVirtualMemory` for the range after
  the one being read. Bounded by bytes, not by request count.
  *Verify:* reading N MB/frame sustains without main-thread stalls (PIX), and texture streaming
  latency is unaffected.
- [ ] **T12. Decompress on the worker (M, only with D-C).** Meshoptimizer codec + quantised
  attributes; a bounded decompressed-blob LRU so a re-request does not redo the work.
  *Verify:* CPU time per blob, cache hit rate, and bit-identical rendering against the raw layout.
- [x] **T13. Byte-budgeted, ordered requests (M).** Replace the 128-group cap with
  `--cluster-io-mb-per-frame` plus a max-group count; order coarse→fine, then by distance; keep the
  per-frame dedupe tag. Interact with P1: only request groups the cut can draw.
  *Verify:* flying the camera through the synthetic scene never leaves a hole for more than N
  frames, and `dropped` stays bounded.
- [x] **T14. Deepen the request readback (S).** Three in-flight readback slots and a 128-load cap
  throttle anything above ~40 loads/frame. Add slots or a dedicated request ring sized for the new
  budget.
  *Verify:* request→load latency p95 at the target load rate.

**S4 — Metadata at scale**

- [ ] **T15. 32-bit audit and widening (M).** Arena address (31-bit element offset), `ClusterRecord`
  offsets, scene-wide vertex indices (depends on T7), total group/cluster counts, patch/queue
  indices, D3D12 buffer element counts. Widen what the target scale breaks; add `static_assert`s and
  a synthetic overflow unit test for the rest.
  *Verify:* the test fails on any narrowing that would alias, and the 10^9-vertex scene runs.
- [ ] **T16. Per-primitive metadata paging (L).** Keep a small resident per-primitive index (bbox,
  level count, per-level min error, blob table offsets); page node/group/cluster-record arrays by
  primitive with the same request mechanism as blobs. A primitive the cut cannot use keeps only its
  coarse entry.
  *Verify:* `--cluster-metadata-ram` is honoured on the synthetic scene and the cut still matches
  the CPU reference.
- [ ] **T17. Fast first frame (M).** Put each primitive's coarsest level first in the blob section
  and build the initial arena from it (the floor already exists, `m_CoarsestElements`).
  *Verify:* time-to-first-frame on the GB scene is independent of its full size; a coarse-only
  budget still renders a complete scene.

**S5 — Aggressive behaviour**

- [ ] **T18. Budgeted prefetch of one finer level (M).** Uses the §2 overlap table; cancelled when
  the instance leaves the cut or the camera turns; shares the byte budget with demand loads.
  *Verify:* prefetch hit rate and the reduction in "new detail appears after N frames".
- [ ] **T19. Accounting in the UI (S).** Arena reservation/resident, blob RAM ring, mmap working
  set, disk MB/s and IOPS, queue depth, prefetch hits, per-level residency (exists). One line for
  `--cluster-stream-stats-frame`.
  *Verify:* a capture of a tight-budget run explains every megabyte.
- [ ] **T20. Pressure-aware eviction (S).** Combine with §4/§15: cut-gated age, fine→coarse unload
  ordering, and account the decompressed cache in the same budget. Keep the existing pressure gate.
  *Verify:* no request/unload oscillation at a static camera (the §15 failure mode).

**S6 — Validation**

- [x] **T21. Gates (M).** Image equality at a budget that holds everything; `--cluster-no-culling`
  cut match in both buckets; forced-LOD sweep match; `--rhidebug` clean; RenderDoc `hazards` no new
  findings (the 8 pre-existing ones are known).
- [ ] **T22. Stress (M).** A scripted camera path over the synthetic scene with pinned budgets for
  minutes: RSS and VRAM ceilings never exceeded, no load spikes longer than the frame budget, no
  `nvwgf2umx` crashes in Debug.
- [x] **T23. Document (S).** Update this section with the measured numbers and the final switches,
  and point `AGENTS.md` at it for the streaming model.

### 14.4 Scale check

At 10^9 vertices (≈ 1.5×10^9 triangles, ≈ 15–20 M clusters): resident vertex attributes of the
current path would be ~24 GB and the blobs ~10 GB — both impossible. With per-group attributes
plus the meshlet codec the on-disk side is single-digit GB, the metadata is ~1 GB (hence T16), and
the VRAM side is only the resident cut (a few million vertices, tens of MB). Disk bandwidth becomes
the limiter: at ~24 B/vertex of attributes read once, a 16 MB/frame budget loads ~700 k
vertices/frame, which is the number T13's budget and T11's queue depth have to meet.

### 14.5 Risks / non-goals

* **R1. mmap is not a hard RAM cap.** Mapped pages count in the working set and are trimmed lazily;
  if the requirement is a strict ceiling, D-A moves to overlapped IO with an explicit cache (more
  code, same tasks).
* **R2. Per-group attribute duplication.** Without intra-group dedup the blob grows; T7 measures it
  before the layout is frozen.
* **R3. RT parity.** Until cluster BLAS exists, a paged vertex buffer means no RT for these scenes
  (T10 decides the gate).
* Non-goals: virtual-texture-style GPU page faults, GPU-side decompression, animation/deformation,
  and moving the cook off the main process — each deserves its own decision once T1/T2 have numbers.

### 14.6 Implementation status (2026-11-23)

Stage S1 (disk-addressable blobs) and the S3 scheduling pieces are implemented; S2 (attributes in
the blobs) and S4 (metadata paging) remain.

**What changed**

| File | Change |
|---|---|
| `src/SceneCache.{h,cpp}` | Cache **v8**: the per-primitive index record no longer carries the blob words; a per-group table (`ClusterLod::GroupBlobRef`, 16 B/group) and a 4 KiB-aligned blob section follow the legacy arrays. The cook packs exactly the arena layout (group triangles, then its scene-wide vertex indices, 16 B padded) and **reads every blob back afterwards** (≤256 MB sections) to verify the table's offsets. `LoadOrCookMeshData` returns the table + path. |
| `src/ClusterLod.{h,cpp}` | `GroupBlobRef`/`BlobFile`; `WriteToStream`/`ReadFromStream` are metadata-only; `ValidateDag` tolerates an index-only primitive (blob checks only when the arrays are present). |
| `src/ClusterLodGpu.{h,cpp}` | `GpuData` no longer materialises `m_Triangles`/`m_Vertices`; record offsets come from per-primitive counters, and the micro/vertex-index validation only runs while a freshly cooked scene still has the arrays. |
| `src/GeometryStreaming.{h,cpp}` | Maps the cooked file (`MemoryMappedDataReader`), takes the group table, and copies each blob **straight from the mapping** in `Pack`/`UploadLoadedBlobs`. Loads are sorted coarse→fine, bounded by `--cluster-io-mb-per-frame` (default 16 MB) in addition to the 512-group device cap; 8 readback slots. Stats/log carry blob-file MB, bytes/frame and the budget. |
| `src/shaders/ClusterTraversal.hlsl` | The P1 request gate: a leaf group that is already too coarse for this camera is neither queued nor requested (forced-LOD and discrete seeds bypass it). |
| `src/Renderer.h`, `src/Scene.cpp`, `src/Config.{h,cpp}`, `src/ImGuiLayer.cpp`, `src/Renderer.cpp` | Own/plumb the blob file, `--cluster-io-mb-per-frame`, the panel "Load budget" slider and the new stats fields. |
| `tests/ClusterLodTests.cpp` | Round-trip compares metadata; asserts a loaded primitive has no blob words. |

**Measured**

* Bistro cook: 5 378 groups, 25.57 MB of blobs in a 144.93 MB v8 cache; blob verification passed.
* Full budget: cut matches the CPU reference exactly — opaque 41 822, masked 17 852, discrete match.
* 16 MB arena (48 MB shared budget, sampler feedback off, `--cluster-discrete-lod 0`): continuous cut
  **still matches exactly** (41 822 / 17 852) with 2 709 of 5 378 groups resident (15.08 / 16.00 MB) and
  0.92 MB spare — the GPU draws the full cut from a mapped half-scene.
* Request gate (8 MB arena): group-queue entries 6 565 → 5 068 (opaque), requests per frame 512 (capped)
  → 143, cumulative dropped 52 634 → 10 044. Full-budget counts unchanged.
* RenderDoc (`bin/captures/bistrox_capture.rdc`, frame 6): `verify` 0 problems; `hazards` the same known
  HZB / RTXDI-light-PDF / OpaqueColor findings, none on cluster resources; `deps` shows
  `Scene_ClusterStreamData` 513 writes before its read, and the `Cluster Stream Update` / `Cluster
  Traversal` markers present.
* PIX (`bin/captures/bistrox-frame6.wpix`): playback exit 0; event list contains `Cluster Stream Update`
  (`Update` 311 inst + `AgeFilter` 3 129 inst) and both `Cluster Traversal` scopes; `--rhidebug` run is
  clean.

**Still open, in the order the plan needs them**

* S2/T7–T10: attributes are still the one global `VertexBufferQuantized`; blobs carry only indices. This
  is the VRAM unlock for the 10^9-vertex case, and T10 must decide the RT gate with it.
* S4/T15–T17: the per-primitive metadata and the legacy `VerticesQ`/`Indices`/meshlet arrays are still
  loaded whole; the 32-bit audit (T15) is not done.
* T11's dedicated IO executor: loads currently `memcpy` from the mapping inline in `PreRender`. Fine at
  the measured rates; revisit when decompression (T12) or a hard RAM cap is added.

T2 is now measured on a real high-polygon scene (§16) and the ray-tracing capability question is
answered in §17: the raster path needs no SM 6.10 / CLAS / PTLAS, and cluster-level ray tracing is
deferred because Agility 1.620.1 cannot expose `D3D12_OPTIONS_RAYTRACING2`. §16 also adds **T24**
(oversubscribed working-set policy) and **T25** (out-of-core cook).


**Gate note** — under a partial arena the continuous cut matches, but the *total* opaque count is lower
by design: `UpdateDiscreteLevels` clamps discrete instances to the finest fully resident level (a pop,
never a hole), and the reference reads the same host table. Use `--cluster-discrete-lod 0` when the
question is the cut itself, and compare the continuous buckets.

## 15. Q&A: does completely culled geometry stream out, and does it stream back in?

Short answer: **VRAM residency, yes — per group, time-based, and only when the budget forced a
bounded arena. The bytes come back from the cooked mesh file, which is mapped read-only**
(`MemoryMappedDataReader`); system RAM holds the group table, not the blobs. There is no
per-instance or per-geometry eviction; the unit is the group.

### What happens to a fully culled instance

1. `ClusterTraversalInit_CSMain` sphere-tests every instance (`SphereTestsWorld`) before it is
   seeded; a fully culled instance produces no seeds, so none of its groups is visited.
2. If the instance passes but a group's bounds fail the node pass' frustum/HiZ tests
   (`ClusterTraversalRun_CSMain`), the group is never queued either.
3. The traversal's only age reset is the groups pass, after the cut test:
   `g_GroupStates[groupRef.m_GroupIndex].m_Age = 0;` (`ClusterTraversal.hlsl:568`); apart from
   that only a load patch starts a group at age 0. A culled instance's groups therefore stay
   untouched by the traversal and their age keeps rising.
4. `ClusterStreaming.hlsl` (`ClusterStreamAgeFilter_CSMain`) increments `m_Age` and appends the
   group to the unload list when it reaches `m_AgeThreshold` (default 16 frames,
   `--cluster-stream-age`, clamped to ≥2 by the host). It asks exactly **once** per idle
   period, so a group is not re-requested in a loop while the host defers the unload.
5. `GeometryStreaming::ApplyUnloads` honours every request unconditionally: `Free(offset, elements)`
   returns the block to the CPU free list (with neighbour coalescing), `MarkUnresident` drops it
   from the active list, and one `ClusterStreamPatch` writes `kInvalidAddressStart` into
   `ClusterGroupState::m_Address`, which is how the next traversal knows the group is missing,
   but keeps "was requested at frame N" in the low bits.

Two important qualifiers:

* **The age filter only runs under memory pressure.** `DispatchAgeFilter`
  (`src/GeometryStreaming.cpp:598`) returns immediately when
  `m_ArenaElements >= m_TotalElements`, i.e. when the budget already holds every blob. With the
  default budget (the OS-reported VRAM budget, ~9 GB on this machine; Bistro's whole arena is
  ~25.6 MB) nothing ever unloads — culled or not. Stream-out is a tight-budget behaviour.
* **"Stream out" frees arena space, not the reservation.** The arena buffer (`m_StreamData`) is
  sized once per budget (`Resize` only re-packs when the budget value changes); an unload hands a
  block back to the free list, so the VRAM reservation is unchanged and the space is only reusable
  by the next load. `m_Stats.m_ResidentBytes` drops, `m_Stats.m_ArenaBytes` does not.

Also note the granularity: the age reset is **per group**, and the group-level visibility test is a
sphere. A group whose sphere still passes but whose clusters are all occlusion-culled keeps its
age reset, so it stays resident. "Completely culled" has to mean the instance or the group failed
its spatial tests, not merely that every cluster was rejected.

### What happens when it is no longer culled

1. The traversal reaches the group, `GroupIsResident` is false, and `RequestGroup`
   (`ClusterTraversal.hlsl:172`) tags the address with the current frame index and appends the
   group index to the load list (deduplicated per frame by that tag, capped at 128 loads).
2. `GeometryStreaming::PostRender` copies the request list into one of three staging slots;
   `ConsumeCompletedRequests` picks it up once its event has signalled — in practice 2+ frames of
   latency.
3. `ApplyLoads` allocates a block, marks the group resident, and pushes a patch; `UploadLoadedBlobs`
   then writes the blob into the arena with `cmd->writeBuffer` **straight from the mapped file**
   (`GetGroupBlob` → `GroupBlobRef::m_FileOffset`), so nothing of the group is ever materialised in
   a RAM vector.

So a re-entry into view is a few frames of missing (coarser-fallback or absent) geometry, then the
group is back. The bytes are read from the mapped cooked file (so the OS page cache serves repeated
loads); the scene cache is no longer read wholesale at load time.

Because residency is shared per geometry, an eviction also moves
`GeometryStreaming::UpdateDiscreteLevels` — the per-geometry "first fully resident level" that
discrete instances are clamped to — so other instances of the same geometry may pop a level coarser
while the culled one is out. That is by design (never draw a level with holes), not a bug.

### Interaction with the P1 recommendation

Today's traversal requests every leaf group it *visits*, so when a culled instance returns, all its
levels inside the frustum are requested at once (and its coarsest levels are dragged back in even
though the cut will never draw them). With the §3 metric-gated requests, only the groups the cut
could actually draw are requested on re-entry, which both shortens the refill burst and stops the
arena from re-absorbing levels it just evicted. Disk-backed blob streaming is implemented (§14.6);
the remaining system-RAM cost is the per-primitive metadata and the legacy vertex/meshlet arrays
(§14 tasks T9/T16).

## 16. T2 measured: `threedscans_animals` at 18.7 M triangles

### 16.1 What the scene is

| Quantity | Value |
|---|---|
| Primitives | 5 (one mesh each: 65 k, 111 k, 1.07 M, 1.28 M, 1.41 M optimized vertices) |
| Triangles | **18.7 M** (56 115 451 indices) — larger than the 7.9 M estimate, so this is a harder case than planned |
| Cooked cache (v8) | **547.34 MB** (`animals_mesh.bin`) |
| Cluster blobs | **101.16 MB** in 13 549 groups, 26 518 344 arena elements, largest blob 13.1 KB |
| GPU metadata | **8.30 MB** — 25 622 nodes + 25 622 node bounds + 13 549 groups + 5 geometries = **613 B per group** |
| Groups / levels | 13 549 groups, 8 LOD levels |

The metadata-per-group figure is the one that decides T15/T16: 613 B × millions of groups is the
next wall after the vertex buffer (§17.3).

### 16.2 Cook parallelism: job-granular → group-granular

The cook used to parallelise at *job* granularity only: `TaskScheduler::ParallelFor(jobs.size())`
with one **serial** `Build` per primitive. That is fine for Bistro (1 647 primitives fill 28
threads) and wrong for this scene: 5 primitives gave 5-way parallelism, and the dominant mesh
(20.8 M indices) alone set the wall time. `Build` now takes an optional `ParallelForFn` and
`SceneLoader` chooses:

* `jobs.size() >= threadCount` → primitives in parallel, each with a serial per-group loop (the
  Bistro-shaped case, unchanged).
* `jobs.size() < threadCount` → prepare every `BuildInput` in parallel, then build **one primitive
  at a time with per-group parallelism**, so the dominant mesh uses all workers.

Inside `ClusterLod::Build` the three group-level phases run in parallel, each written to its own
slot with read-only inputs (`SDL_assert` on the pre-sized output arrays):

1. per-group decimation → `producedPerGroup`, `groupStuck`, `rawErrorPerGroup`;
2. per-cluster bounding spheres → `lo | (hi << 32)` packed into `clusterSpheres`;
3. per-group next-level `ClusterizeIndices` → `producedPerGroup`.

The merge back into `clusters.m_Groups` stays serial **in group order**, so the cooked DAG layout
is byte-identical to the serial build (`TaskScheduler::ParallelFor` is not nestable; the parallel
regions never call it).

Measured: `Cluster builds: 5 primitives, 28 worker threads -> per-group parallel (few primitives)`,
per-primitive build times 21.5 s and 25.2 s for the two largest meshes, whole scene (cook + write)
inside the 91 s end-to-end run that also rendered 60 frames.

Two builder findings came out of the run:

* **Validation was over-strict.** `ValidateDag` required a level's *worst* group error to be ≥ the
  previous level's worst. That is not a property of a valid DAG: the worst group of level *n+1*
  need not be an ancestor of the worst group of level *n*. It rejected 2 of the 5 primitives
  (`level 6 does not have a larger worst error than the level below`) and would reject any
  real-world asset that has an isolated high-error region that coarsens early. The per-edge check
  (`a group is replaced by a group with a smaller error`) is the property the cut actually needs and
  is kept; the per-level comparison is gone.
* The metric merge with `m_ErrorMergePrev = 1.0` means a level can plateau at the same worst error.
  The per-edge check tolerates equality by design (planar regions simplify at zero cost), so nothing
  else changes.

### 16.3 Rendering and streaming

Full budget, `--cluster-no-culling --cluster-discrete-lod 0`: 13 549/13 549 groups resident
(101.16 MB), 26 518 344/26 518 344 elements, 8 instances, **16 401 clusters, CPU cut reference
match**.

48 MB arena, 16 MB/frame (the first scene whose working set exceeds the arena):

* 6 293/13 549 groups resident (48.00/48.00 MB), 43 096 clusters against a 16 401 reference → the
  cut is **incomplete**, and the run's own no-culling gate fails (exit 1).
* 541 399 dropped load requests over 90 frames, and **0 unloads**: every frame re-requests the same
  groups, all of which are still needed, so nothing ever becomes stale enough to evict. The arena
  reaches a steady state that is permanently short of the cut.

At 128 MB with culling on, everything fits (12 034 groups active) and the cut is 12 532 clusters
against the 16 401 CPU reference — the difference is frustum/HiZ rejection, which the CPU reference
does not model; the gate only requires equality under `--cluster-no-culling`.

**The finding**: residency is all-or-nothing per group, so a budget below the working set produces
a permanently incomplete image instead of a coarser complete one. For a fixed budget over GB-scale
scenes this needs a policy, not a bigger default.

- [ ] **T24. Oversubscribed working-set policy (M).** When the requested set exceeds the arena,
  either raise the LOD of the offending instances until their cover fits (`UpdateDiscreteLevels`
  already does exactly this clamp for discrete instances — extend it to continuous ones), or stop
  requesting the finest groups of an over-budget instance and keep its coarse cover resident.
  *Verify:* the animals scene at 48 MB renders a **complete** cut at a coarser level: zero dropped
  requests once converged, arena at budget, and the discrete-level clamp reported in the panel.

- [ ] **T25. Out-of-core cook (L).** The builder is still fully resident — dedup vertices (12 B),
  indices (4 B), every level's cluster/index arrays and the emitted blob words all live at once,
  so the cook peak is O(scene) (≈ 0.6–1.4 GB for this scene, tens of GB at 10^9 vertices) even
  though the *cache* is now paged. Spill each level's arrays once the next level has consumed them
  and stream the emitted blobs to the cache file.
  *Verify:* the cook's peak working set on this scene drops below the sum of its inputs, byte-equal
  cache, unchanged cut.

## 17. Is SM 6.10 / clustered geometry / PTLAS / indirect AS builds needed?

**Short answer: no for the raster paging path — S2/T7–T9 need nothing beyond what we ship today.
Yes for hit-based streaming, so that part is deferred.**

### 17.1 Why the raster path needs none of it

Everything S2 asks for is mesh-shader work over structured buffers:

| Needs | API/shader model | We have |
|---|---|---|
| Read attributes from the group blob instead of `g_Vertices` | `StructuredBuffer` in a mesh shader | `ms_6_8`/`ms_6_9` entries in `shaders.cfg` |
| Per-cluster draw (already done) | mesh pipeline + `DispatchMesh` | in use (`ClusterBasePass.hlsl`) |
| Indirect/forced LOD | `DrawIndexedIndirect`-equivalent job list | in use |
| 64-bit blob file offsets | host-side only (`GroupBlobRef::m_FileOffset` is `uint64_t`) | done in v8 |

None of these is a "cluster geometry" (DXR 1.2) feature; CLAS/PTLAS are *ray-tracing* acceleration
structures. A raster mesh-shader pipeline cannot consume them anyway.

### 17.2 What would need it, and why we cannot have it yet

Clustered geometry, partitioned TLAS and indirect acceleration-structure builds are the D3D12
**Raytracing 2** feature set:

* `D3D12_OPTIONS_RAYTRACING2` must be reported by the D3D12 core we ship. `CMakeLists.txt:143`
  pins Agility **1.620.1** and `D3D12RHI.cpp` exports `D3D12SDKVersion = 620`, so the option is not
  even *queryable* — this is a structural limit of the SDK we ship, not only of the GPU. The
  preview SDKs that expose it are ≥ 1.716/1.722.
* The shaders must be compiled for **SM 6.10**; `shaders.cfg` pins `-m 6_8`/`-m 6_9` per entry and
  overrides `--shaderModel`, so every entry would have to be retargeted.
* The driver must report `ClustersAndPTLASSupported`. The earlier sample study measured **NO** on
  this machine, so even with a newer core it is not usable.

What our RT path has instead: one BLAS per mesh per LOD level, built at load, with OMM links and
ray queries in 6.9 (`RaytracingCommon.hlsli`). Hit-based streaming needs a **hit → group** mapping.
Without CLAS that means either a BLAS per group (a driver build per streamed group — hundreds per
frame, each a driver round trip: strictly worse than the load budget) or a BLAS per *level* with a
triangle-index → group lookup, which only gives mesh-LOD granularity, i.e. what we already do.

**Decision (T10):** paged cluster geometry is a **raster-only** capability. When the cluster path is
on, the RT path uses the finest *fully resident* level per geometry — the same table
`GeometryStreaming::UpdateDiscreteLevels` already maintains, so RT never traverses geometry the
arena does not hold. Revisit CLAS when Agility ≥ 1.722-preview is adoptable *and* the driver reports
`ClustersAndPTLASSupported`.

### 17.3 What raster actually needs for "billions of vertices"

These are the real gates, in the order they bind. None is a shader-model gate.

1. **Attributes must live in the blob (T7–T9).** — **DONE (§19).** `Scene::m_VertexBufferQuantized`
   was one scene-global 24 B/vertex buffer (1 G vertices = 24 GB of resident VRAM before any cluster
   data). A group blob now carries every cluster vertex's `VertexQuantized` after its triangles, and
   the cluster path never reads the global buffer. This is *the* VRAM unlock, and it is what makes
   the arena a complete geometry representation.
2. **The index inside a blob must stop being scene-wide.** — **DONE (§19).** The triangle micro
   indices were already cluster-local, so the scene-wide `uint32` vertex-index list could be dropped
   outright: a cluster's vertices are now a dense attribute run, and a scene no longer inherits the
   4.29 G-vertex index cap from the blob format.
3. **Metadata must be pageable (T15/T16).** 613 B per group (measured, §16.1). At 10^9 vertices
   (≈ 15–20 M clusters, ≈ 4 M groups) that is ~2.5 GB of index-section RAM even with the blobs fully
   paged. **Not done** — highest-value remaining item.
4. **32-bit audit (T15).** The arena address in `ClusterGroupState::m_Address` is 31 bits
   (`kInvalidAddressStart = 1u << 31`) = **8 GB of resident blob data**, fine as a cap but it must be
   documented and asserted, not discovered. `GroupStreamState::{m_Offset,m_Elements}` are `uint32`
   elements, `GroupBlobRef::m_FileOffset` is already 64-bit. **Not done.**
5. **Compression + a dedicated IO executor (T11/T12).** At 10^9 vertices the on-disk blobs are
   single-digit GB and disk bandwidth is the limiter; 16 MB/frame of *uncompressed* reads is the
   bench number to beat. **Now more urgent**: inline attributes made the blobs 3.1–3.6× bigger (§19).
6. **A fast first frame (T17).** The initial load already preloads the coarsest cover (27 groups /
   123 KB on this scene) before the cut converges; that is the mechanism to keep.
7. **Cook scale (T25)** and **oversubscribed budgets (T24)**, above.

Hardware we do *not* need to change: D3D12 buffer sizes (8 GB arena is address-space, not a
driver limit), mesh-shader output limits (`kMaxClusterVertices`/`kMaxClusterTriangles` already bound
a cluster), or the draw list.

## 18. The two stress scenes: what they actually contain, and which wall binds first

Measured from the glTF JSON itself (accessor counts, no geometry loaded), 2026-11-23.

| | `zorah/zorah_main_public.v2.gltf` | `junkworld/junkworld.v1.gltf` |
|---|---|---|
| Nodes | 16 988 | 258 357 (256 203 with `EXT_mesh_gpu_instancing`) |
| Meshes / primitives | 2 068 / **3 163** | 65 143 / **100 951** |
| Triangles | **1 627.2 M** | **2 292.8 M** |
| Vertices (POSITION count) | **1 050.9 M** | **1 574.2 M** |
| Files on disk | 9.53 GB `.bin` + 9.9 MB `.gltf` | 8.47 GB (12 872 buffers, one `.bin` per mesh) |
| **Decoded size** (glTF `buffer.byteLength` sum) | **41.69 GB** | **25.20 GB** |
| Index width | 1 455 prims `R32`, 1 708 `R16` | 3 196 `R32`, 97 755 `R16` |
| Attributes | **POSITION only**, all 3 163 prims | NORMAL+POSITION+UV 1 936, NORMAL+POSITION 99 015 |
| Extensions | `EXT_meshopt_compression` | `EXT_meshopt_compression`, `EXT_mesh_gpu_instancing` |

The machine has **32 GB RAM** and a 10 GB (9 283 MB streaming budget) card. The walls, in order:

1. **Import is the first wall, and it is not a renderer problem.** `EXT_meshopt_compression` means
   the on-disk sizes above are compressed: decoded, Zorah is **41.69 GB** and junkworld **25.20 GB**,
   and `cgltf_load_buffers` decodes *every* bufferView up front. Zorah cannot be loaded on this
   machine at all, and junkworld only just. Streaming geometry needs a **streamed importer**: parse
   the JSON, then for each primitive decode only its accessors (meshopt `mode` per bufferView),
   build its clusters, append to the cooked file, free the primitive. That is T25, and it is now the
   gate for both scenes — nothing else can be measured until it exists.
2. **Resident vertices.** 1 050.9 M × 24 B = **25.2 GB** (Zorah) and 1 574.2 M × 24 B = **37.8 GB**
   (junkworld) as the scene-global vertex buffer. §19 is what makes this go away; without it neither
   scene can be *drawn*, cooked or not.
3. **Cook time.** Animals (18.7 M triangles) cooks in ≈ 75 s with per-group parallelism, so 1.63 G
   ≈ 1.8 h and 2.29 G ≈ 2.5 h. Acceptable as a one-off, but the importer has to be resumable.
4. **Metadata.** 3 163 / 100 951 primitives is fine (613 B per *group*, not per primitive): at
   ~96 triangles per cluster the two scenes hold ~17 M / 24 M level-0 clusters, i.e. roughly
   1.4 M / 2 M groups → **~0.9 GB / ~1.2 GB** of group metadata plus 32 B per cluster record
   (**0.5 / 0.8 GB**). T15/T16 (paging) is needed, but it is not the first wall.
5. **Instances.** junkworld's 2 M instances are the per-instance arrays and the culling/TLAS paths,
   not the cluster data; the BLAS question is §20.
6. **32-bit.** Per primitive, both scenes stay far below 4.29 G vertices (Zorah averages 332 k
   vertices per primitive), so the scene-wide-index cap is not hit today — but a single *global*
   vertex buffer for them is 25–38 GB, which is exactly what §19 removes.

## 19. Attributes in the group blob — implemented

The first two items of §17.3 are in. Layout of a group's arena blob is now

```
[ triangles : 1 word per triangle, three cluster-local 8-bit micro indices ]
[ attributes : 6 words per cluster vertex — a VertexQuantized verbatim      ]
```

padded to the 16-byte alignment a group address promises. The scene-wide vertex-index run is gone:
triangle micro indices were already cluster-local, so a cluster's vertices were always a dense run,
and the old index list only existed to reach the global buffer.

| File | Change |
|---|---|
| `src/shaders/Cluster.sr` | `ClusterRecord::m_VertexOffset` → `m_AttributeOffset` (into the group blob, six words per vertex). Same record size. |
| `src/shaders/BasePassCommon.hlsli` | `ReadClusterVertex(streamBase, cluster, localIndex)`: six `g_ClusterStreamData` words → `VertexQuantized`. |
| `src/shaders/ClusterBasePass.hlsl`, `ClusterDebug.hlsl` | Read vertices through it instead of `g_Vertices[g_ClusterStreamData[...]]`. |
| `src/SceneCache.{h,cpp}` | Blob **v9**: `GroupBlobByteSize` = Σ triangles + 6·Σ vertices; `packGroup` copies each vertex's `VertexQuantized` out of the scene's packed vertex array (bit-identical to what the global buffer holds); `kCookedMeshVersion = 9`. |
| `src/ClusterLodGpu.{h,cpp}` | `StreamGroup::GetBlobElements()` = `triangles + 6·vertices`; the group-local rewrite sets `m_AttributeOffset = triangles + 6·(primitive-local vertex element)`. |
| `AGENTS`-level docs | `ClusterLod.h` / `GeometryStreaming.h` comments describe the new blob. |

**Verified**

* **Cut unchanged.** Bistro full budget, `--cluster-no-culling`: opaque 45 553, masked 18 106, both
  `(match)`. Animals: 16 401 clusters `(match)`.
* **Attributes are the right ones.** The same scene rendered by the meshlet path (which reads the
  global vertex buffer) differs from the cluster path on **0.51 %** of pixels by more than 8 levels —
  edges and LOD differences only, no shading or UV error.
* **Blob growth** (the honest cost of 6 words per vertex, uncompressed):

  | Scene | blobs before | blobs after | × | cache before | cache after |
  |---|---|---|---|---|---|
  | Bistro | 25.57 MB | **92.35 MB** | 3.6 | 144.93 MB | 211.71 MB |
  | animals | 101.16 MB | **311.56 MB** | 3.1 | 547.34 MB | 757.74 MB |

* `ClusterLodTests` passes; `--rhidebug` run is clean.
* Cache **v9 rejects v8 files** and re-cooks, as intended.
* The tight-arena gate moves with the blobs: Bistro's drawn set is now ≈ 90 MB of its 92.35 MB of
  blobs, so the "16 MB arena still draws the full cut" measurement from §14.6 no longer applies (at
  16 MB the arena holds 17 % of the scene and the cut is legitimately incomplete — the §16.3
  oversubscription case, not a regression in the cut logic).

**What this buys.** VRAM no longer scales with the scene's vertices, only with the resident cut. The
cluster path is now independent of `Scene::m_VertexBufferQuantized`, which is the precondition for
T9 (stop uploading the legacy vertex/index/meshlet arrays for cluster-only scenes). The buffer is
still required by the meshlet base pass, the deferred/path-tracer/SHARC inputs **and the BLAS**
(§20), so T9 is now a "which passes need it" decision rather than a blocker.

**What it costs / what is next.** 6 words per vertex uncompressed is 3.1–3.6× the blob. The next
two tasks in value order are per-group vertex dedup (the same vertex appears in several clusters of
a group) and T12 compression; T7's layout spike should decide between 24 B `VertexQuantized` and a
quantised 12–16 B position (which changes pixels and needs its own gate).

## 20. Ray tracing at this scale: yes, BLAS has to stream — and we cannot stream it

**Measured.** One BLAS per primitive per LOD, `PreferFastTrace`, built at load from the scene-global
vertex and index buffers. `threedscans_animals`, 18.66 M triangles: **1 291.69 MB** of BLAS
(69.2 B per triangle). The log line that reports it is now enabled (`Scene.cpp`).

Extrapolated, unique geometry only (instancing does not multiply BLAS size):

| Scene | Triangles | BLAS ≈ 69 B/tri |
|---|---|---|
| Zorah | 1.63 G | **≈ 112 GB** |
| junkworld | 2.29 G | **≈ 158 GB** |
| 10 GB 3080 (9.3 GB budget) | — | ≈ **145 M triangles** |

So no: a 3080 cannot hold either scene's BLAS — Zorah needs ~12× the card and junkworld ~17×, and
that is before vertex data (25–38 GB, §18) and the rest of the frame. Two structural reasons, not
just a size problem:

* A triangle BLAS is **monolithic per geometry**: the AS references one vertex-buffer range and one
  index range, so a BLAS cannot be partly resident. "Streaming a BLAS" is therefore not a thing —
  the only options are rebuild-per-load (a full driver build of that geometry every time, which
  cannot fit a 16 MB/frame budget) or no RT.
* Our RT path also needs `m_VertexBufferQuantized` and `m_RTIndexBuffer` **resident for the whole
  scene**, which is what §19 just removed for raster.

**What upstream does instead** (`D:\Workspace\vk_lod_clusters`, `renderer_raytrace_clusters_lod.cpp`,
`scene_streaming.{hpp,cpp}`, `partitioned_acceleration_structures.{hpp,cpp}`): the unit of RT
residency is the **cluster acceleration structure (CLAS)**, not the BLAS.

* A fixed-size CLAS buffer with its own budget — `--maxClasMegaBytes`, `--startClasMegaBytes`,
  `--clasGrowMegaBytes` — plus `getRequiredClasScratchSize()`/`getClasSize(reserved)`. Groups are
  loaded and evicted exactly like our geometry blobs; there is even a resident allocator choice
  (simple compaction vs a persistent gap-based allocator that moves a CLAS at most once).
* **Every geometry keeps its lowest-detail CLAS resident** (`m_clasLowDetailBuffer`,
  `getLowDetailBlasAddress`) — the RT analogue of our coarsest-level preload, so a ray always has
  something to hit.
* BLAS are built *from* CLAS (`blas_clusters_insert.comp`, `blas_setup_insertion.comp`,
  `instance_assign_blas.comp`), so a BLAS is a list of cluster-AS references and is small; the
  caching layer has its own budget (`--maxBlasCachingMegaBytes`) with sharing, merging and dedup, and
  cluster AS addresses are pinned while a cached BLAS refers to them.
* The TLAS is partitioned (`PartitionedTlas`, PTLAS) so a re-insert touches one partition.

None of that is reachable on D3D12 with the SDK we ship: CLAS/PTLAS/indirect AS builds are
`D3D12_OPTIONS_RAYTRACING2` (§17.2 — Agility 1.620.1 cannot report it, shaders are pinned to 6.9,
and the driver here reports `ClustersAndPTLASSupported: NO`).

**Decision (T10, restated with numbers).** For scenes of this size RT is off: the raster path draws
the cut from the arena, and RT keeps using the finest *fully resident* per-mesh LOD for ordinary
scenes only. Revisit when a DXR 2 core + SM 6.10 + driver support are all available; the upstream
CLAS budget model is then the design to copy, and our arena/age-filter machinery maps onto it almost
one-to-one.

## 21. RT is now switched off automatically when the geometry cannot fit (implemented)

`Scene::BuildAccelerationStructures()` measures the scene's ray-tracing footprint **before** any
acceleration structure is created and compares it with the GPU budget:

```
footprint = Σ primitive vertices × 24 B        (the buffer a triangle BLAS references)
          + Σ per-LOD indices × 4 B
          + triangles × 69 B                   (measured on animals, §20)
```

| Knob | Meaning |
|---|---|
| `--rt-max-scene-mb <MiB>` | Budget the footprint may use. `0` (default) = `Renderer::GetStreamingBudgetBytes()`, i.e. the card's own streaming budget. Exists so the oversized path can be forced for testing. |
| `Scene::IsRayTracingDisabled()` | State for the UI / other passes. |

When the footprint exceeds the budget:

* no BLAS is built (the whole point — 69 B/triangle is the largest single allocation in the scene),
* `Renderer::m_EnableRTShadows`, `m_EnableReSTIRDI` are cleared and
  `m_IndirectLightingTechnique = NONE` (direct lighting and raster shadows only),
* an **empty TLAS** is created from a one-element placeholder instance buffer, so every shader
  binding and the TLAS-patch pass stay valid instead of dereferencing a null AS,
* two `[RT ]` log lines state the numbers and what was turned off.

Verified: animals with `--rt-max-scene-mb 100` reports *1.66 GB needed (0.09 vertices + 0.23 indices
+ 1.34 BLAS for 19 361 817 triangles), 0.10 GB available*, builds no BLAS, renders correctly and
exits 0; the same scene/budget above the footprint, and Bistro at the default budget (BLAS 258.44 MB,
cut 41 822 / 17 852 `(match)`), are unchanged.

## 22. Streamed, resumable glTF import — design for Zorah and junkworld

Not implemented yet. This is the one blocking item for §18's scene 1, and it has to change three
things at once, which is why it is planned as two stages rather than one flag:

1. **Nothing scene-wide may be materialised.** Today `decompressMeshopt` expands every compressed
   bufferView up front (Zorah: 9.53 GB on disk → 41.69 GB in RAM), and `ProcessMeshes` then appends
   every primitive into scene-global `allVerticesQuantized` (25 GB) and `allIndices` (19.5 GB). The
   load path therefore has to become cluster-only: the cooked cache carries the cluster metadata and
   the group blobs (attributes included, cache v9 layout), and no legacy vertex/index/meshlet arrays.
2. **Blobs must be written while the primitive is in RAM.** `SaveCookedMesh` currently packs a
   group's attributes from `allVerticesQuantized` at the very end. In a streamed cook that array no
   longer exists, so each primitive writes its own group blobs as soon as it is cooked.
3. **Resumability.** A 1.6 G-triangle cook is ~2 h; it must survive a crash and continue.

### Stage 1 — per-primitive import (resumable)

For every mesh primitive, in file order:

1. Read the primitive's accessors **only**: for each `bufferView` its accessors touch, decode it if
   it is `EXT_meshopt_compression` (`mode` → `meshopt_decodeVertexBuffer` / `DecodeIndexBuffer` /
   `DecodeIndexSequence`, then the `filter` step — the existing `decompressMeshopt` loop is the code
   to lift), copy the elements out, then **free the view**. Peak = one primitive plus its views.
   Buffers themselves are mmapped, not read into a vector (the raw `.bin` is 9.5 GB for Zorah).
2. Build `srrhi::Vertex`/indices for the primitive, meshopt-dedup and optimise (as today, bounded by
   the primitive), run `ClusterLod::Build`.
3. Write `<cache>.parts/<primIndex>.bin`: the primitive's metadata record followed by its group
   blobs (`triangles + attributes`, exactly `GroupBlobByteSize`'s layout), then a `.done` marker.
4. Free the primitive. Keep only its metadata record (≈ 20 B/cluster + 64 B/group; Zorah ≈ 2 GB for
   the whole scene, which is why a spill option for the metadata stream is worth having).

Resume is `ls <cache>.parts`: a primitive with a `.done` marker is skipped, a torn part is truncated
and redone. Because a part is self-contained and its blobs are already final, resuming never has to
re-cluster earlier work.

### Stage 2 — assemble

Scan the parts in primitive order, compute the absolute blob offsets (4 KiB-aligned section), write
the header + per-primitive records + blob table, then stream each part's blob region into the file and
**delete the part as it is copied**, so peak disk is the final cache plus one part rather than twice
the cache.

### Cache v10 and the runtime side

* Header gains a `bClusterOnly` flag; the legacy arrays and meshlet vectors are written as empty
  sections (or omitted, with the flag telling the reader), so `LoadCookedMesh` needs no new code path
  and simply yields empty vectors.
* `Scene::CreateAndUploadGpuBuffers` already guards every upload with `if (!…empty())`; the only
  unconditional use is the RT index rebase loop (`allIndices[...]`), which needs a guard, and a
  one-element dummy vertex buffer keeps `BasePassRenderer`'s binding valid for the passes that are
  still recorded. Mesh bounds come from the cooked group bounds instead of `CreateFromPoints` over
  the vertex array.
* RT is already handled: §21's gate turns it off for these scenes automatically, since the footprint
  of a scene that needs paged geometry always exceeds the budget.
* Verification: force the streamed path on `threedscans_animals` (18.7 M triangles, 91 s) and require
  the identical cut and image; then `--import-limit-primitives 200` on Zorah for a first real-data
  test that costs minutes rather than hours; then the full cook.

### 22.1 What is already in place (stage 1, part-file layer)

`src/SceneCache.{h,cpp}` now owns the resumable half of the import:

* `CookedPartHeader` + `GetPartPath()` — one `<cache>.parts/<mesh>_<primitive>.part` per primitive:
  header, the primitive's `PrimitiveClusters` metadata (`ClusterLod::WriteToStream`), its group blob
  table, then the blob bytes, all packed from that primitive's own vertex block (which is what lets the
  blob carry attributes without the scene-wide array).
* `WriteCookedPart()` writes to `<part>.tmp` and **renames** on success, so a part file's existence
  *is* the "done" marker — a torn part is simply absent and gets redone. That is the whole resume
  protocol; no journal is needed.
* `ReadCookedPartHeader()` rebuilds a primitive's scene records (vertex/index counts, per-LOD index
  counts, bounds, group count) without touching the geometry, and `ReadCookedPart()` also restores its
  cluster metadata, so a resumed cook skips the work instead of redoing it.
* `AssembleCookedMesh()` writes the cooked mesh with **empty** legacy vertex/index/meshlet arrays and
  streams the parts' blob regions in, deleting each part as it is copied (peak disk = cache + one part).

Still to wire, in this order:

1. `SceneLoader::ProcessMeshes`: a `--stream-import 1` mode that runs the primitive body **serially**
   (the existing `ParallelFor(jobs.size())` becomes a named lambda and is dispatched either across jobs
   or one job at a time), builds each primitive's clusters with the scheduler inside that loop, calls
   `WriteCookedPart`, and then clears that primitive's vertex/index/meshlet vectors. Peak becomes one
   primitive plus the cluster metadata (Zorah ≈ 2 GB) instead of 44 GB.
2. The merge phase: skip the global index rebasing/appending and take mesh bounds from the cluster
   group bounds instead of `Sphere::CreateFromPoints` over the (now absent) vertex array.
3. `LoadOrCookMeshData`: call `AssembleCookedMesh` instead of `SaveCookedMesh` when the parts
   directory holds the scene.
4. `decompressMeshopt` still expands every compressed bufferView up front; the per-primitive
   decode-and-release (`mode`/`filter` per view, as the existing loop already dispatches) is the piece
   that removes the remaining 41.69 GB spike. Until then a streamed cook needs the raw `.bin` buffers
   to fit, so it is `--import-limit-primitives` that makes the first real-data test affordable.

### 22.2 Stage 1 is implemented and verified on `threedscans_animals` (2026-11-23)

All four items above except the meshopt laziness are in, behind `--stream-import 1` (plus
`--import-limit-primitives <n>` for a first look at a huge scene):

| Where | What |
|---|---|
| `SceneLoader::ProcessMeshes` | the per-primitive body is a named lambda; streamed mode drives it one job at a time, builds that primitive's clusters (still per-group parallel through the scheduler), writes its part, then frees its vertex/index/meshlet/`m_Triangles`/`m_VertexIndices` vectors. A part that already exists and validates is read back instead of rebuilt, so a resumed run is one part per missing primitive. |
| merge phase | `m_IndexOffsets` stay primitive-local and the mesh sphere comes from the per-part bounds recorded in `PrimitiveResult`. |
| `LoadOrCookMeshData` | routes the save to `AssembleCookedMesh` when `<cache>.parts` exists. |
| `CreateAndUploadGpuBuffers` | a placeholder vertex buffer (one vertex) and a placeholder RT index buffer keep every binding valid; the RT index rebase is skipped when there is no index array. |
| `Scene::BuildAccelerationStructures` | `m_bLegacyGeometryAbsent` (set from the load) is now an explicit RT-off reason, so a streamed scene never asks the driver for a BLAS. |

Measured on `threedscans_animals` (5 primitives, 18.7 M triangles, 28 threads):

* streamed cook **213 s** wall (vs 91 s with every primitive in flight — the price of one primitive at
  a time and the serial meshopt/optimise work), 5 parts of 318 MB, assembly **311.56 MB of blobs** in a
  **317.67 MB** cache (the same scene's full cache is 757.74 MB);
* the assembled cache reloads and draws: 13 549 groups, 311.56 MB of blobs mapped, RT auto-disabled
  (`[RT ] Scene has no scene-wide geometry buffers (streamed import)`), and
  `--cluster-no-culling` still reports **16 401 clusters `(match)`** — the exact cut of the
  non-streamed path;
* `--import-limit-primitives 2` produces a working partial cache (8 503 groups) whose screenshot shows
  the correctly shaded statues, which is the cheap first-look route for a huge scene;
* resume was exercised by corrupting a part: the size check rejected it (119 069 376 vs 119 098 688
  bytes, the pre-padding writer) and only that primitive was rebuilt.

What is left for Zorah/junkworld is exactly item 4: `decompressMeshopt` still materialises every
compressed bufferView (41.69 GB for Zorah) before the first primitive is imported. Until that is made
per-primitive, the streamed path removes the *scene-wide* arrays (25 GB + 19.5 GB) but not the decode
spike.

### 22.3 The meshopt decode spike, and how upstream avoids it

**Corrected numbers** (Zorah's own glTF): buffer 0 is `zorah_main_public.v2.bin`, **10.0 GB of
*compressed* data**; buffer 1 has no URI and declares **31.69 GB** — the decoded container the views
expand into. So the spike is **31.69 GB decoded**, not 41.69 (that figure summed both buffers). Every
bufferView carries `EXT_meshopt_compression`, and the largest single view is **6 MB**, so decoding one
view at a time is free.

Our loader does the opposite of that: `SceneLoader::decompressMeshopt` walks *every* bufferView at
load and `malloc`s `count × stride` for each, then `ProcessMeshes` runs — which is 31.69 GB of heap
(plus `cgltf_load_buffers`' own 10 GB vector) before one primitive is touched.

`vk_lod_clusters` solves it in three layers, with **no cgltf fork and no meshopt support inside
cgltf** (their `src/cgltf.cpp` has no meshopt code at all):

1. **The buffers are mapped, not read.** `cgltf_options.file.read/release` are pointed at a
   `FileMappingList` (`src/scene_gltf.cpp:132-151`, wired at `:316-318`), so `cgltf_load_buffers`
   hands back mapped pointers and `meshopt_compression.buffer->data` is a mapping. Our vendored cgltf
   is 1.15 and has the same `cgltf_file_options` hooks, so this needs no library change.
2. **Decode is per geometry and reference counted.** `loadCompressedViewsGLTF`
   (`src/scene_gltf.cpp:1089-1178`) decodes only the views the current geometry's accessors use, with
   a `SpinLock` per view and a `bufferViewUsers[]` count; `unloadCompressedViewsGLTF` (`:1181-1199`)
   decrements and `free`s when the last user lets go. Peak memory is the views in flight, never the
   scene. Their decode logic is explicitly derived from meshoptimizer's own
   `gltf/parsegltf.cpp::decompressMeshopt` — the same code we copied, minus the "all views at once".
3. **A throttled, resumable preprocess pass.** `Scene::init` detects a large scene and re-runs with
   `processingOnly = true` (`src/scene.cpp:386-427`), which only writes their cache
   (`src/scene_cache.cpp:375-500`: header + per-geometry offsets, a `.gltf_mesh_cache_partial` file,
   `--processingpartial` to resume). Geometries are processed one task each, biggest first, gated by
   `estimateGeometryProcessingBytes = 128 B per input triangle` and `--processingmemorygigabytes`
   (default 0 = 60 % of installed RAM; negative = a percentage of it), with `--processingthreadpct`
   (default 50 %). Attributes are read straight out of the decoded view into their quantised format
   (`readAttributesGLTF`, `cgltf_buffer_view_data(view) + accessor->offset`, `:1202-1216`), so no
   intermediate vertex array is built either.

**The fix for us**, in the order it is worth doing (all three keep the part files and the streamed
loop from §22.2 unchanged):

1. **Release what is decoded.** Give `decompressMeshopt` an acquire/release pair keyed on the
   primitive being imported (per-view user count + spin lock, exactly upstream's shape), and call them
   from the streamed per-primitive loop. Peak becomes one primitive's views instead of 31.69 GB.
2. **Map the buffers.** Set `cgltf_options.file.read/release` to a `MemoryMappedDataReader`-backed
   mapping list (the reader already exists in this codebase), which removes the 10 GB vector as well.
3. **Read accessors through the view.** `cgltf_accessor_read_float` per element is the slow path;
   when `accessor->stride == sizeof(T)` the view can be read directly, as upstream does. Worth doing
   in the same edit since the decode path is being touched anyway.

With those, Zorah's decode peak is "one primitive's views" (tens of MB) on top of the ~2 GB of
resident cluster metadata, which is what the 32 GB machine needs.







