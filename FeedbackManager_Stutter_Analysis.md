# FeedbackManager::BeginFrame — Initial Load Stutter Analysis

## Symptom

Loading the Bistro map (~3000+ textures) causes several seconds of heavy CPU stutter
that stabilises once the scene settles.  The profiler pins the cost to
`FeedbackManager::BeginFrame`.  The readback batch is capped at
`kFeedbackTexturesToResolvePerFrame = 30` textures per frame, so the question is:
**why is BeginFrame expensive when it is only reading back 30 textures?**

---

## Root Cause Summary

There are **three independent O(N) loops** inside `BeginFrame` that iterate over
**all ~3000 textures every single frame**, regardless of the readback budget.
During the first few seconds the TTM state is also maximally "dirty" (every texture
has freshly-requested tiles), which multiplies the per-texture cost of each loop.

---

## Detailed Breakdown

### 1. Step 1b — "Re-submit cached feedback" loop  *(partial guard, still O(N))*

```cpp
// FeedbackManager.cpp – Step 1b
for (uint32_t texIdx = 0; texIdx < (uint32_t)m_Textures.size(); ++texIdx)
{
    if (readbackSet.count(texIdx)) continue;
    FeedbackTexture* tex = GetTextureByIndex(texIdx);
    if (tex->m_CachedFeedbackData.empty()) continue;          // guard A
    if (!tex->HasAllocatedStandardTiles()) continue;          // guard B

    m_TiledTextureManager->UpdateWithSamplerFeedback(...);
}
```

**Why it is still expensive at startup:**

- **Guard A** (`m_CachedFeedbackData.empty()`) only skips textures that have
  *never* been read back.  With 3000 textures and 30 read-backs per frame, the
  first full ringbuffer cycle takes **100 frames (~1.7 s at 60 fps)**.  During
  that window every texture that has already been read back once will pass Guard A
  and enter `UpdateWithSamplerFeedback`.

- **Guard B** (`HasAllocatedStandardTiles()`) was added to skip textures still at
  packed-mip-only residency.  However, during the initial burst the GPU is
  streaming in standard tiles as fast as possible.  `UpdateTileMappings` (called
  the frame *after* tiles are flushed) increments `m_AllocatedStandardTileCount`,
  so textures graduate past Guard B progressively.  Within a few frames a large
  fraction of the 3000 textures have at least one standard tile mapped and Guard B
  stops helping.

- `UpdateWithSamplerFeedback` inside TTM does an **O(regularTilesNum) scan** over
  every standard tile of the texture to refresh `lastRequestedTime`.  With 3000
  textures × potentially hundreds of tiles each, this is the dominant cost.

**Net effect:** during the first ~100 frames, Step 1b calls
`UpdateWithSamplerFeedback` for an ever-growing fraction of the 3000 textures —
starting at 0 and ramping up to ~3000 — even though only 30 textures are actually
being read back per frame.

---

### 2. Step 6 — "Collect tiles to unmap/map" loop  *(unconditional O(N))*

```cpp
// FeedbackManager.cpp – Step 6
for (uint32_t texIdx = 0; texIdx < (uint32_t)m_Textures.size(); ++texIdx)
{
    FeedbackTexture* feedbackTexture = m_Textures.at(texIdx).get();

    m_TiledTextureManager->GetTilesToUnmap(feedbackTexture->GetTiledTextureId(), tilesToUnmap);
    // ... process unmaps ...

    m_TiledTextureManager->GetTilesToMap(feedbackTexture->GetTiledTextureId(), tilesRequestedNew);
    // ... process maps ...
}
```

This loop has **no guard at all** — it calls `GetTilesToUnmap` and `GetTilesToMap`
for every one of the 3000+ textures on every frame.

- During the initial burst, TTM has freshly-requested tiles for a large number of
  textures simultaneously (the camera sees the whole scene for the first time).
  `GetTilesToMap` returns non-empty results for many textures, so the inner
  `updateTextureTileMappings` D3D12 calls pile up.
- Even for textures with no pending tiles, the TTM calls themselves are not free —
  they touch internal TTM state (tile arrays, allocation lists) for every texture.
- The `std::vector<uint32_t> tilesRequestedNew` and `tilesToUnmap` are declared
  **outside** the loop and reused, but `GetTilesToMap`/`GetTilesToUnmap` still
  clear and repopulate them on every iteration.

**Net effect:** 3000 × (GetTilesToUnmap + GetTilesToMap) calls per frame, with
many of them producing real work during the initial burst.

---

### 3. Step 7 — `DefragmentTiles(16)` after a maximally-fragmented initial state

```cpp
m_TiledTextureManager->DefragmentTiles(16);
```

`DefragmentTiles` is called unconditionally every frame with a budget of 16 moves.
During the initial burst the heap is being filled from scratch, so the tile
allocation is maximally fragmented.  While the budget caps the *moves*, the
function still has to **scan the heap** to find candidates, and that scan is
proportional to the number of allocated tiles — which grows rapidly during the
first seconds.

---

### 4. Step 4 — Heap allocation loop during the initial burst

```cpp
while (m_NumTTMHeaps < numRequiredHeaps)
{
    uint32_t heapId = m_HeapAllocator->AllocateHeap();
    m_TiledTextureManager->AddHeap(heapId);
    m_NumTTMHeaps++;
}
```

`AllocateHeap` calls `device->createHeap` + `device->createBuffer` +
`device->bindBufferMemory` — all synchronous D3D12 calls.  During the first few
frames TTM's `GetNumDesiredHeaps()` jumps from 0 to a large number as feedback
floods in, causing many heaps to be allocated in a single `BeginFrame` call.
Each heap is 256 tiles × 64 KB = **16 MB**, so allocating e.g. 20 heaps in one
frame is 320 MB of D3D12 heap creation in a single CPU frame.

---

### 5. The `readbackSet` construction cost (minor, but present)

```cpp
std::unordered_set<uint32_t> readbackSet(readbackTextures.begin(), readbackTextures.end());
```

This is constructed every frame from the 30-element readback list.  The cost is
negligible in isolation, but it is worth noting that the `readbackSet.count(texIdx)`
call inside the 3000-iteration Step 1b loop adds 3000 hash lookups per frame.

---

## Why "only 30 readbacks per frame" Does Not Bound the Cost

The 30-texture readback budget controls only **Step 1** (map buffer + decode
feedback + UpdateWithSamplerFeedback for 30 textures) and **Step 2** (clear 30
sampler feedback textures + schedule their resolve).

It does **not** bound:
- Step 1b: re-submits cached feedback for up to **all 3000** textures.
- Step 6: queries TTM for tile changes for **all 3000** textures.
- Step 7: defragments across **all allocated tiles**.
- Step 4: allocates as many heaps as TTM demands in a single frame.

The readback budget is purely a GPU-side bandwidth/latency knob.  The CPU-side
loops are O(total textures) or O(total tiles), not O(readback batch size).

---

## Proposed Fixes

### Fix 1 — Amortise Step 6 (highest impact)

Instead of iterating all textures every frame, maintain a **dirty set** of texture
indices that TTM has actually modified (i.e. textures for which
`GetTilesToMap`/`GetTilesToUnmap` would return non-empty results).  TTM should
expose a "dirty texture" list or a per-texture dirty flag; if it does not, maintain
one manually by only adding a texture to the dirty set when:
- Its feedback was just processed in Step 1 or Step 1b, **and** TTM's internal
  state changed (detectable via a return value or a pre/post tile-count comparison).
- A tile mapping was just updated for it in `UpdateTileMappings`.

```cpp
// Proposed Step 6 — only iterate textures TTM has dirtied
for (uint32_t texIdx : m_TilesDirtyTextures)   // small set, not 3000
{
    ...GetTilesToUnmap / GetTilesToMap...
}
m_TilesDirtyTextures.clear();
```

### Fix 2 — Cap Step 1b re-submissions per frame

The re-submission loop exists to keep `lastRequestedTime` fresh so tiles do not
time out between ringbuffer cycles.  With `kTileHysteresisSeconds = 1.0 s` and
3000 textures / 30 per frame = 100-frame cycle (~1.7 s), the hysteresis is already
shorter than the cycle.  Two options:

**Option A — Increase hysteresis to cover the full cycle:**
```cpp
// kTileHysteresisSeconds must be > (numTextures / kFeedbackTexturesToResolvePerFrame) / fps
// e.g. 3000 / 30 / 60 ≈ 1.67 s → set to 2.0 s
static constexpr float kTileHysteresisSeconds = 2.0f;
```
This eliminates the need for Step 1b entirely — tiles will not time out before
their texture is read back again.  The re-submission loop can be removed.

**Option B — Amortise Step 1b across frames (if hysteresis must stay short):**
Process only a fixed budget of re-submissions per frame (e.g. 100), using its own
ring-buffer cursor, so the cost is bounded regardless of texture count.

### Fix 3 — Spread heap allocation over multiple frames

Instead of allocating all required heaps in one `BeginFrame`, allocate a fixed
maximum per frame (e.g. 4 heaps = 64 MB per frame):

```cpp
const uint32_t maxHeapsPerFrame = 4;
uint32_t heapsAllocatedThisFrame = 0;
while (m_NumTTMHeaps < numRequiredHeaps && heapsAllocatedThisFrame < maxHeapsPerFrame)
{
    uint32_t heapId = m_HeapAllocator->AllocateHeap();
    m_TiledTextureManager->AddHeap(heapId);
    m_NumTTMHeaps++;
    heapsAllocatedThisFrame++;
}
```

This spreads the D3D12 heap creation cost over several frames at the cost of
slightly delayed tile availability.

### Fix 4 — Pre-allocate heaps at scene load

If the expected tile working set is known at load time (e.g. from a pre-computed
tile budget), pre-allocate the required heaps during scene loading (off the hot
path) so `BeginFrame` never needs to allocate more than 1–2 heaps per frame during
normal operation.

### Fix 5 — Defer Step 1b until after the first full ringbuffer cycle

During the first `ceil(numTextures / kFeedbackTexturesToResolvePerFrame)` frames,
every texture is being read back for the first time.  Re-submitting cached feedback
for textures that have already been read back once (but not yet a second time) is
premature — their tiles are brand new and cannot have timed out yet.  A simple
frame counter guard eliminates the entire Step 1b cost during the initial burst:

```cpp
const uint32_t ringbufferCycleFrames = 
    ((uint32_t)m_Textures.size() + kFeedbackTexturesToResolvePerFrame - 1)
    / kFeedbackTexturesToResolvePerFrame;

if (m_FramesSinceLoad >= ringbufferCycleFrames)
{
    // Step 1b re-submission loop
    ...
}
```

---

## Summary Table

| Step | Loop size | Cost during burst | Fix |
|------|-----------|-------------------|-----|
| Step 1b re-submit | O(all textures) | High — grows as textures get first readback | Fix 2 / Fix 5 |
| Step 6 unmap/map | O(all textures) | High — many textures have pending tiles | Fix 1 |
| Step 4 heap alloc | O(new heaps) | High — many heaps allocated at once | Fix 3 / Fix 4 |
| Step 7 defrag | O(allocated tiles) | Medium — heap is maximally fragmented | Defer defrag for first N frames |
| Step 1 readback | O(30 textures) | Low — correctly bounded | ✓ already fine |
| Step 2 collect | O(30 textures) | Low — correctly bounded | ✓ already fine |
