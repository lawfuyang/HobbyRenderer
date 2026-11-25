# Render graph review: our `RenderGraph` vs "Practical, efficient render graphs"

**Date:** 2026-11-25
**Article:** *Practical, efficient render graphs* — Ipotrick (Patrick), 2026-11-20,
<https://ipotrick.github.io/site/blogs/practical-efficient-rendergraphs/>
(local copy: `REFERENCES/Practical, efficient render graphs _ Patrick's Pretty Pixels.html`,
text extract: `REFERENCES/_rg_article.txt`)
**Code under review:** `src/RenderGraph.h`, `src/RenderGraph.cpp`, `src/RenderGraphUI.cpp`,
`src/Renderer.cpp` (`ScheduleAndRunAllRenderers`, `ExecutePendingCommandLists`),
`src/Renderer.h` (`IRenderer`), `src/TaskExecutor.cpp`,
plus the nvrhi barrier sink in `external/nvrhi/src/d3d12/d3d12-state-tracking.cpp`.

---

## 1. TL;DR

The article describes a render graph whose **core** is a *resource timeline* per resource, from which
**barriers, access groups, batches, pass order and aliasing** are all derived. Our graph is a
**two-level recorder with aliasing** — it has the recording model and a decent allocator, but it
**does not generate barriers at all**: every barrier comes from nvrhi's per-command-list automatic
state tracking, and each pass is its own command list submitted after a full `waitForIdle()`.

| Article concept | Ours | Verdict |
|---|---|---|
| Two-level command recording | Yes (`Setup` records, `Render` replays later) | **Matches** |
| Value-semantics resource handles, no versions | Yes (`RGTextureHandle` = `uint32` index) | **Matches** the article's preferred API style |
| Resource timelines (ordered accesses per resource) | Only `firstPass`/`lastPass` + per-pass *sets* | **Partial** — enough for aliasing, not for barriers |
| Barriers generated from timelines | **No** — delegated to nvrhi auto-tracked states | **Missing (core)** |
| Access kinds (SRV vs RT vs indirect vs copy…) | Only `Read`/`Write` | **Missing (core prerequisite)** |
| Read groups / merged read layouts | No | **Missing** |
| Batching passes, merged boundaries | No — 1 pass = 1 command list | **Missing** |
| Reordering passes (ASAP scheduling) | No — fixed schedule list | **Missing** |
| Memory aliasing | Yes — pool + first-fit + owner-block reuse | **Present**, allocator differs |
| Wiggle passes for aliasing | No | Missing |
| Sort passes by kind (NVIDIA subchannel switches) | No | Missing |
| Split barriers | No | Missing — article itself says drivers don't honour them |
| Async compute / multiple queues | No (graphics + copy queue only) | Missing — article recommends manual-only |
| Precomputed/static graph | No — full rebuild per frame, but heaps/handles cached | **Missing (CPU optimization)** |
| Don't track static resources | Yes — only declared resources are tracked | **Matches** |
| Whole-resource state, no subresource tracking | Yes | **Matches** the article's recommendation |
| Bindings generated from attachments | No — manual binding sets | Missing (ergonomics) |
| Debug tooling | Yes — lifetime chart, heap map, text export | **Ahead of the article** |

The single most important sentence for us: *"Every dependency limits reordering… the timelines
already declare logical order."* Our graph doesn't currently **know** the logical order of most
reads (see §4.4), so even the prerequisites for the article's optimizations are partly absent.

---

## 2. How our graph actually runs (the baseline to compare against)

Per frame (`Renderer::ScheduleAndRunAllRenderers`, `Renderer.cpp:1536`):

1. `Reset()` — re-reads `Config::m_EnableRenderGraphAliasing`, clears pass list, resets lifetimes,
   evicts resources idle for >3 frames (`RenderGraph.cpp:147`).
2. `BeginSetup()` — asserts the previous `Reset()` happened and that `PostRender()` ran.
3. For each renderer, in a **hard-coded order**: `Setup()` runs inline on the calling thread
   (`RenderGraph.cpp:277`), declares/reads/writes resources, then `BeginPass()` commits the pending
   access set and bumps the pass index. `ScheduleRenderer` then **acquires one nvrhi command list per
   pass** and queues the pass's `Render()` as a *deferred* task.
4. `EndSetup()` → `Compile()`:
   * validation scans (declared-but-never-accessed, access-before-declaration),
   * `AllocateResourcesInternal` twice (textures, then buffers): pool reuse → aliasing → heap suballocation,
   * computes `m_PassNeedsGlobalSyncBarrier[]` (one full global barrier before the first pass of each
     aliased resource).
5. `WaitForAllTasks()` — the deferred pass tasks are pushed to the taskflow pool and the **passes are
   recorded concurrently** (`TaskExecutor.cpp:143`).
6. `PostRender()` → `ExecutePendingCommandLists()` (`Renderer.cpp:2601`): **`waitForIdle()`**, then a
single `ExecuteCommandLists` of every acquired list in order (`d3d12-device.cpp:694-704`).

Consequences that matter for the comparison:

* **Pass order is the schedule order.** Nothing reorders, batches or re-lays-out passes.
* **Passes are never merged into one command list** (one per renderer), so barriers cannot be merged
  across pass boundaries.
* **The graph owns no barriers.** Inside each list, nvrhi's `CommandListResourceStateTracker` inserts
  transitions from binding/framebuffer usage, and because this fork defaults
  `TextureDesc::keepInitialState = true` (`nvrhi.h:530`), `CommandList::close()` calls
  `keepTextureInitialStates()`/`keepBufferInitialStates()` (`d3d12-commandlist.cpp:334-337`).
  A transient therefore **round-trips to its `initialState` at the end of every pass** in which it is
  used, and transitions back at the start of the next one.
* **Frame N cannot overlap frame N−1**: the pre-submit `waitForIdle()` serialises CPU and GPU.
  Aliasing and `FlushDeferredReleases()` rely on this (`RenderGraph.cpp:773`, `Renderer.cpp:2608`).
* **CPU cost scales with resources**: every `GetMemoryRequirements()` call *creates a virtual
  resource* through nvrhi (`RenderGraph.cpp:58-72`) and is called per resource *and per alias
  candidate* every frame (see §4.7).

---

## 3. Feature-by-feature: what we can implement from the article

### 3.1 Resource timelines and graph-generated barriers (the core) — **implementable, high value**

The article's whole pipeline is: build `resource → [pass, access]` lists, then derive everything
else. We already store `PassAccess` (`RenderGraph.h:250`) and the pass order, so the timeline can be
built in `Compile()` with one O(P×A) sweep. What we lack is:

* **access kinds.** `RGResourceAccessMode` is `Read|Write` only. A read needs a target state
  (`ShaderResource` vs `NonPixelShaderResource` vs `IndirectArgument` vs `CopySource` vs
  `DepthRead` vs `RenderTarget`…). This is the same point the article makes with typed attachments
  (`color_attachment_write`, `indirect_command_read`, …). Without it, the graph cannot emit
  transitions — which is exactly why barriers are delegated to nvrhi today.
* **complete declarations.** See §4.4; most reads are not declared.

Integration path with nvrhi is clean: for a graph-owned barrier set, call
`commandList->setEnableAutomaticBarriers(false)` and then `setTextureState`/`setBufferState` for the
transitions the graph computed, before the pass body. nvrhi's explicit state API already accepts the
exact `ResourceStates` values the graph would choose. Note this also means the pass bodies can no
longer rely on binding-driven state; that is the price of ownership and must be a deliberate step.

**Recommendation:** do this only after §4.4 (declaration enforcement), otherwise the generated
barriers would be based on incomplete data and break rendering.

### 3.2 Access groups (merged reads) — **implementable, medium value**

With `keepInitialState = true` + one command list per pass, a resource that flips
`RenderTarget → ShaderResource → RenderTarget` across passes pays two barriers per pass. Merging
consecutive reads into a group whose merged state is `ShaderResource` (or `GenericRead` with enhanced
barriers) removes the read→read transitions and the back-and-forth. This is cheap to add once
timelines exist.

### 3.3 Batching (fewer, merged barriers) — **implementable, but conflicts with parallel recording**

The article's batching needs several passes in **one command list** so their barriers become one API
call at a batch boundary. We deliberately use one list per pass to record in parallel on the
taskflow pool. Options:

* batch only "cheap" passes (clear, compute) into a single list on one thread; keep heavy passes
  parallel;
* or keep one list per pass and accept per-list barriers (D3D12 cannot merge barrier calls across
  lists).

Honest assessment: the default branch schedules 13 renderers (plus two debug renderers and ImGui,
most of which disable themselves) — `Renderer.cpp:1561-1577` — so the win is small. It matters only
if the pass count grows.

### 3.4 Reordering passes down to the critical path — **blocked by API design, then implementable**

Requires (a) complete dependencies (§4.4), (b) an execution path that can reorder *command lists*
(submission order can change freely — lists are already independent), and (c) passes that are pure
functions of their declared resources. Currently `Setup()` mutates renderer state and later renderers
read globals written by earlier ones (`g_RG_TAAOutput` etc.), and several renderers read resources
they never declared, so reordering would be wrong today.

If declarations are fixed, the article's algorithm is trivial to add: `earliestBatch(pass) = 1 +
max(over accesses)(batch of the dependency)`, appending batches as needed, then emit barriers at
batch boundaries.

### 3.5 Aliasing — **already present; the allocator is the difference**

Ours (`RenderGraph.cpp:1236-1460`):

* sort resources by **first pass ascending**;
* reuse existing owners when the new resource's `firstPass > owner.m_PhysicalLastPass`, fits the
  owner's block and meets alignment;
* otherwise first-fit into the free-block list, else next-pow2 heap.

Differences from the article:

1. **Sort order.** The article sorts by **lifetime length descending** ("big grains first"). We sort
   by start time, which tends to interleave short and long lifetimes and fragments earlier.
2. **Placement.** The article does 2D rectangle packing (time × offset) and places each resource at
   the **lowest offset where it fits**. We only alias into an existing *owner's whole block*; a 4 MB
   resource cannot reuse the free tail of a 24 MB block it could legally overlap in time.
3. **Persistence.** We additionally keep heaps and slots across frames (a strength the article doesn't
   cover).
4. **Cost side.** Each alias can force a **global sync barrier** (full `SYNC_ALL` drain, see §4.1), so
   an alias is not free: our allocator doesn't model that cost at all.

### 3.6 Wiggling passes for aliasing — **implementable later**

Only after reordering exists. The article's back-to-front sweep with an "area = lifetime × size"
cost is ~50 lines and testable in isolation.

### 3.7 Sorting passes inside a batch by kind — **cheap, low risk, small win**

Tag renderers `graphics/compute/transfer`, stable-sort within a batch/group. On NVIDIA this reduces
subchannel switches; the article's own numbers are modest. Cheap to try behind a config flag.

### 3.8 Split barriers — **do not implement**

The article concludes drivers on NVIDIA and AMD treat split barriers as normal barriers at the wait,
so the result is more waits, not fewer. Skip.

### 3.9 Async compute / multiple queues — **do not automate**

The article's conclusion (vendor/GPU dependent, needs hand tuning, submits are expensive) matches our
current single-queue design. A manual "section" API is possible later; not worth it now.

### 3.10 Static / precomputed graph — **implementable, real CPU win**

Timberdoodle and Activision report recording costing 0.1–1 ms+ per frame. Ours does:

* a setup pass per renderer (string-keyed registry lookups, descriptor construction),
* O(R×P) validation scans,
* `createTexture(..., isVirtual=true)` for **every** resource's memory requirements (multiple
  times per resource, §4.7),
* heap block vector churn.

A cached "compiled plan" keyed by `(render mode, debug view, feature toggles, resolution/format)`
would skip validation, sizing and layout, and only re-bind dynamic resources per frame. That is a
larger refactor; the cheap 80% is caching memory requirements and skipping the validation scans when
the plan hash is unchanged.

### 3.11 Bindings from attachments — **ergonomic, optional**

The article's "shared binding indices" / "static reflection with macros" would remove a lot of
per-pass binding-set code. Our `Renderer::CreateBindingSetDesc(inputs)` pattern already centralizes
some of it. This is a design change for later; no correctness value.

---

## 4. Bugs, correctness risks and measurement gaps

### 4.1 Aliasing has no barrier without enhanced barriers, and the flag is never cleared

`insertGlobalSyncBarrier()` only sets `m_PendingGlobalSyncBarrier = true`
(`d3d12-state-tracking.cpp:355-359`). `commitBarriers()` emits it **inside the enhanced-barrier branch
only** (`:113`, `:228-232`) and resets the flag there (`:214`, `:260`). The legacy-barrier branch
(`:262` onward) ignores and never clears it.

* `DeviceDesc::enableEnhancedBarriers` defaults to `true` (`nvrhi/d3d12.h:153`), but the device only
  gets the `ID3D12Device10`/enhanced path when the driver reports `EnhancedBarriersSupported`
  (`d3d12-device.cpp:284-287`).
* On a device without it, the graph still aliases (it checks only `Config`, `RenderGraph.cpp:151`)
  and **no aliasing barrier is emitted at all** — a placed-resource aliasing hazard that D3D12
  requires a barrier for. Legacy D3D12 would need an `D3D12_RESOURCE_BARRIER_TYPE_ALIASING` entry,
  which nvrhi doesn't currently expose.

**Fix (either):** gate `m_AliasingEnabled` on `device->queryFeatureSupport(Feature::EnhancedBarriers)`
and log when disabled; or add the aliasing barrier to nvrhi's legacy path.

### 4.2 The global alias barrier is a full pipeline drain

`globalBarrier.SyncBefore/After = D3D12_BARRIER_SYNC_ALL`, `AccessBefore/After = ACCESS_COMMON`
(`d3d12-state-tracking.cpp:228-232`). One per aliased resource's first pass. Since aliasing is on by
default and the allocator happily aliases many pairs, the barrier cost is unbounded and invisible in
our stats. Prefer per-resource enhanced barriers on the two aliased resources (or a global barrier
only when the previous user is on another queue), and **count** them.

### 4.3 Validation error path can throw instead of logging

`RenderGraph.cpp:839`:

```cpp
m_PassNames.at(firstAccessPass), m_PassNames.at(buf.m_DeclarationPass)
```

`firstAccessPass` and `m_DeclarationPass` are **1-based** (`BeginPass()` increments before pushing,
`:245-250`), while `m_PassNames` is 0-based. The message prints the wrong pass name and, when the last
pass is involved, `at(size)` throws `std::out_of_range` — from a diagnostic path. Same 1-based/0-based
mix in the texture branch (which doesn't use names, but reports indices).

**Fix:** `m_PassNames.at(firstAccessPass - 1)` (and same for declaration pass), and pre-size/guard.

### 4.4 Access declarations are incomplete — the graph does not really know the frame's dependencies

Measured across `src/**`: **32 `ReadTexture/ReadBuffer` calls vs 67 `WriteTexture/WriteBuffer`**
(`Read` in 7 files, `Write` in 7 files). Concrete evidence in a tiny renderer:

* `BloomRenderer.cpp:42` declares `WriteTexture(g_RG_TAAOutput)` and
  `:59` fetches it with `RGResourceAccessMode::Write`, while `:67` binds it as
  `SetInputTexture(taaOutput)` — it is a **read**. The write declaration passes validation
  (see §4.5) and poisons the timeline: `g_RG_TAAOutput`'s last write becomes Bloom, not TAA.

Effects: aliasing is conservative (lifetimes extended), dependencies for any future reordering are
wrong, and any graph-generated barrier would be wrong. **This is the gating issue for §3.1–3.4.**

**Fix:** (a) correct the mislabeled accesses, (b) add a debug mode that records the resources a pass
actually binds/uses (wrap `setGraphicsState`/`setComputeState`/framebuffer/`drawIndirect` in a debug
build) and asserts they are a subset of the declared sets, (c) make `DeclareTexture` not silently
imply a write that a read-only declaration contradicts.

### 4.5 `GetTexture(Read)` accepts a pass that only declared a write

`RenderGraph.cpp:1502` and `:1552`:

```cpp
found |= passAccess.m_WriteTextures.count(...) > 0; // allow read if write declared
```

The comment's rationale ("write implies read") is only true *after* the write within the same pass,
and this check exists precisely to catch missing declarations. It let the Bloom case above through.
Consider a distinct "read-write" mode or requiring both declarations for read-after-write in one pass.

### 4.6 Frame-level `waitForIdle()` prevents CPU/GPU overlap

`ExecutePendingCommandLists()` waits for idle **before** submitting the current frame
(`Renderer.cpp:2605-2615`). Combined with the article's "keep the GPU full" premise, this is the
largest throughput loss in the whole frame — bigger than any barrier micro-optimization. It is what
makes cross-frame aliasing and immediate handle release trivially safe.

**Fix (larger project):** N-frame transient heaps (or per-frame heap pools) + fence-based deferred
release instead of the pre-submit wait. nvrhi already exposes event queries and
`runGarbageCollection`. Track as a separate initiative; it is not part of render-graph internals.

### 4.7 Memory requirements are re-queried per frame, per resource, per candidate

`RGTextureDesc::GetMemoryRequirements()` (`RenderGraph.cpp:58-72`) creates a **virtual texture** via
nvrhi and asks the device for requirements. It is called:

* once per resource in `SubAllocateResource` (`:1134`),
* once per resource in `AllocateResourcesInternal` (`:1259`),
* once per **alias candidate** in the aliasing scan (`:1314`),
* plus from the UI tooltip and `ExportToString`.

For ~40 transients that is dozens of resource creations/queries per frame. The requirements are a pure
function of the descriptor hash we already compute. **Fix:** cache `nvrhi::MemoryRequirements` in the
resource (or a desc-hash → requirements map) and invalidate on hash change. This is the cheapest real
CPU win available.

### 4.8 Aliasing allocator quality (see §3.5)

* It cannot alias into free space inside a block, only into a whole owner block.
* It is first-fit by start order, not longest-lifetime-first.
* It ignores the alias barrier cost (one full drain per alias, §4.2).
* `m_PhysicalLastPass` is a pass index and assumes schedule order == GPU order. That is true today
  because submission follows `AcquireCommandList` order; if reordering is ever added, the allocator
  must operate on *scheduled* order. Worth an explicit invariant comment.

### 4.9 Export/tooling nits

* `ExportToString` iterates `std::unordered_set` (`RenderGraphUI.cpp:500-509`), so the ordering of
  `[R]/[W]` entries is implementation-defined and varies run to run — bad for capture diffs.
  Iterate resource indices in sorted order.
* `Stats::m_Total*Memory` sums **owner resource sizes**, not heap bytes, so it under-states the real
  footprint (pow2 heaps, alignment, fragmentation) and cannot show aliasing savings (there is no
  "without aliasing" baseline in the stats). Add `m_HeapBytes`, `m_UnAliasedBytes` and
  `m_AliasingSavingsBytes`.
* No barrier/alias-barrier counters, no per-pass barrier list, no peak/resident heap bytes.
* `Shutdown()` requires the caller to have called `waitForIdle()` first (`RenderGraph.cpp:100-121`);
  it warns and defends, but a graph that owns GPU memory should own that drain.

---

## 5. Ranked plan

| # | Change | Value | Effort | Depends on |
|---|---|---|---|---|
| P0 | Cache memory requirements by desc hash (kill per-frame virtual creations) | CPU: real, easy to measure | S | — |
| P0 | Gate aliasing on `Feature::EnhancedBarriers` (+ legacy aliasing barrier in nvrhi) | **Correctness** | S | — |
| P0 | Fix `m_PassNames.at()` off-by-one; deterministic export ordering | Correctness/tooling | XS | — |
| P0 | Stats: heap bytes, unaliased baseline, alias count, alias barrier count | Visibility for P1 | S | — |
| P1 | Alias allocator: lifetime-length descending + 2D (time × offset) lowest-fit, allow aliasing into free gaps | Memory: article reports ~50 % of transient memory | M | P0 stats |
| P1 | Correct mislabeled accesses (Bloom/TAA…), then add a debug "bindings vs declarations" checker | Unblocks P2+ | M | — |
| P2 | Typed access kinds in the API (`ColorAttachmentWrite`, `DepthWrite`, `DepthReadOnly`, `ShaderRead`, `UnorderedAccess`, `IndirectArgument`, `CopySrc/Dst`, `VertexIndexRead`) | Prerequisite | M | P1 |
| P2 | Build timelines, read groups, batch boundaries; emit graph-owned barriers via `setEnableAutomaticBarriers(false)` + `set*State` | GPU: removes round-trips | L | P1, P2 |
| P3 | ASAP pass scheduling (reorder command-list submission) + kind sorting | GPU: fewer drains, fewer subchannel switches | L | P2 |
| P3 | Wiggle passes for aliasing | Memory: article's second-order win | M | P3 |
| P4 | Static compiled plan cached by frame-graph hash | CPU: 0.1–1 ms | L | P2 |
| P5 | Frame pipelining (remove pre-submit `waitForIdle`) | Throughput: largest single win, but architectural | L | separate |
| — | Split barriers, automatic async compute | Do not do (article's own conclusion) | — | — |

Suggested first PR: **P0 only** (four independent, low-risk items), plus the exact numbers before/after in
the graph UI so the next steps can be justified with data rather than the article's numbers.

---

## 6. Tools / visualization / debug helpers to add

We already have more than the article describes (lifetime bar chart, heap block table, filters,
"Copy to Clipboard", per-pass CPU/GPU timers). Missing, in order of usefulness:

1. **Declared-vs-actual access checker** (debug builds): after each pass records, compare the
   resources actually bound/used against `PassAccess`; log the offending pass and resource. This is
   the single tool that would have caught the Bloom/TAA mislabel.
2. **Barrier accounting:** count transitions, UAV barriers and global sync barriers per pass; show in
   the per-pass table and export. Without it, P2's benefit is unmeasurable outside PIX.
3. **Aliasing report:** "sum of resource sizes" vs "heap bytes" vs "heap bytes without aliasing",
   per type; plus largest free block and fragmentation histogram. Turns the current single
   `Aliased: N` counter into an actionable number.
4. **Graphviz/DOT + JSON export** alongside the Markdown dump: pass → resource edges, access markers,
   alias chains, heap map. Deterministic ordering (§4.9).
5. **Access timeline per resource** (R/W markers per pass) instead of only the lifetime bar; this is
   the article's timeline, and it is also the data structure P2 needs, so showing it validates the
   implementation.
6. **Batch / critical-path overlay** for the lifetime view, once P3 lands (color passes by batch,
   highlight the longest chain).
7. **Pass kind tags** and, if available, a subchannel-switch count (NVIDIA has counters in Nsight; we
   can at least show the switch count the scheduler *would* cause).
8. **Validation toggles in the UI**: verbose logging already exists (`SetVerboseLogging`), aliasing is
   CLI-only (`--disable-rendergraph-aliasing`, `Config.cpp:42-44`); expose both in the Render Graph
   panel.

---

## 7. What we should *not* copy

* **Resource versioning** (`A₀/A₁` names): the article argues against it; our index handles + linear
  recording already match its preferred model.
* **Per-subresource tracking**: matches the article's recommendation to avoid.
* **Split barriers**: article says drivers turn them into normal barriers at the wait.
* **Automatic async compute**: article recommends manual, per-case tuning only.
* **Explicit image layouts everywhere**: with enhanced barriers, `COMMON` + precise sync/access is
  already what nvrhi emits; adding Vulkan-style optimal layouts is not worth it on modern hardware.

---

## 8. Appendix — article concept → our symbol

| Article | Ours |
|---|---|
| `graph.add_image` / `add_buffer` | `RenderGraph::DeclareTexture/Buffer`, `DeclarePersistent*` |
| `.read(x)` / `.write(x)` | `ReadTexture/ReadBuffer`, `WriteTexture/WriteBuffer` |
| `Pass::Compute("Cull").executes(fn)` | `IRenderer::Setup` + `IRenderer::Render`, `REGISTER_RENDERER` |
| resource timeline | `PassAccess` sets + `ResourceLifetime{m_FirstPass,m_LastPass}` (needs full access list) |
| access group (read group) | — (nvrhi merges only within one command list) |
| batch | one command list per pass; submission is one `ExecuteCommandLists` |
| ASAP scheduling | — |
| aliasing (rect packing) | `AllocateResourcesInternal` + `SubAllocateResource` + `FreeBlock` |
| alias chain | `m_AliasedFromIndex`, `m_IsPhysicalOwner`, `m_PhysicalLastPass` |
| transient vs external resource | `Transient*` vs `GetTextureRaw`/scene-owned nvrhi handles |
| dynamic resources / static graph | `DeclarePersistent*` + hash-based pool reuse (partial) |
| `PassInterface` resolving IDs | `RenderGraph::GetTexture/GetBuffer` + `m_IsCompiled` guarantee |
| access validation | `GetTexture/GetBuffer` declaration checks, `Compile()` scans |
| allocator/heap | `m_Heaps`, `HeapEntry::m_Blocks`, `HeapBlock` |

---

### Sources

* Article text extract: `REFERENCES/_rg_article.txt` (generated by `REFERENCES/_extract_rg_article.py`).
* Numbers quoted: ~350 MB (~20 % VRAM, ~50 % transient) and 0.2–0.4 ms/frame on ~80-pass frames
  (article, Timberdoodle).
* Our pass counts: 13 renderers in the default branch (+ 2 debug renderers + ImGui, usually
  disabled), 5 in the debug view, 3 in the reference path tracer — `Renderer.cpp:1543-1583`.
