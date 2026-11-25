# Render graph — implementation plan for the ranked work items

**Date:** 2026-11-25
**Source:** `docs/render-graph-article-comparison.md` §5 "Ranked plan"
**Scope:** `src/RenderGraph.{h,cpp}`, `src/RenderGraphUI.cpp`, `src/Renderer.{h,cpp}`,
`src/TaskExecutor.cpp`, the D3D12 barrier sink in `external/nvrhi/src/d3d12/d3d12-state-tracking.cpp`,
plus a new `tests/RenderGraphTests.cpp`.

This plan expands each ranked item into concrete steps: files, functions, data structures, ordering,
tests, acceptance criteria and risks. It deliberately keeps the original ranking; do not start an item
before its prerequisites are met.

---

## 0. Ground rules

1. **Measure before optimizing.** Every item that claims a win must report a before/after number from
   the existing profiler (`--hitch-dump` / `bin/profiler_dump.html`) or the graph panel.
2. **Default-off for behavioural changes.** Anything that changes barriers, pass order or memory layout
   ships behind a `Config` flag until the image-parity suites in §9 pass.
3. **No speculative generality.** Typed access kinds and ownership flags are added because the
   barrier work needs them, not as an extension point.
4. **The graph stays a recorder.** Nothing in this plan moves rendering decisions into the graph; it
   only learns more about accesses that renderers already perform.
5. **Every behavioural item ends with a debug-layer-clean run** (`--validation` if available, or the
   D3D12 debug layer) of the default scene, the debug view and the reference path tracer.

---

## 1. Baseline and measurement harness (do first, ~½ day)

Nothing in the plan is verifiable without these numbers. They are cheap and they stay useful.

### 1.1 Graph panel additions (read-only, no behaviour change)

`RenderGraph::Stats` (`RenderGraph.h:138`) gains:

| Field | Meaning |
|---|---|
| `m_HeapBytes` | Σ size of live `m_Heaps[i].m_Heap` |
| `m_LargestFreeBlockBytes` | max free `HeapBlock::m_Size` across live heaps |
| `m_UnAliasedBytes` | Σ memory requirements of every declared resource (textures+buffers) |
| `m_AliasedBytes` | Σ memory requirements of physical owners only (today's `m_Total*Memory`) |
| `m_AliasingSavingsBytes` | `m_UnAliasedBytes - m_AliasedBytes` |
| `m_NumAliasBarriers` | count of `true` in `m_PassNeedsGlobalSyncBarrier` |
| `m_NumDeclaredTextures/Buffers` | already present |

Compute in `Compile()` after `AllocateResourcesInternal`, using the **cached** requirements from P0.1.
Show them in `RenderGraph::RenderDebugUI` (`RenderGraphUI.cpp:24-40`) and in `ExportToString`.

### 1.2 CLI switches (Config + `Config.cpp`)

* `--relayout` — force the allocation layout to be rebuilt from scratch once (needed to evaluate
  P1.1, because today's layout is sticky across frames; see §3.3.1).
* `--rendergraph-strict-access` — enable the declared-vs-actual checker asserts (P1.2).
* `--rendergraph-own-barriers` — P2.2 switch (default off).
* `--rendergraph-reorder` — P3.1 switch (default off).

### 1.3 Test target

Add `tests/RenderGraphTests.cpp` to the existing `ClusterLodTests` pattern (pure CPU, no device):
CMake target `RenderGraphTests`, same `add_test` wiring. It will host the allocator and timeline
tests referenced below. This is a prerequisite for P1.1 and P2.2, both of which are pure algorithms.

**Exit criteria:** panel shows the new numbers; test target builds and runs an empty test.

---

## 2. P0 — cheap, independent fixes

### 2.1 P0.1 Cache memory requirements (effort S)

**Problem.** `RGTextureDesc::GetMemoryRequirements()` / `RGBufferDesc::GetMemoryRequirements()`
(`RenderGraph.cpp:58-72`) **create a virtual nvrhi resource** and query the device. They are called:

* per resource in `SubAllocateResource` (`:1134`),
* per resource in `AllocateResourcesInternal` (`:1259`),
* per alias **candidate** in the alias scan (`:1314`),
* per hovered row in the UI and per resource in `ExportToString`.

**Design.**

```cpp
// RenderGraph.h — RenderGraphInternal::TransientResourceBase (line ~73)
nvrhi::MemoryRequirements m_CachedMemoryRequirements{};
size_t  m_CachedMemoryRequirementsHash = 0;
bool    m_HasCachedMemoryRequirements = false;

const nvrhi::MemoryRequirements& GetCachedMemoryRequirements();
void InvalidateMemoryRequirements() { m_HasCachedMemoryRequirements = false; }
```

* `GetCachedMemoryRequirements()` computes once via the existing virtual `GetMemoryRequirements()`
  and stores the result.
* Invalidate in every place that assigns `m_Hash`: `DeclareTexture` (`:431`) and `DeclareBuffer`
  (`:549`) when the hash changes, on pool reuse (new `desc`), and on new-slot creation. The
  cache key is the desc hash, and the descriptor is the only input to the requirements.
* Replace the three `GetMemoryRequirements()` call sites with the cached accessor.
* UI/export: add `RenderGraph::GetResourceMemorySize(index)` (or use the transient accessor) and stop
  calling `m_Desc.GetMemoryRequirements()` from `RenderGraphUI.cpp` (tooltips ~`:107-120`,
  `:206`, export `:520+`). For resources not declared this frame, fall back to the desc computation.

**Verification.**

* Add a debug counter (or use PIX/ETW) for `createTexture`/`createBuffer` calls with
  `isVirtual = true`; expect it to drop from O(N + N×C) per frame to 0 after the first frame.
* `PROFILE_FUNCTION()` on `Compile()` before/after; record in the PR description.
* `Stats` values must be identical before/after (same numbers, fewer driver calls).

**Risks.** A descriptor change that keeps the same hash would keep stale requirements. The hash
covers every field that feeds `GetMemoryRequirements` (`ComputeHash` at `:20-57`); if a new field is
ever added to those descriptors, it must be added to `ComputeHash` too — add a comment there.
*Device changes* recreate the whole graph (`Shutdown()`), so no cross-device staleness.

**Rollback.** None needed; revert the commit.

---

### 2.2 P0.2 Gate aliasing on enhanced barriers (effort XS) + optional legacy aliasing barrier (S)

**Problem.** `insertGlobalSyncBarrier()` only sets a flag (`d3d12-state-tracking.cpp:355`), and
`commitBarriers()` consumes it **only in the enhanced-barriers branch** (`:113`, `:228-232`), clearing
it there (`:214`, `:260`). The legacy branch (`:262+`) ignores it and never clears it. The graph
enables aliasing from `Config` alone (`RenderGraph.cpp:151`), and `DeviceDesc::enableEnhancedBarriers`
defaults to `true` (`nvrhi/d3d12.h:153`) but the device only gets the enhanced path when the driver
reports support (`d3d12-device.cpp:284-287`). On such a device, aliased memory is used with **no
aliasing barrier at all**.

**Step 1 (required).** In `Reset()`:

```cpp
const bool bEnhancedBarriers = g_Renderer.m_RHI->m_NvrhiDevice->queryFeatureSupport(
    nvrhi::Feature::EnhancedBarriers);
m_AliasingEnabled = Config::Get().m_EnableRenderGraphAliasing && bEnhancedBarriers;
// log once per process when aliasing was requested but disabled
```

**Step 2 (optional, only if we must support non-enhanced-barrier devices).** Add an explicit
aliasing barrier to nvrhi:

* `nvrhi::ICommandList::insertAliasingBarrier(nvrhi::IResource* before, nvrhi::IResource* after)`
  (either may be null, matching D3D12).
* Legacy path: append `D3D12_RESOURCE_BARRIER_TYPE_ALIASING` with
  `Aliasing.pResourceBefore/pResourceAfter`.
* Enhanced path: keep the current global barrier (enhanced barriers have no aliasing group).
* Stubs in `d3d11-*`, `vulkan-*`, `validation-*`.
* Graph: replace the per-pass boolean with a list of `(pass, beforeResource, afterResource)` pairs so
  a scoped barrier can be emitted; fall back to the global barrier if either resource handle is gone.

**Verification.** Force `enableEnhancedBarriers = false` in `D3D12RHI.cpp` (temporary local change) and
run the default scene with the debug layer: aliasing must be reported as disabled, rendering must be
unchanged, and no aliasing-hazard message may appear. With step 2 implemented, alias and re-run with
the debug layer; the aliasing barrier must appear in a PIX capture before the first pass of each
aliased resource.

**Risks.** Some drivers report enhanced-barrier support but still mis-handle aliasing; keep
`--disable-rendergraph-aliasing` (already exists, `Config.cpp:42-44`) as the escape hatch.

---

### 2.3 P0.3 Fix the validation error path and deterministic export (effort XS)

**Problem.** `RenderGraph.cpp:839`:

```cpp
m_PassNames.at(firstAccessPass), m_PassNames.at(buf.m_DeclarationPass)
```

Pass indices are 1-based (`BeginPass()` increments before pushing, `:245`) and `m_PassNames` is
0-based, so the message names the wrong pass and `at(size)` throws `std::out_of_range` when the last
pass is involved — from a diagnostic path.

**Design.**

* Add a tiny helper used by all diagnostics:
  `const char* PassName(uint16_t passIndex1Based) const` returning `"<invalid>"` out of range and
  `m_PassNames[passIndex1Based - 1]` otherwise.
* Replace the `at()` calls (also the texture branch, which reports indices only, for consistency).
* `ExportToString` iterates `std::unordered_set` (`RenderGraphUI.cpp:500-509` and the UI filters at
  `:339-342`). Copy the indices into a `std::vector`, `std::sort` them, then print.

**Verification.** Add a unit test (in the new `RenderGraphTests`) that calls the helper with 0, 1, the
last index and the last+1. Run the same frame twice and diff `ExportToString()` output — must be
byte-identical.

---

## 3. P1 — memory and knowledge

### 3.1 P1.2a Correct the mislabeled accesses (effort S, do before P1.2b)

**Known offender.** `BloomRenderer.cpp:42` declares `WriteTexture(g_RG_TAAOutput)` and `:59` fetches it
with `RGResourceAccessMode::Write`, while `:67` uses it as `SetInputTexture(taaOutput)`. A pass cannot
both write a texture and bind it as an input in the same dispatch; this is a read.

**Steps.**

1. Fix the Bloom access and any equivalent case found by the audit below.
2. Audit script/checklist: for each of the 7 files with access calls, list every `GetTexture(...,
   Write)` and confirm the handle is used as a framebuffer attachment / UAV / storage texture, never
   as an input binding in the same pass.
3. Keep the existing `DeclareTexture`-implies-write behaviour; it is correct for freshly declared
   transients.

**Verification.** `ExportToString` shows TAA output's last writer as `TAARenderer`, Bloom under
"reads"; no lifetime regression in the graph panel.

---

### 3.2 P1.2b Declared-vs-actual access checker (effort M)

**Goal.** Make incomplete declarations visible *before* any barrier work depends on them. Today reads
are declared 32 times vs 67 writes across `src/**`; the graph cannot build correct dependencies from
that.

**Design.** Do not wrap `ICommandList` (too many pure virtuals). Use the fact that most passes go
through two helpers:

* `Renderer::AddComputePass(const RenderPassParams&)` (`Renderer.cpp:2160`),
* `Renderer::AddFullScreenPass(const RenderPassParams&)` (`Renderer.cpp:2081`),

and `RenderPassParams` already carries `bindingSetDesc`, `additionalBindingSets`, `framebuffer`,
`dispatchParams.indirectBuffer` (`Renderer.h:160-190`).

1. After `Compile()`, build a reverse map in the graph:
   `const nvrhi::IResource* → (rgIndex, isTexture)` from `m_Textures[i].m_PhysicalTexture` and
   `m_Buffers[i].m_PhysicalBuffer` (only declared resources).
2. Add `RenderGraph::ValidateActualAccess(std::span<nvrhi::IResource* const> usedResources,
   uint16_t passIndex) const` (debug builds). The renderer helpers build the list from
   `params.bindingSetDesc`, `params.additionalBindingSets`, the framebuffer attachments and
   `dispatchParams.indirectBuffer`, so the graph only sees neutral `nvrhi::IResource*` values. The
   function maps each handle to an RG index, checks it against the pass's declared sets (read for
   binding and read-only-attachment usage, write for attachments and UAVs), and logs or asserts
   (`--rendergraph-strict-access`) with `PassName`, resource name and the missing direction.
3. Call it from `AddComputePass`/`AddFullScreenPass` right after the binding set is built (so the
   binding set is fresh) and from the few remaining direct-binding sites (`BasePassRenderer.cpp:80,
   279`, `CommonRenderers.cpp:231` …). Mark any deliberate exceptions with a comment, never silently.
4. Also warn on the inverse (declared but not used) at `EndSetup()`, excluding resources declared by
   `DeclarePersistent*` that a later pass in a different frame uses.

**Verification.**

* Default frame + debug view + PT mode with `--rendergraph-strict-access`: zero asserts after the
  audit.
* Re-introducing the Bloom bug locally must produce a diagnostic naming `BloomRenderer` and
  `TAAOutput` as an undeclared read.

**Risks.** Bindless static textures and scene-owned buffers are not graph resources; the map lookup
must tolerate "not found" silently for those. Some passes use `Texture_UAV` bindings for writes; the
checker treats any `Texture_UAV`/`Buffer_UAV` bindings in a pass's binding sets as write-usage **and**
read-usage (UAV is read-write) — require the pass to declare `Write` for them, not `Read`.

---

### 3.3 P1.1 Alias allocator v2 (effort M)

**Goal.** Improve peak transient memory beyond the current first-fit/owner-block scheme, and make the
result measurable (the article reports ~50 % of transient memory; we have no baseline yet).

**3.3.1 Make the layout rebuildable.** Today the layout is sticky: once allocated, a resource keeps its
block across frames (trivial-reuse path in `AllocateResourcesInternal`, `:1263-1300`). Ordering changes
therefore only matter when a layout is first built. Add a `m_LayoutGeneration` counter and a
`RebuildLayout()` path (exposed as `--relayout`) that frees all non-persistent blocks and re-runs
allocation. This is also the hook P4 needs.

**3.3.2 Order.** For the resources needing new placement, sort by
`(lifetimeLength descending, firstPass ascending)` instead of `firstPass ascending` (`:1250`). The
article's sand analogy: long-lived resources are hard to alias later, so place them first.

**3.3.3 Candidate selection.** Current loop takes the first adequate candidate in `sortedIndices`
(`:1302-1330`). Change to: among candidates satisfying
`resource.firstPass > candidate.m_PhysicalLastPass && size ≤ candidate block size && alignment fits`,
pick the one with the **largest `m_PhysicalLastPass`** (tightest reuse), tie-broken by smallest block.
Keep the existing `m_PhysicalLastPass` extension.

**3.3.4 Extraction for tests.** Move placement/free-block logic into
`RenderGraphAllocator.{h,cpp}` with a pure interface:

```cpp
struct PlacementRequest { uint32_t index; uint64_t size, alignment; uint16_t firstPass, lastPass; bool persistent; };
struct PlacementResult  { uint64_t heapId, offset; bool aliasedFrom; uint32_t aliasedIndex; };
std::vector<PlacementResult> PlaceResources(std::span<const PlacementRequest>, size_t& peakBytes);
```

`RenderGraph` keeps heap creation and nvrhi binding; the pure part is testable.

**3.3.5 Do not implement yet:** rectangle packing into partial blocks. Our resources occupy exact-size
blocks, and an alias always takes the whole retired block, so partial-block packing only helps when
three or more lifetimes interleave. Evaluate from the P0.4 numbers first; if `m_HeapBytes` is already
close to `m_AliasedBytes` + alignment, skip it.

**Verification (tests).**

* Reproduce the article's small frame (7 resources, known lifetimes) as a unit test; assert the
  placement order and peak bytes are no worse than a hand-computed optimum.
* Two orders (first-pass vs longest-first) over the same input: assert longest-first peak ≤ first-pass
  peak on the synthetic sets, and record the delta.
* On the Bistro default scene: `--relayout` once, compare `m_HeapBytes` and `m_AliasingSavingsBytes`
  against the baseline layout; accept only if peak does not regress and no image change.

**Risks.** Changing the order changes which physical owner a resource aliases, which changes
`AliasedFromIndex` chains shown in the UI and `m_PhysicalLastPass` spreading. Re-run the aliasing
validation in `Compile()` (`:970-1035`); it already asserts aliased first-use is write-only. Keep the
old ordering behind `--legacy-alias-order` for one release if a regression is suspected.

---

## 4. P2 — typed accesses and graph-owned barriers

### 4.1 P2.1 Typed access API (effort M, no behaviour change)

**Goal.** Give the graph enough information to choose a target state per access. Today
`RGResourceAccessMode` is `Read|Write` only, so the graph cannot emit transitions and delegates them
to nvrhi.

**Design.**

```cpp
enum class RGPassKind : uint8_t { Graphics, Compute, RayTracing, Transfer };

enum class RGTextureAccess : uint8_t {
    ShaderRead,        // sampled/read
    UnorderedAccess,   // UAV read-write
    ColorWrite,        // RT attachment
    DepthWrite,        // DSV write
    DepthRead,         // depth as SRV / read-only DSV
    CopySource, CopyDest, ResolveSource, ResolveDest, Present,
};

enum class RGBufferAccess : uint8_t {
    ShaderRead, ConstantBuffer, VertexBuffer, IndexBuffer, IndirectArgument,
    UnorderedAccess, CopySource, CopyDest, AccelStructBuildInput,
};
```

* Add overloads `ReadTexture(handle, RGTextureAccess)`, `WriteTexture(handle, RGTextureAccess)` and the
  buffer equivalents. Keep the old signatures as shorthands:
  * `ReadTexture` → `ShaderRead`; `WriteTexture` (texture) → unresolved until the pass kind/attachment
    is known — in strict mode require the typed form; `WriteBuffer` → `UnorderedAccess`.
* Add `IRenderer::GetPassKind()` (default `Graphics`); the compute renderers override to `Compute`.
  This mirrors the article's "the stage comes from the pass type" for plain reads.
* Store per pass `std::vector<AccessRecord>{ uint32_t resource; uint8_t access; }` alongside the
  existing sets (`PassAccess`, `RenderGraph.h:250`). Sets stay for O(1) validation; the vector feeds
  the timeline.
* Map access → `nvrhi::ResourceStates` in one table (see Appendix B). Graphics-pass `ShaderRead` →
  `PixelShaderResource | NonPixelShaderResource` (`ShaderResource`); compute/RT pass → `NonPixelShaderResource`.
* No barrier is emitted in this phase. The data is used by the checker (P1.2b), by a "would-be
  barrier" counter, and by P2.2.

**Verification.** Every pass in the default frame has a typed access on each resource it declares;
the would-be barrier counter is non-zero only where states differ. Images unchanged.

**Risks.** Writers that also read (UAV storage images) must declare `UnorderedAccess`, not
`ShaderRead|Write`; document this. `DepthRead` (layout `DEPTH_STENCIL_READ`) and `ShaderRead` (layout
`SHADER_RESOURCE`) cannot be merged into one texture state; the group resolution in P2.2 must fall
back to `Common` — see §4.2.

---

### 4.2 P2.2 Graph-owned barriers behind a flag (effort L)

**Goal.** Stop the per-pass `initialState` round-trip and merge consecutive reads. With
`keepInitialState = true` (fork default, `nvrhi.h:530`) `CommandList::close()` emits transitions back
to `initialState` (`d3d12-commandlist.cpp:334-337`), so a transient used by many passes pays a
transition into and out of every pass.

**Prerequisites.** P1.2b (complete declarations), P2.1 (typed accesses).

**4.2.1 Timeline construction.** In `Compile()`, for each declared resource, build
`std::vector<TimelineEntry>{ pass, access, state }` from the per-pass access records in **scheduled
pass order** (which is the declaration order until P3.1 exists). Store on the resource (or in a
parallel array) for the recording phase.

**4.2.2 Read groups.** Consecutive `ShaderRead`/`DepthRead`/etc. entries form one group:

* buffers: merged state is the bitwise OR of the states (legal; nvrhi already ORs buffer states);
* textures: if all merged states map to the same `D3D12_BARRIER_LAYOUT`, OR them; otherwise use
  `ResourceStates::Common` for the group. This is the same trade the article makes with GENERIC_READ,
  and nvrhi's conversion table asserts equal layouts when combining (`d3d12-constants.cpp:520-530`).
* the group stores merged state and the first/last pass, so a later write depends on the last pass of
  the group without scanning.

**4.2.3 Transition lists.** For each resource, emit one transition when the state changes between
groups, placed at the start of the first consumer pass's command list. Include the aliasing barrier
(§2.2 step 2) and the initial transition from `Common` for newly created resources.

**4.2.4 Seeding the nvrhi tracker.** With `setEnableAutomaticBarriers(false)` (`d3d12-state-tracking.cpp:361`)
nvrhi stops inserting implicit transitions (≈40 call sites are gated by that flag), but its tracker
starts at `Unknown` per command list. For every resource the pass touches, first call
`beginTrackingTextureState(tex, AllSubresources, stateAtListEntry)` /
`beginTrackingBufferState(buf, stateAtListEntry)`, then `setTextureState`/`setBufferState` for the
first access state and `commitBarriers()`. Otherwise `require*State` logs
"Unknown prior state" (`src/common/state-tracking.cpp:290-297`).

**4.2.5 Stop the round trip.** Set `keepInitialState = false` on graph-created transients in
`Compile()`'s create-and-bind lambdas (`RenderGraph.cpp:860-950`, where `m_NvrhiDesc.isVirtual = true`
is set) — the graph now owns the end-of-frame state; `close()` will not emit trailing transitions for
them. Keep `keepInitialState = true` for resources not managed by the graph.

**4.2.6 Wiring.** In `ScheduleRenderer`'s pass task (`RenderGraph.cpp:326-340`), before
`pRenderer->Render(...)`:

```
if (m_OwnBarriers) {
    scopedCmd->setEnableAutomaticBarriers(false);
    m_RenderGraph.EmitPassBarriers(passIndex, scopedCmd);   // seeding + transitions + commit
}
```

Persistent resources and resources touched by non-graph command lists (streaming pre/post, uploads)
keep automatic barriers; audit that no such list touches a graph transient.

**Verification.**

* Image parity: default scene, debug view, PT mode, TAA on/off, bloom on/off — byte-identical
  screenshots (existing `--screenshot-frame`/comparison workflow).
* PIX capture: per-frame transition count drops; no transitions back to the initial state between
  passes; no debug-layer barrier errors.
* Fall back to automatic barriers if any resource in a pass has an untyped access (log once).

**Risks.**

* Passes that call `set*State` explicitly (for example the streaming system,
  `Streaming/FeedbackManager.cpp:587-687`) must not touch graph transients; assert if they do.
* Read groups that collapse to `Common` (mixed layouts) can be slower than two transitions on some
  GPUs — measure before making `Common` the default; consider keeping per-layout groups.
* `keepInitialState=false` changes the behaviour of any code that reuses a graph texture outside the
  graph; `GetTextureRaw` users must be audited.

---

## 5. P3 — scheduling

### 5.1 P3.1 ASAP pass scheduling + kind sorting (effort L)

**Prerequisites.** P2.2 (barriers follow the scheduled order), complete declarations.

**5.1.1 Dependencies.** For each pass and each access, the dependency is: last write for a read, last
access for a write — i.e. the article's timeline lookup. Build `passDeps: vector<vector<pass>>` in
`Compile()`.

**5.1.2 Batches.** `batch(pass) = 1 + max(batch(dep))` over its dependencies (0 if none); append new
batches as needed. This is length-of-longest-chain layering; it is optimal for barrier count.

**5.1.3 Submission order.** Pass command lists are acquired in `ScheduleRenderer`
(`RenderGraph.cpp:323`) and pushed into `Renderer::m_PendingCommandLists` by `AcquireCommandList`
(`Renderer.cpp:2375`). Add `Renderer::AcquirePassCommandList(passIndex, kind)` recording
`m_PassCommandList[passIndex] = handle` (do not push yet), and have
`ExecutePendingCommandLists` submit pass lists in `m_ScheduledPassOrder`, keeping non-pass lists
(streaming pre-render, post-render, ImGui, uploads) at their existing relative positions.

**5.1.4 Everything else follows the scheduled order.** `ResourceLifetime`, aliasing (`m_PhysicalLastPass`),
`m_PassNeedsGlobalSyncBarrier`, the debug UI's pass axis and the profiler grouping must all use the
scheduled index, not the declaration index. Add a `m_ScheduledIndex[passIndex]` map and assert
`m_PhysicalLastPass` is documented as "scheduled order".

**5.1.5 Kind sorting.** Within a batch, stable-sort passes by `GetPassKind()` (Graphics < Compute <
Transfer order chosen to minimise NVIDIA subchannel switches; measure). Passes that must stay in
declaration order (e.g. two passes writing the same resource) are already ordered by dependencies.

**Verification.**

* Same image parity suite as P2.2.
* A unit test with the article's two-chain example (GBuf→AO, Cull→Shadow) asserts 2 batches and 1
  barrier, and with the 8-pass example asserts 5 batches.
* PIX: barrier count decreases; the GPU marker order in the timeline matches the scheduled order.
* `--rendergraph-reorder` off must produce the old order exactly.

**Risks.**

* Reordering changes the order in which renderers' `Render()` bodies execute; any pass with an
  undeclared side effect (renderer state mutation read by a later pass) breaks. The strict-access
  checker plus an audit of `m_bClearOnNextRender` and similar flags is mandatory.
* GPU timers are attributed per pass name, not per index, so the profiler keeps working. State that
  in the UI ("pass N (scheduled M)").

### 5.2 P3.2 Wiggle passes for aliasing (effort M)

**Goal.** Move non-critical passes later to create more aliasing opportunities without changing the
batch count.

**Algorithm (article's).** After scheduling, for each pass not on the critical path and not pinned by
dependencies, sweep **from last batch to first**: move it one batch later while (a) no dependency is
violated, (b) `batch(move)` keeps `batch(pass) ≥ 1 + max(batch(deps))`, and (c) total area
`Σ (lifetimeInBatches × size)` decreases. Stop at the first increase. Then rebuild the alias layout
(`--relayout` path) and re-check peak memory.

**Verification.** Synthetic test with the article's cloud example; on Bistro, wiggle on/off must not
increase batch count and must not regress peak heap bytes; record deltas.

**Risks.** Wiggle interacts with kind sorting (article's planting problem); apply kind sorting first,
then wiggle, and do not move a pass into a batch of a different kind unless it improves the barrier
count.

---

## 6. P4 — static compiled plan (effort L, conditional)

**Goal.** Cut per-frame CPU cost: validation scans, sizing, layout and barrier computation.

**Honest assessment:** after P0.1, `Compile()`'s remaining cost is the O(R×P) validation scans and the
allocation walk. If the profiler shows `Compile()` below ~0.1 ms after P0–P3, **do not build P4**.

**Design (if pursued).**

1. During Setup, compute a **frame-graph signature**: hash of `(render mode, debug view, ordered pass
   names, per-resource desc hashes, feature flags that gate passes, resolution)`.
2. Cache `{validation result, placements, barrier lists, scheduled order, batch boundaries}` keyed by
   the signature.
3. Each frame, still run `Setup()` (renderers build bindings from current state), but skip validation,
   sizing and layout when the signature matches; assert the declared resource set matches the cached
   plan exactly (same order and hashes) and fall back to a full compile on mismatch.
4. A settings change naturally changes the signature → one full compile, as today.

**Non-goal:** a fully static graph where `Setup()` is not run. That needs renderers to declare into a
cached graph and rebind dynamic resources, which is a separate project (the article's §"Precomputing
static graphs").

**Verification.** Signature hit rate ~100 % in steady state; `Compile()` CPU time drops to hashing;
toggling any render option causes exactly one miss; image parity unchanged.

---

## 7. P5 — frame pipelining (separate track, effort XL)

**Problem.** `ExecutePendingCommandLists()` calls `waitForIdle()` **before** submitting the frame
(`Renderer.cpp:2605-2615`), serialising CPU and GPU. Aliasing and `FlushDeferredReleases()`
(`RenderGraph.cpp:78`, called from `Compile()` at `:770-776`) rely on that.

**Design sketch.** Not part of the render-graph internals; track separately.

1. Replace the pre-submit wait with a wait on the fence of frame N−2 (or per-queue completion).
2. Double-buffer graph transients *or* the whole aliased heap so frame N's resources are not in flight
   while frame N−1 still uses the same memory. Memory cost: up to one extra heap.
3. Make `FlushDeferredReleases()` fence-driven (`SignalEventAfterFrame`/`pollEventQuery` already exist
   in `Renderer`).
4. Keep the current serialization for `DeclarePersistent*` resources, whose memory is never aliased.

**Verification.** GPU-bound scene throughput (present-to-present) improves measurably; device-removed
and validation clean over a long run with aliasing on and off; image parity.

**Risk.** Any missed hazard is a device-removed or a corrupted frame; do not combine this with P2.2 in
one change.

### 7.1 As built

`Config::m_EnableFramePipelining` (default on, `--frame-pipelining` / `--no-frame-pipelining`).
`RenderGraph::kFrameSetCount = 2` transient heap sets plus `kPersistentFrameSet`; every heap entry and
every transient carries the set it belongs to, `Reset()` picks the set from the frame parity,
`SubAllocateResource` only fits a resource into a heap of its own set, and aliasing only pairs
resources of the same set. `Renderer::WaitForGraphMemoryFrame()` — called before `Reset()` — waits the
event query of the frame that used the set about to be reused (N−2) and resolves that frame's GPU
timers; `ExecutePendingCommandLists()` then submits without waiting when pipelining is on, and records
a new event query for the frame it just submitted. Three invariants make the removal of the pre-submit
`waitForIdle()` safe:

1. Command-list recycling needs no extra fence — nvrhi's `open()` only reuses internal lists whose
   submission has completed.
2. A handle replaced in frame N was last read by frame N−2 (same set), which the fence covers.
3. `m_CurrentFrameSet` is derived from `m_FrameNumber % 2` while `m_SwapChainImageIdx` alternates from
   0, so parity holds; a mismatch (swapchain recreation) logs once and falls back to `waitForIdle`.

The one readback that P5 breaks is the screenshot copy, which now has its own `waitForIdle()` before
mapping.

### 7.2 Measured (Sponza, `--hide-ui`, debug build, `--hitch-log 0.01`)

Correctness: `--rhidebug` 0 errors/0 warnings on both sides; RenderDoc `verify` `problems: 0` for both
captures; RenderDoc `hazards` returns the identical 6 findings / 4 conflicts (the known bindless-range
artefacts) with and without pipelining; the two PIX captures produce **1400 events with a
positional name match of 1400/1400** and identical counters (instructions +0.24 %, sectors +0.02 %) and
both replay with exit 0. Screenshots with `--no-taa --no-auto-exposure` differ on 1.05 % of pixels
(max Δ6), which is below the run-to-run noise floor of the same build (ON vs ON: 4.41 %, max Δ9).

Performance: the first measurement showed pipelining 6 % slower with GPU time +0.5 ms, but that does
not survive clock control — `nvidia-smi` samples show `OFF`'s absolute frame time moving 9.00 → 9.76 ms
between batches, the same ratio as the measured SM clock (1389 → 1280 MHz). With matched clocks
(~1280 MHz, ~151 W, three alternating pairs): frame period ON 9.17–9.25 ms vs OFF 9.73–9.80 ms, GPU
time ON 9.24–9.32 vs OFF 9.46–9.53, clock-normalised GPU ms/GHz ON 11.88 vs OFF 12.17. The overlap is
worth ~5 % here; the CPU's record phase (≈4 ms) is what the serialised path leaves idle.

Costs and caveats: one extra transient heap set (+18 MB VRAM on Sponza), the CPU can never run more
than one frame ahead, and one batch showed a fat tail under pipelining (p95 13.3 ms, p99 15 ms) that did
not reproduce in later batches — treat any A/B of this item as valid only when the SM clock is recorded
alongside it. The renderer's own `gpu` figure includes GPU idle gaps waiting for the CPU, so it is not a
"GPU busy" timer.

Tool notes for anyone repeating this: RenderDoc `passdiff`/`diff` are not a valid A/B here — two
identical OFF runs mismatch 118 of 127 marker positions, because pass order follows task completion.
PIX is the A/B of record because its event list is the execution order.

### 7.3 Why a debug-view frame shows no CPU/GPU overlap

A debug view (Albedo, `--debug 4`) is the cheapest frame the renderer can draw, and it is the frame in
which pipelining *looks* broken: the GPU row has 2.5 ms holes in every frame and its bars sit under the
CPU's sleep, not under the CPU's recording. Measured by decoding the microprofile dump (`bin/analyze_dump.py`,
which rebuilds per-frame CPU scopes and GPU bars and intersects them):

| dump | frame | CPU work | GPU busy | overlap of GPU busy | GPU idle while the CPU worked |
|---|---|---|---|---|---|
| debug view, paced at 200 FPS | 5.51 ms | 3.20 ms | 2.79 ms (51 %) | 18 % | 2.42 ms/frame |
| debug view, `--target-fps 0` | 8.66 ms | 8.13 ms (94 %) | 3.30 ms (38 %) | 83 % | 5.33 ms/frame |
| default view, `--target-fps 0` | 12.38 ms | 12.04 ms | 11.95 ms (96 %) | 96 % | 0.60 ms/frame |

1. **The pacer ends the frame, not the GPU.** `m_TargetFPS = 200` gives a 5 ms period; the dump's `Sleep`
   bar is 2.05 ms of 5.51 ms. The GPU work then runs during that sleep, which is why almost nothing
   overlaps and why the GPU is idle half the frame. `--target-fps <n>` (0 = uncapped) is the switch that
   makes the question measurable; before it, the loop was hardcoded to 200.
2. **The GPU waits for the whole frame to be recorded.** `WaitForAllTasks()` blocks the submission on
   every pass: 1.54 ms/frame in the paced debug view, 4.49 ms in the unpaced one, and its longest single
   window (5.65 ms) is exactly the longest GPU-starvation window. Inside it two workers record
   `Opaque Renderer` (5.5 ms, of which `BasePassRendererBase::PerformClusterTraversal` 4.1 ms) and
   `MaskedPass` (3.25 ms). `ExecutePendingCommandLists()` submits all lists in one `executeCommandLists`
   *after* the slowest one is recorded, so no pass can reach the GPU early. That is the structural
   cost the article's "batches" remove, and the reason a debug view starves the GPU even though its CPU
   work is only 8 ms.
3. **The gap after `Garbage Collection` is a worker row.** GC is dispatched with
   `ScheduleTask(..., bImmediateExecute=true)` → `silent_async`, so it runs on a task-pool thread (0.3–0.65 ms)
   and that thread then idles 1–5 ms until the next frame's passes are queued. It is not on `Main` and it
   is not a stall; it is one of 28 workers waiting for work.
4. **In the GPU-bound frame the overlap is there**: 96 % of GPU busy time is concurrent with CPU work,
   0.6 ms/frame of starvation, and `WaitForGraphMemoryFrame` is 5.99 ms/frame — the intended wait on
   frame N−2, because the GPU is the bottleneck now.

Consequence: removing the starvation needs the passes to be submitted as they finish recording, not a
different pipelining depth — P3.1's ASAP scheduling is what puts a pass's list on the queue while the
next pass is still being recorded. `WaitForAllTasks()` also waits on *every* task in the executor rather
than the frame's pass group, so an unrelated fire-and-forget task can delay submission; a taskflow task
group per frame would make that wait exact.

---

## 8. Sequencing

```mermaid
graph TD
    B[1. Baseline: stats + flags + test target] --> P01[P0.1 memreq cache]
    B --> P02[P0.2 enhanced-barrier gate]
    B --> P03[P0.3 pass-name + export]
    P02 --> P02b[P0.2b legacy aliasing barrier (optional)]
    P01 --> P11[P1.1 allocator v2]
    B --> P12a[P1.2a fix mislabeled accesses]
    P12a --> P12b[P1.2b declared-vs-actual checker]
    P11 --> P21[P2.1 typed access]
    P12b --> P21
    P21 --> P22[P2.2 graph-owned barriers]
    P22 --> P31[P3.1 ASAP scheduling + kind sort]
    P31 --> P32[P3.2 wiggle]
    P22 --> P4[P4 static plan - conditional]
    P22 --> P5[P5 frame pipelining - separate]
```

Milestones:

| Milestone | Contains | Exit gate |
|---|---|---|
| M0 "instrumented" | §1, P0.1–P0.3 | Panel numbers; compile CPU delta; no visual change |
| M1 "truthful graph" | P1.2a/b | Strict checker green on all modes |
| M2 "better memory" | P1.1 | Peak heap bytes ≤ baseline, savings measured |
| M3 "owned barriers" | P2.1/P2.2 | Image parity + PIX barrier reduction |
| M4 "scheduled" | P3.1/P3.2 | Batch test matrix + parity |
| M5 "cheap CPU" | P4 (if justified) | Compile time |

---

## 9. Acceptance matrix (run for every behavioural item)

| Configuration | Command | Expectation |
|---|---|---|
| Default deferred | `--scene bistro --screenshot-frame 120` | byte-identical to baseline |
| TAA off | `--no-taa` (existing switch) | byte-identical |
| Bloom off | panel/CLI toggle | byte-identical |
| Aliasing off | `--disable-rendergraph-aliasing` | byte-identical |
| Debug view | debug-view switch | byte-identical |
| Reference path tracer | PT mode | byte-identical |
| Non-enhanced barriers | temporary `enableEnhancedBarriers=false` | renders; aliasing disabled (P0.2) |
| Strict access | `--rendergraph-strict-access` | zero diagnostics |

Record the graph panel numbers (`m_HeapBytes`, `m_UnAliasedBytes`, `m_AliasingSavingsBytes`,
`m_NumAliasBarriers`) and `Compile()`/`Frame` CPU times for each row in the PR description.

---

## 10. Risks and open decisions

1. **Declaration completeness is the gate.** If P1.2b cannot reach zero mismatches (for example due to
   bindless accesses the checker cannot see), P2.2 must not ship. Decide whether bindless static
   resources need a separate declaration mechanism (they are outside the graph today).
2. **Read groups defaulting to `Common`.** On some GPUs a `Common` group is slower than two precise
   transitions. Keep a per-resource override and measure on the target GPU (RX 9070 XT / RTX 4080 per
   the article's numbers).
3. **Reordering side effects.** `Setup()` is stateful and several renderers read globals written by
   earlier ones (`g_RG_TAAOutput`, etc.). P3.1 must only reorder passes whose *execution* is
   independent, which is exactly what dependencies encode — but any hidden side effect breaks it.
4. **`m_PhysicalLastPass` semantics.** It is a pass index today and doubles as both "lifetime end" and
   "block occupancy end". Rename/document it before P3.1 to avoid aliasing a block whose owner is
   scheduled later.
5. **P4 is conditional.** Do not build it before P0.1's savings are in; the residual CPU cost may not
   justify the risk.
6. **P5 is orthogonal.** Do not couple it to the graph-owned barrier work; a bug in either will be
   impossible to bisect.

---

## Appendix A — proposed API sketches

```cpp
// RenderGraph.h (public)
enum class RGPassKind : uint8_t { Graphics, Compute, RayTracing, Transfer };
enum class RGTextureAccess : uint8_t { ShaderRead, UnorderedAccess, ColorWrite, DepthWrite, DepthRead,
                                       CopySource, CopyDest, ResolveSource, ResolveDest, Present };
enum class RGBufferAccess : uint8_t { ShaderRead, ConstantBuffer, VertexBuffer, IndexBuffer,
                                      IndirectArgument, UnorderedAccess, CopySource, CopyDest,
                                      AccelStructBuildInput };

void ReadTexture(RGTextureHandle, RGTextureAccess);      // new
void WriteTexture(RGTextureHandle, RGTextureAccess);     // new
void ReadBuffer(RGBufferHandle, RGBufferAccess);
void WriteBuffer(RGBufferHandle, RGBufferAccess);

// debug/validation
void ValidateActualAccess(std::span<nvrhi::IResource* const> usedResources,
                          uint16_t passIndex) const;
const char* PassName(uint16_t passIndex1Based) const;

// diagnostics/export
size_t GetResourceMemorySize(uint32_t index, bool isBuffer) const;      // cached
```

```cpp
// Renderer.h
virtual RGPassKind GetPassKind() const { return RGPassKind::Graphics; }
```

```cpp
// nvrhi (optional P0.2b)
virtual void insertAliasingBarrier(IResource* before, IResource* after) = 0;
```

## Appendix B — access → `nvrhi::ResourceStates` mapping

| Access | State | Enhanced layout |
|---|---|---|
| texture `ShaderRead` (graphics pass) | `PixelShaderResource | NonPixelShaderResource` | `SHADER_RESOURCE` |
| texture `ShaderRead` (compute/RT pass) | `NonPixelShaderResource` | `SHADER_RESOURCE` |
| texture `UnorderedAccess` | `UnorderedAccess` | `UNORDERED_ACCESS` |
| texture `ColorWrite` | `RenderTarget` | `RENDER_TARGET` |
| texture `DepthWrite` | `DepthWrite` | `DEPTH_STENCIL_WRITE` |
| texture `DepthRead` | `DepthRead` | `DEPTH_STENCIL_READ` |
| texture `CopySource`/`CopyDest` | `CopySource`/`CopyDest` | `COPY_SOURCE`/`COPY_DEST` |
| texture `ResolveSource`/`ResolveDest` | `ResolveSource`/`ResolveDest` | `RESOLVE_SOURCE`/`RESOLVE_DEST` |
| texture `Present` | `Present` | `PRESENT` |
| buffer `ShaderRead` | `PixelShaderResource | NonPixelShaderResource` (graphics) / `NonPixelShaderResource` (compute) | n/a |
| buffer `ConstantBuffer` | `ConstantBuffer` | n/a |
| buffer `VertexBuffer`/`IndexBuffer` | `VertexBuffer`/`IndexBuffer` | n/a |
| buffer `IndirectArgument` | `IndirectArgument` | n/a |
| buffer `UnorderedAccess` | `UnorderedAccess` | n/a |
| buffer `CopySource`/`CopyDest` | `CopySource`/`CopyDest` | n/a |
| buffer `AccelStructBuildInput` | `AccelStructBuildInput` | n/a |

Read-group merge rule: OR the states when (buffers) or when all texture states share one layout;
otherwise `ResourceStates::Common`.
