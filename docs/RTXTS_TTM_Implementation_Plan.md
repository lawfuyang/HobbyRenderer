# RTXTS-TTM Implementation Plan for HobbyRenderer

> **Based on:** [RTXTS_TTM_Integration_Analysis.md](RTXTS_TTM_Integration_Analysis.md)  
> **Reference sample:** `REFERENCES/RTXTS/`  
> **Target version:** RTXTS-TTM v0.7.1-release (`external/RTXTS-TTM/`)

---

## Overview

This plan translates the integration analysis into concrete implementation steps, mapped to existing HobbyRenderer source files and the RTXTS reference sample. Each step includes the files to create/modify, the reference code to follow, and acceptance criteria.

---

## Architecture Summary

```
┌──────────────────────────────────────────────────────────────────┐
│                        Renderer::RenderFrame()                    │
│  ┌──────────┐  ┌─────────────┐  ┌──────────┐  ┌───────────────┐ │
│  │BeginFrame│→│TileUpload    │→│UpdateMap  │→│    Render     │ │
│  │readback  │  │(CPU staging)│  │(GPU tile  │  │(GBuffer/      │ │
│  │feedback  │  │             │  │ mapping)  │  │ Lighting)     │ │
│  └──────────┘  └─────────────┘  └──────────┘  └───────┬───────┘ │
│                                                        │         │
│                              ┌──────────────┐          │         │
│                              │  EndFrame    │←─┐       │         │
│                              │(ring rotate) │  │       │         │
│                              └──────────────┘  │       │         │
│                                                │       │         │
│                              ┌──────────────┐  │       │         │
│                              │ResolveFeedback│─┘       │         │
│                              │ (decode SF)  │←─────────┘         │
│                              └──────────────┘                    │
└──────────────────────────────────────────────────────────────────┘
```

---

## Phase 0 — CMake Build Integration (0.5 day)

### Step 0.1 — Verify RTXTS-TTM library builds

**Status:** CMake already configured in `CMakeLists.txt` lines 165-172, 206, 298.

```cmake
# CMakeLists.txt — already present, verify only:
set(RTXTSTTM_VERSION "0.7.1-release")
set(RTXTSTTM_URL "...")
download_library("RTXTS-TTM" "${RTXTSTTM_VERSION}" "${RTXTSTTM_URL}" "RTXTS-TTM-*")
# ...
add_subdirectory(${RTXTSTTM_SRC_DIR})                                        # line 206
target_include_directories(${PROJECT_NAME} PRIVATE "${RTXTSTTM_INCLUDE_DIR}") # line 298
```

**Action:** Run a clean CMake configure to confirm `rtxts-ttm` library compiles and links.

**Acceptance:** `rtxts-ttm::rtxts-ttm` target available. `#include <rtxts-ttm/TiledTextureManager.h>` compiles.

### Step 0.2 — Add `nvfeedback` source files to CMake

**New files to register in `CMakeLists.txt`:**

```
src/Streaming/
├── FeedbackManager.cpp
├── FeedbackManager.h
├── FeedbackTexture.cpp
├── FeedbackTexture.h
├── FeedbackTextureSet.cpp
├── FeedbackTextureSet.h
├── HeapAllocator.cpp
├── HeapAllocator.h
├── TileUploadHelper.cpp
├── TileUploadHelper.h
├── AsyncTileIO.cpp         (Phase 1 stub, sync path only)
└── AsyncTileIO.h
```

**Action:** Add `src/Streaming/` sources to the `add_executable` target in `CMakeLists.txt`.

**Acceptance:** `nvfeedback::FeedbackManager` and `nvfeedback::FeedbackTexture` compile as part of the HobbyRenderer binary.

---

## Phase 1 — Resource Setup & TTM Registration (4-6 days)

### Step 1.1 — Port `FeedbackTexture` class

| Source file | Action |
|---|---|
| `src/Streaming/FeedbackTexture.h` | **Create** — based on `REFERENCES/RTXTS/src/feedbackmanager/src/FeedbackTexture.h` |
| `src/Streaming/FeedbackTexture.cpp` | **Create** — based on `REFERENCES/RTXTS/src/feedbackmanager/src/FeedbackTexture.cpp` |

**Key changes from reference:**
- Drop `donut::` dependencies — use existing HobbyRenderer utilities
- Use `nvrhi::Context` as passed from `D3D12RHI.cpp` (donut wraps this; we don't)

**Responsibilities:**
- Create `isTiled=true` reserved texture from `TextureDesc`
- Call `getTextureTiling()` to query tile layout
- Register with TTM via `AddTiledTexture()`
- Create paired `ISamplerFeedbackTexture` (D3D12 only)
- Create readback buffers (ring buffer, `numFramesInFlight` deep)
- Create `R32_FLOAT` MinMip texture (dimensions = tile count)
- Expose `GetReservedTexture()`, `GetSamplerFeedbackTexture()`, `GetMinMipTexture()`, `GetTiledTextureId()`

**Resource sizing:**
- Readback buffer size = `tileCountX × tileCountY` bytes (1 uint8 per feedback texel)
- MinMip texture size = `tileCountX × tileCountY`, format `R32_FLOAT`

**Acceptance:** Create a `FeedbackTexture` from a texture description; destroy it cleanly. Verify `getTextureTiling()` returns sane values for 1024², 2048² BC7 textures.

---

### Step 1.2 — Port `FeedbackTextureSet` class

| Source file | Action |
|---|---|
| `src/Streaming/FeedbackTextureSet.h` | **Create** — based on `REFERENCES/RTXTS/src/feedbackmanager/src/FeedbackTextureSet.h` |
| `src/Streaming/FeedbackTextureSet.cpp` | **Create** — based on `REFERENCES/RTXTS/src/feedbackmanager/src/FeedbackTextureSet.cpp` |

**Responsibilities:**
- Group textures sharing UV coordinates (same material textures)
- Designate one primary texture per set for sampler feedback (first texture added is primary)
- `MatchPrimaryTexture()` call delegated to `FeedbackManager::BeginFrame()`
- Track which textures are "followers" — they mirror the primary's tile state

**Grouping rule (from reference):** One `FeedbackTextureSet` per `Material`. All block-compressed textures belonging to the same material are grouped together. See **Step 1.6** for the post-load assembly pass.

**Acceptance:** Create a `FeedbackTextureSet` with 3 textures (baseColor, normal, roughness); verify primary/index configuration.

---

### Step 1.3 — Port `HeapAllocator` class

| Source file | Action |
|---|---|
| `src/Streaming/HeapAllocator.h` | **Create** — design from analysis §5.4 |
| `src/Streaming/HeapAllocator.cpp` | **Create** — design from analysis §5.4 |

**Responsibilities:**
- `AllocateHeap(heapId)`: Create `nvrhi::Heap` (device-local, `heapSizeInTiles × 64KB`), create virtual buffer bound to heap, register with TTM
- `ReleaseHeap(heapId, frameIndex)`: Defer release by `framesInFlight`, call `ttm->RemoveHeap()`
- `ProcessDeferredReleases(frameIndex)`: Actually release heaps/buffers deferred N frames ago
- `GetNumDesiredHeaps()`: Query TTM demand

**Default:** `heapSizeInTiles = 256` (256 × 64KB = 16MB per heap).

**Acceptance:** Allocate/release a heap; verify virtual buffer `isVirtual=true` and `bindBufferMemory()` succeeds.

---

### Step 1.4 — Port `FeedbackManager` class

| Source file | Action |
|---|---|
| `src/Streaming/FeedbackManager.h` | **Create** — based on `REFERENCES/RTXTS/src/feedbackmanager/include/FeedbackManager.h` |
| `src/Streaming/FeedbackManager.cpp` | **Create** — based on `REFERENCES/RTXTS/src/feedbackmanager/src/FeedbackManager.cpp` |
| `src/Streaming/FeedbackManagerInternal.h` | **Create** — based on `REFERENCES/RTXTS/src/feedbackmanager/src/FeedbackManagerInternal.h` |

**Key differences from reference:**
- Replace `donut::engine::ShaderFactory` with HobbyRenderer's shader loading
- Replace `donut::vfs` with `MemoryMappedDataReader` (already in `Utilities.h`)
- Use `srrhi` (not donut's `nvrhi::app`) for swapchain/windowing

**Public API:**
```cpp
class FeedbackManager {
    bool CreateTexture(const nvrhi::TextureDesc& desc, FeedbackTexture** ppTex);
    bool CreateTextureSet(FeedbackTextureSet** ppTexSet);
    void BeginFrame(nvrhi::ICommandList*, const FeedbackUpdateConfig&, FeedbackTextureCollection*);
    void UpdateTileMappings(nvrhi::ICommandList*, FeedbackTextureCollection*);
    void ResolveFeedback(nvrhi::ICommandList*);
    void EndFrame();
    FeedbackManagerStats GetStats();
};
```

**Internal state:**
- `std::vector<FeedbackTexture*> m_textures` — all registered textures
- `std::vector<FeedbackTexture*> m_readbackQueue` — ring-buffer round-robin
- `uint32_t m_readbackIndex` — current readback position in ring
- `std::vector<std::vector<FeedbackTexture*>> m_texturesToReadback` — per-frame-in-flight, feedback to decode next cycle
- `HeapAllocator m_heapAllocator`
- `rtxts::TiledTextureManager* m_ttm`

**`BeginFrame()` implementation (from reference):**
1. Readback feedback from N frames ago → `mapBuffer(readbackBuf)` → `UpdateWithSamplerFeedback()`
2. For each texture set primary → `MatchPrimaryTexture()` for followers
3. Clear next batch of feedback textures
4. `TrimStandbyTiles()`, `AllocateRequestedTiles()`
5. Get tiles to unmap → call `updateTextureTileMappings(NULL)` immediately
6. Get tiles to map → return in `results` collection

**Acceptance:** `CreateTexture()` for a 2048² BC7 texture; verify TTM registration and resource creation. `BeginFrame()` with empty feedback produces no map/unmap requests.

---

### Step 1.5 — Modify `SceneLoader` to create streaming textures

| Source file | Action |
|---|---|
| `src/SceneLoader.cpp` | **Modify** — add streaming texture creation path |
| `src/SceneLoader.h` | **Modify** — add `FeedbackManager*` member (optional if owned by Renderer) |
| `src/Scene.h` | **Modify** — add `StreamingTexture` struct (per analysis §2.3) |

**Key design note:** SceneLoader is responsible for creating `FeedbackTexture` objects (reserved resources + sampler feedback + TTM registration), but **NOT** for grouping them into `FeedbackTextureSet`s. Texture set assembly is a separate post-load pass (see **Step 1.6**) because it depends on ALL textures being registered first.

**Changes to `Scene.h`:**
```cpp
struct StreamingTexture {
    nvrhi::TextureHandle           reservedTexture;
    nvrhi::SamplerFeedbackTextureHandle feedbackTex;
    std::vector<nvrhi::BufferHandle>    readbackBufs;
    nvrhi::TextureHandle           minMipTexture;
    uint32_t                       ttmTextureId;
    std::shared_ptr<TextureData>   sourceData;      // mmap + DDS metadata
    uint32_t                       bindlessIndex;
};
```

**Changes to `SceneLoader::LoadTexture()`:**
- Detect DDS textures → create `FeedbackTexture` via `FeedbackManager::CreateTexture()` instead of committed `createTexture()`
- Pre-load packed mips to a small committed texture, copy to reserved texture tile region, map permanently
- Store `sourceData` (mmap handle kept open) for later tile extraction
- Keep bindless slot assignment unchanged (reserved resource handle is permanent)
- **Crucially:** only create streaming textures for block-compressed 2D textures (BC1-BC7). Non-BC textures, 3D textures, and arrays skip streaming and use committed textures as before.

**Packed mip pre-mapping:**
```cpp
// After FeedbackTexture creation:
uint32_t packedMipStart = packedMipDesc.numStandardMips;
// Create small committed texture for packed mips only
// Upload packed mip data from DDS
// Copy to reserved texture at packed tile coordinates
// Map packed tiles permanently (never pass to TTM)
```

**Acceptance:** Scene loads with streaming textures (reserved resources), packed mips visible as fallback. Existing rendering still works (no streaming yet, but resources are correct).

---

### Step 1.6 — Post-Load Texture Set Assembly

| Source file | Action |
|---|---|
| `src/Streaming/StreamingContext.h` | **Create** — new struct to hold texture set map and FeedbackManager reference |
| `src/Streaming/StreamingContext.cpp` | **Create** — `BuildTextureSets()` implementation |
| `src/Renderer.cpp` | **Modify** — call `BuildTextureSets()` after scene load and `FeedbackManager` init |
| `src/Renderer.h` | **Modify** — add `StreamingContext` member |

**Why a separate step:** Texture set creation must happen AFTER all `FeedbackTexture` objects are registered (Step 1.5 is complete), and AFTER the `FeedbackManager` is created (which happens in `Renderer::Init` or similar). It cannot live inside `SceneLoader` because the SceneLoader has no knowledge of the `FeedbackManager` or streaming policy.

**`StreamingContext` design:**
```cpp
struct StreamingContext {
    nvfeedback::FeedbackManager* feedbackManager;

    // Map from Scene::Material* → FeedbackTextureSet*
    std::unordered_map<const Scene::Material*,
                       nvfeedback::RefCountPtr<nvfeedback::FeedbackTextureSet>>
        textureSetsByMaterial;

    // Map from nvrhi::TextureHandle → FeedbackTexture* (reverse lookup for set assembly)
    std::unordered_map<nvrhi::TextureHandle, nvfeedback::FeedbackTexture*>
        feedbackTexturesByHandle;
};
```

**`BuildTextureSets()` algorithm (adapted from reference `main.cpp` `EnsureTextureSets()`):**
```cpp
void StreamingContext::BuildTextureSets(const Scene& scene)
{
    textureSetsByMaterial.clear();

    for (size_t matIdx = 0; matIdx < scene.m_Materials.size(); matIdx++)
    {
        const auto& mat = scene.m_Materials[matIdx];

        // ── Check: does this material have a primary (baseColor) texture? ──
        int baseColorIdx = mat.m_BaseColorTexture;
        if (baseColorIdx == -1)
            continue;

        const auto& baseTex = scene.m_Textures[baseColorIdx];

        // ── Check: is this texture registered as a FeedbackTexture? ──
        auto it = feedbackTexturesByHandle.find(baseTex.m_Handle);
        if (it == feedbackTexturesByHandle.end())
            continue;  // not block-compressed, or Vulkan fallback → skipped

        nvfeedback::FeedbackTexture* primaryFt = it->second;

        // ── Create the set ──
        nvfeedback::FeedbackTextureSet* texSet;
        feedbackManager->CreateTextureSet(&texSet);  // NOTE: baseColor auto-selected as primary

        // ── Helper: add texture if it's a registered FeedbackTexture ──
        auto tryAdd = [&](int texIdx) {
            if (texIdx == -1) return;
            auto it = feedbackTexturesByHandle.find(scene.m_Textures[texIdx].m_Handle);
            if (it != feedbackTexturesByHandle.end())
                texSet->AddTexture(it->second);
        };

        // Order matters: first texture added = primary
        tryAdd(mat.m_BaseColorTexture);           // PRIMARY
        tryAdd(mat.m_NormalTexture);              // follower
        tryAdd(mat.m_MetallicRoughnessTexture);   // follower
        tryAdd(mat.m_EmissiveTexture);            // follower

        // ── Validate: reject if follower exceeds primary ──
        uint32_t numTex = texSet->GetNumTextures();
        if (numTex <= 1) continue;  // skip sets with no followers

        auto* primaryReserved = primaryFt->GetReservedTexture();
        uint32_t pw = primaryReserved->getDesc().width;
        uint32_t ph = primaryReserved->getDesc().height;
        uint32_t pm = primaryReserved->getDesc().mipLevels;

        bool valid = true;
        for (uint32_t i = 1; i < numTex; i++)
        {
            auto* ft = texSet->GetTexture(i)->GetReservedTexture();
            if (ft->getDesc().width > pw || ft->getDesc().height > ph ||
                ft->getDesc().mipLevels > pm)
            {
                valid = false;
                break;
            }
        }

        if (!valid)
        {
            texSet->Release();  // destructor cleans up per-texture set membership
            continue;
        }

        textureSetsByMaterial[&mat] = texSet;
    }
}
```

**Integration into `Renderer`:**
```cpp
// In Renderer::Init() or equivalent scene-load callback:
m_feedbackManager.reset(nvfeedback::CreateFeedbackManager(device, fmDesc));
m_streamingCtx.feedbackManager = m_feedbackManager.get();

// After SceneLoader completes:
m_streamingCtx.BuildTextureSets(*m_scene);
```

**The `feedbackTexturesByHandle` map:**
- Populated in Step 1.5 during `FeedbackManager::CreateTexture()` — each created `FeedbackTexture*` is added to this map
- Used for O(1) lookup from the `Scene::Texture::m_Handle` to the `FeedbackTexture*`
- Cleared on scene unload

**Acceptance criteria:**
- `BuildTextureSets()` called after scene load; texture sets created for all materials with ≥2 block-compressed textures
- Materials with only committed textures (non-BC) are silently skipped
- Materials whose normal/metallic-roughness textures are larger than baseColor are rejected (no crash)
- `textureSetsByMaterial` map populated; verified via debug log output counting sets

### Step 2.1 — Create `Streaming.hlsli` HLSL helper

| Source file | Action |
|---|---|
| `src/shaders/Streaming.hlsli` | **Create** — based on analysis §9.1 and reference `shaders/gbufferfeedback_ps.hlsl` |

**Contents:**
```hlsl
// Sampler feedback + MinMip fallback sampling
// Requires: [earlydepthstencil] on entry point when using WriteSamplerFeedback

float4 SampleStreamedTexture(
    Texture2D tex, SamplerState samp, SamplerState pointSamp,
    Texture2D<float> minMipTex, float2 uv,
    inout SamplerFeedbackTexture feedbackTex, float feedbackLOD)
{
    float lod = tex.CalculateLevelOfDetail(samp, uv);
    float status;
    float4 color = tex.Sample(samp, uv, 0, status);

    if (!CheckAccessFullyMapped(status)) {
        uint2 fbCoord = uint2(uv * float2(feedbackWidth, feedbackHeight));
        float minResidentMip = minMipTex.SampleLevel(pointSamp, uv, 0);
        for (int mip = 1; mip <= (int)minResidentMip; mip++) {
            color = tex.SampleLevel(samp, uv, mip, status);
            if (CheckAccessFullyMapped(status)) break;
        }
    }

    color.WriteSamplerFeedback(feedbackTex, samp, uv, feedbackLOD);
    return color;
}
```

**Acceptance:** HLSL compiles (via ShaderMake in existing pipeline).

---

### Step 2.2 — Add feedback UAV + MinMip SRV to material bindings

| Source file | Action |
|---|---|
| `src/shaders/Common.sr` | **Modify** — add feedback/minMip register slots |
| `src/shaders/BasePass.hlsl` | **Modify** — use `SampleStreamedTexture()` for albedo/normal/roughness fetches |
| `src/CommonResources.cpp` | **Modify** — create binding layout with feedback slot |
| `src/BasePassRenderer.cpp` | **Modify** — populate per-material bindings with feedback texture handles |

**Register assignment (example):**
```hlsl
// Common.sr — new slots
SamplerFeedbackTexture<float> g_FeedbackTexture : register(u5);
Texture2D<float> g_MinMipTexture : register(t10);
SamplerState g_PointSampler : register(s2);
```

**Key requirement:** All pixel shaders that use `WriteSamplerFeedback()` **must** have `[earlydepthstencil]`.

**Acceptance:** BasePass compiles and renders correctly with the new binding layout. If feedback texture is NULL (Vulkan path), `SampleStreamedTexture` must degrade to normal `Sample()` (use `#ifdef`).

---

### Step 2.3 — Integrate feedback per-frame loop into `Renderer`

| Source file | Action |
|---|---|
| `src/Renderer.cpp` | **Modify** — add `FeedbackManager` member and per-frame calls |
| `src/Renderer.h` | **Modify** — add `FeedbackManager*` and streaming config |

**Per-frame flow in `Renderer::RenderFrame()`:**

```cpp
// After scene is loaded, after commandList creation, BEFORE GBuffer pass:

// ── Phase A: BeginFrame ──
{
    nvrhi::FeedbackTextureCollection updatedTextures;
    nvfeedback::FeedbackUpdateConfig config;
    config.frameIndex = m_frameIndex;
    config.maxTexturesToUpdate = m_streamingCfg.maxTexturesPerFrame; // default: 8
    config.tileTimeoutSeconds = m_streamingCfg.tileTimeout;          // default: 0.0f
    config.defragmentHeaps = true;
    config.trimStandbyTiles = true;
    config.releaseEmptyHeaps = true;
    config.numExtraStandbyTiles = 0;

    m_commandList->open();
    m_feedbackManager->BeginFrame(m_commandList, config, &updatedTextures);
    m_commandList->close();
    m_device->executeCommandList(m_commandList);
    m_device->waitForIdle(); // or fence-sync for D3D12 readback consistency
    // updatedTextures now contains tiles needing data upload
}

// ── Phase B: Tile Data Upload (synchronous stub in Phase 2) ──
std::vector<TileUploadRequest> requests;
for (auto& texUpdate : updatedTextures.textures) {
    for (auto tileIndex : texUpdate.tileIndices) {
        requests.push_back({texUpdate.texture, tileIndex});
    }
}
// Upload budget
uint32_t uploadCount = std::min(m_streamingCfg.tilesPerFrame, (uint32_t)requests.size());
for (uint32_t i = 0; i < uploadCount; i++) {
    UploadTileDataSync(requests[i]); // stub — Phase 3 implements
}

// ── Phase C: UpdateTileMappings ──
{
    m_commandList->open();
    m_feedbackManager->UpdateTileMappings(m_commandList, &updatedTextures);
    m_commandList->close();
    m_device->executeCommandList(m_commandList);
}

// ── Phase D: Render (existing) ──
RenderGBuffer();  // shaders write sampler feedback
RenderLighting();

// ── Phase E: ResolveFeedback ──
{
    m_commandList->open();
    m_feedbackManager->ResolveFeedback(m_commandList);
    m_commandList->close();
    m_device->executeCommandList(m_commandList);
}

// ── Phase F: EndFrame ──
m_feedbackManager->EndFrame();

m_frameIndex++;
```

**Acceptance:** Per-frame loop compiles and runs. Phase B is a no-op stub in Phase 2. No crash, no validation errors. `FeedbackManagerStats` printed to log shows zero tile activity.

---

## Phase 3 — Tile Upload (4-6 days)

### Step 3.1 — Implement `TileUploadHelper` (DDS tile extraction)

| Source file | Action |
|---|---|
| `src/Streaming/TileUploadHelper.h` | **Create** — row-by-row DDS tile extraction utility |
| `src/Streaming/TileUploadHelper.cpp` | **Create** — per analysis §7.1-7.2 |

**`ExtractTileFromLinearDDS()` algorithm:**
```cpp
// Input: MemoryMappedDataReader for the DDS file, mip level, tile (x,y) in texels
// Output: linear tile data in staging buffer (GPU-tiled conversion done by CopyTextureRegion)

struct TileExtractParams {
    uint32_t mipLevel;
    uint32_t tileXInTexels, tileYInTexels;
    uint32_t tileWidthInTexels, tileHeightInTexels;
    uint32_t sourceWidth, sourceHeight;  // mip dimensions
    DXGI_FORMAT format;                  // e.g., DXGI_FORMAT_BC7_UNORM
    uint32_t bytesPerBlock;              // 16 for BC7
};

void ExtractTileFromLinearDDS(
    const void* sourceMipBase,     // pointer to start of this mip in mmap
    const TileExtractParams& params,
    void* stagingBuffer)           // output, must be tileWidth*tileHeight bytes (linear layout)
{
    uint32_t blocksPerRow = (params.sourceWidth + 3) / 4;
    uint32_t tileBlocksWidth  = params.tileWidthInTexels / 4;
    uint32_t tileBlocksHeight = params.tileHeightInTexels / 4;
    uint32_t sourceBlockX = params.tileXInTexels / 4;
    uint32_t sourceBlockY = params.tileYInTexels / 4;
    uint32_t rowPitchSource = blocksPerRow * params.bytesPerBlock;
    uint32_t rowPitchTile = tileBlocksWidth * params.bytesPerBlock;

    const uint8_t* src = static_cast<const uint8_t*>(sourceMipBase);
    uint8_t* dst = static_cast<uint8_t*>(stagingBuffer);

    for (uint32_t row = 0; row < tileBlocksHeight; row++) {
        uint32_t readOffset = (sourceBlockY + row) * rowPitchSource + sourceBlockX * params.bytesPerBlock;
        memcpy(dst + row * rowPitchTile, src + readOffset, rowPitchTile);
    }
}
```

**Support for non-BC formats** (extension beyond reference):
- BC1: `bytesPerBlock=8`, block size=4×4 texels
- BC3/BC5/BC7: `bytesPerBlock=16`, block size=4×4 texels
- RGBA8: `bytesPerBlock=4`, block size=1×1 texels (no block compression; simpler math)
- R8: `bytesPerBlock=1`, block size=1×1 texels

**Acceptance:** Unit test: extract a known tile from a test DDS → compare against reference bytes.

---

### Step 3.2 — Implement tile upload via NVRHI `writeTexture` sub-region

| Source file | Action |
|---|---|
| `src/Streaming/TileUploadHelper.cpp` | **Modify** — add `UploadTileToReservedTexture()` |

**NVRHI already provides the necessary sub-region upload capability.** The existing `commandList->writeTexture(ITexture*, const TextureSlice&, const void*, size_t, size_t)` method (custom-added with `[rlaw]` annotations) handles:

1. Suballocating from NVRHI's automatic upload buffer
2. CPU-side memcpy of tile data into the upload buffer
3. Resource state transition (`ShaderResource → CopyDest`, via NVRHI automatic barriers)
4. Calling D3D12 `CopyTextureRegion` with the correct x/y/z destination offset from the `TextureSlice`

This means **no raw D3D12 code is needed in the renderer at all.** If any raw API code were necessary, it would be implemented in the NVRHI module — but in this case, the functionality already exists.

**Approach:**
```cpp
void UploadTileToReservedTexture(
    nvrhi::ICommandList* cmd,
    nvrhi::ITexture* reservedTexture,
    const TileExtractParams& params,
    const void* tileData,
    size_t tileDataSize)
{
    // NVRHI's writeTexture with TextureSlice handles:
    // - Upload buffer suballocation
    // - CPU→GPU memcpy
    // - D3D12 CopyTextureRegion with origin = (tileX, tileY)
    // - Automatic resource state tracking (CopyDest)
    nvrhi::TextureSlice destSlice;
    destSlice.x = params.tileXInTexels;
    destSlice.y = params.tileYInTexels;
    destSlice.width = params.tileWidthInTexels;
    destSlice.height = params.tileHeightInTexels;
    destSlice.mipLevel = params.mipLevel;
    destSlice.arraySlice = 0;

    size_t rowPitch = (params.tileWidthInTexels / params.blockSizeX) * params.bytesPerBlock;
    cmd->writeTexture(reservedTexture, destSlice, tileData, rowPitch, /*depthPitch=*/0);
}
```

**Why `TextureSlice` (not raw D3D12):**
- The `TextureSlice` variant was added for exactly this purpose — sub-region texture uploads from CPU memory
- D3D12 backend implementation: [d3d12-texture.cpp lines 1516-1596](d:\Workspace\HobbyRenderer\external\nvrhi\src\d3d12\d3d12-texture.cpp) — suballocates upload buffer, memcpy, then `CopyTextureRegion(&dstLocation, resolvedDstSlice.x, resolvedDstSlice.y, resolvedDstSlice.z, ...)`
- The `TextureSlice::resolve()` method handles dimension computation for compressed formats (block size rounding)
- NVRHI's automatic barriers handle the `ShaderResource → CopyDest` transition; no manual barrier code needed
- Vulkan backend currently has a stub for `writeTexture(TextureSlice)` — the existing D3D12-only streaming restriction means this is not a concern

**`TileExtractParams` — extended fields:**
```cpp
struct TileExtractParams {
    uint32_t mipLevel;
    uint32_t tileXInTexels, tileYInTexels;
    uint32_t tileWidthInTexels, tileHeightInTexels;
    uint32_t sourceWidth, sourceHeight;
    nvrhi::Format format;
    uint32_t bytesPerBlock;     // e.g., 16 for BC7
    uint32_t blockSizeX;        // 4 for BC formats, 1 for uncompressed
    uint32_t blockSizeY;        // 4 for BC formats, 1 for uncompressed
};
```

**Important:** `updateTextureTileMappings()` must be called BEFORE tile upload — TTM returns tiles as "to map", the application uploads tile data, then calls `UpdateTileMappings()` which acknowledges the mapping via `UpdateTilesMapping()`.

**Acceptance:** Upload a single tile to a reserved texture; verify via GPU readback or RenderDoc that the tile data is correct. No D3D12 headers or raw API calls in renderer code.

---

### Step 3.3 — Persistent scratch buffer for DDS tile extraction

| Source file | Action |
|---|---|
| `src/Streaming/TileUploadHelper.h` | **Modify** — add `std::vector<uint8_t> m_scratchBuffer` member |
| `src/Streaming/TileUploadHelper.cpp` | **Modify** — resize once, reuse across all UploadTile calls |

**NVRHI handles GPU upload staging internally** via `writeTexture(TextureSlice)` — upload buffer suballocation, memcpy, and `CopyTextureRegion` are all managed transparently. We do **not** need a GPU-side staging buffer pool.

The only allocation needed is a CPU-side scratch buffer for DDS tile extraction output. A single persistent `std::vector<uint8_t>` living for the entire application lifetime is sufficient:

```cpp
class TileUploadHelper {
    std::vector<uint8_t> m_scratchBuffer;  // resized once, reused forever
    // ...
};

void TileUploadHelper::UploadTileToReservedTexture(...)
{
    // Resize once to accommodate the largest tile
    if (m_scratchBuffer.size() < kMaxTileSize)  // 64KB = standard D3D12 tile size
        m_scratchBuffer.resize(kMaxTileSize);

    ExtractTileFromLinearDDS(sourceMipBase, params, m_scratchBuffer.data());
    cmd->writeTexture(reservedTex, destSlice, m_scratchBuffer.data(), rowPitch);
}
```

**Rationale for persistent buffer (not stack allocation):**
- Avoids 64KB stack frame bloat per tile (multiple tiles per frame × stack = unnecessary pressure)
- Single allocation, zero overhead per upload call
- Matches the pattern: compute → write to NVRHI (data consumed synchronously before next tile)

**Acceptance:** Upload 100 tiles in one frame with a single persistent scratch buffer. No per-tile allocations. NVRHI's internal upload manager handles GPU staging transparently.

---

### Step 3.4 — Wire tile upload into per-frame loop

| Source file | Action |
|---|---|
| `src/Renderer.cpp` | **Modify** — implement Phase B tile upload (replacing stub) |

```cpp
// Phase B — Tile Data Upload
// Open a command list for upload: NVRHI's writeTexture handles all staging internally
m_commandList->open();

nvfeedback::FeedbackTextureCollection tilesThisFrame;
uint32_t uploadBudget = std::min(m_streamingCfg.tilesPerFrame, (uint32_t)requests.size());

for (uint32_t i = 0; i < uploadBudget; i++) {
    auto& req = requests[i];
    auto* feedbackTex = static_cast<FeedbackTexture*>(req.texture);

    // 1. Get tile info from FeedbackTexture
    std::vector<FeedbackTextureTileInfo> tileInfos;
    feedbackTex->GetTileInfo(req.tileIndex, tileInfos);

    // 2. Get DDS source data
    auto& sourceData = feedbackTex->GetSourceData();

    // 3. Extract tile from DDS → persistent scratch buffer, then upload via NVRHI writeTexture
    for (auto& tileInfo : tileInfos) {
        ExtractTileFromLinearDDS(sourceData->GetMipData(tileInfo.mip), tileInfo,
                                 m_tileUploadHelper->GetScratchBuffer());

        nvrhi::TextureSlice destSlice;
        destSlice.x = tileInfo.tileXInTexels;
        destSlice.y = tileInfo.tileYInTexels;
        destSlice.width = tileInfo.widthInTexels;
        destSlice.height = tileInfo.heightInTexels;
        destSlice.mipLevel = tileInfo.mip;
        destSlice.arraySlice = 0;

        size_t rowPitch = (tileInfo.widthInTexels / tileInfo.blockSizeX) * tileInfo.bytesPerBlock;
        m_commandList->writeTexture(feedbackTex->GetReservedTexture(), destSlice,
                                    m_tileUploadHelper->GetScratchBuffer(), rowPitch);
    }

    tilesThisFrame.textures.push_back({feedbackTex, {req.tileIndex}});
}

m_commandList->close();
m_device->executeCommandList(m_commandList);
// Tile data upload complete — now proceed to UpdateTileMappings
```

**Acceptance:** Streaming textures populate tiles as the camera moves. RenderDoc capture shows `CopyTextureRegion` calls (from within NVRHI) followed by `updateTextureTileMappings()`. Higher-res mip levels appear dynamically.

---

### Step 3.5 — Implement MinMip dirty-texture upload

| Source file | Action |
|---|---|
| `src/Streaming/FeedbackManager.cpp` | **Modify** — inside `UpdateTileMappings()` |

**Per the reference's `FeedbackManager::UpdateTileMappings()`:**
```cpp
void FeedbackManager::UpdateTileMappings(CL* cmd, FeedbackTextureCollection* tilesReady) {
    // 1. For each texture with tiles mapped this frame:
    for (auto& update : tilesReady->textures) {
        auto* tex = static_cast<FeedbackTextureImpl*>(update.texture);

        // 2. TTM: UpdateTilesMapping() — acknowledge mapping done
        ttm->UpdateTilesMapping(tex->GetTiledTextureId(), update.tileIndices);

        // 3. Get tile allocations
        std::vector<rtxts::TileAllocation> allocs;
        ttm->GetTileAllocations(tex->GetTiledTextureId(), allocs);

        // 4. Batch per-heap: updateTextureTileMappings()
        // Group by heap → one NVRHI call per heap
        // ...
    }

    // 5. WriteMinMipData for dirty textures
    for (auto& update : tilesReady->textures) {
        auto* tex = static_cast<FeedbackTextureImpl*>(update.texture);
        std::vector<uint8_t> minMipData;
        // TTM returns per-tile residency as uint8
        ttm->WriteMinMipData(tex->GetTiledTextureId(), minMipData);
        // Upload to R32_FLOAT MinMip texture via writeTexture()
        cmd->writeTexture(tex->GetMinMipTexture(), 0, 0, minMipData.data(), ...);
    }
}
```

**Acceptance:** MinMip texture updates visible in shader. `SampleStreamedTexture()` correctly reads `minResidentMip` and falls back.

---

## Phase 4 — Debug, Polish & Tuning (2-3 days)

### Step 4.1 — ImGui stats panel

| Source file | Action |
|---|---|
| `src/ImGuiLayer.cpp` | **Modify** — add streaming stats window |

**Display using `FeedbackManagerStats`:**
```
Texture Streaming
├── Memory
│   ├── Heap Allocated:   64.0 MB
│   ├── Heap Free Tiles:  512
│   └── Tiles Total:      2048
├── Activity
│   ├── Tiles Allocated:  1024 (50%)
│   ├── Tiles Standby:    256
│   ├── Tiles Loaded/frame: 32
│   └── Textures Updated:  4 / 8
├── Perf
│   ├── BeginFrame:    0.25 ms
│   ├── TileUpload:    0.80 ms
│   ├── UpdateMappings: 0.15 ms
│   └── Resolve:       0.10 ms
└── Control
    ├── Max Textures/Frame: [====8====]
    ├── Tiles Per Frame:    [===32====]
    ├── Timeout (s):        [0.00]
    └── Extra Standby:      [0]
```

**Acceptance:** Stats panel updates every frame, reflects real heap/tile counts.

---

### Step 4.2 — Tile residency visualization overlay

| Source file | Action |
|---|---|
| `src/shaders/DebugTileVis.hlsl` | **Create** — overlay shader reading MinMip |
| `src/Renderer.cpp` | **Modify** — toggleable debug pass |

**Visualization:**
- Green tile = resident (mapped)
- Red tile = not resident (needs streamed → falls back)
- Yellow tile = standby (mapped but evictable)
- Overlay on main render target

**Acceptance:** Toggleable via ImGui checkbox in the stats panel. Camera movement shows red tiles fading to green as streaming catches up.

---

### Step 4.3 — Performance profiling

| Source file | Action |
|---|---|
| `src/Renderer.cpp` | **Modify** — add scoped timers around each stream phase |

**Use existing `microprofile` integration** (already in the project):
```cpp
MICROPROFILE_SCOPEI("Streaming", "BeginFrame", MP_YELLOW);
MICROPROFILE_SCOPEI("Streaming", "TileUpload", MP_ORANGE);
MICROPROFILE_SCOPEI("Streaming", "UpdateMappings", MP_GREEN);
MICROPROFILE_SCOPEI("Streaming", "ResolveFeedback", MP_BLUE);
```

**Acceptance:** Microprofile timeline shows per-phase GPU/CPU times. BeginFrame < 0.5ms, TileUpload < 2ms (worst case), UpdateMappings < 0.3ms, Resolve < 0.3ms.

---

### Step 4.4 — Configurable settings

| Source file | Action |
|---|---|
| `src/Config.h` | **Modify** — add streaming config struct |
| `src/Config.cpp` | **Modify** — add streaming defaults |

```cpp
struct StreamingConfig {
    bool     enabled = true;
    uint32_t maxTexturesPerFrame = 8;
    uint32_t tilesPerFrame = 32;
    float    tileTimeoutSeconds = 0.0f;
    bool     trimStandbyTiles = true;
    bool     releaseEmptyHeaps = true;
    uint32_t numExtraStandbyTiles = 0;
    uint32_t heapSizeInTiles = 256;
    uint32_t numFramesInFlight = 3;
};
```

**Acceptance:** Settings loadable from JSON/config file, adjustable at runtime via ImGui.

---

### Step 4.5 — Vulkan graceful degradation

| Source file | Action |
|---|---|
| `src/Renderer.cpp` | **Modify** — disable streaming when not D3D12 |
| `src/shaders/Streaming.hlsli` | **Modify** — `#ifdef STREAMING_ENABLED` compile-time switch |
| `src/SceneLoader.cpp` | **Modify** — fall back to committed textures on Vulkan |

**Logic:**
```cpp
if (m_renderBackend == RenderBackend::D3D12 && m_streamingCfg.enabled) {
    // Create FeedbackManager, FeedbackTextures...
} else {
    // Fall back to existing committed texture path
}
```

**Caveats for `#ifdef STREAMING_ENABLED`:**
- When `STREAMING_ENABLED` is not defined:
  - `SampleStreamedTexture()` → regular `tex.Sample()`
  - No `WriteSamplerFeedback()` call
  - No `[earlydepthstencil]` requirement
- ShaderMake config needs two permutations

**Acceptance:** Vulkan backend renders correctly with committed textures. D3D12 with `StreamingConfig::enabled=false` uses committed textures.

---

## File Creation/Modification Summary

### New Files

| File | Phase | Lines (est) |
|---|---|---|
| `src/Streaming/FeedbackManager.h` | 1 | ~80 |
| `src/Streaming/FeedbackManager.cpp` | 1 | ~600 |
| `src/Streaming/FeedbackManagerInternal.h` | 1 | ~120 |
| `src/Streaming/FeedbackTexture.h` | 1 | ~80 |
| `src/Streaming/FeedbackTexture.cpp` | 1 | ~300 |
| `src/Streaming/FeedbackTextureSet.h` | 1 | ~50 |
| `src/Streaming/FeedbackTextureSet.cpp` | 1 | ~60 |
| `src/Streaming/HeapAllocator.h` | 1 | ~40 |
| `src/Streaming/HeapAllocator.cpp` | 1 | ~120 |
| `src/Streaming/StreamingContext.h` | 1 | ~60 |
| `src/Streaming/StreamingContext.cpp` | 1 | ~150 |
| `src/Streaming/TileUploadHelper.h` | 3 | ~60 |
| `src/Streaming/TileUploadHelper.cpp` | 3 | ~300 |
| `src/Streaming/AsyncTileIO.h` | 3 | ~40 |
| `src/Streaming/AsyncTileIO.cpp` | 3 | ~80 |
| `src/shaders/Streaming.hlsli` | 2 | ~80 |
| `src/shaders/DebugTileVis.hlsl` | 4 | ~60 |
| `docs/RTXTS_TTM_Implementation_Plan.md` | — | (this file) |

### Modified Files

| File | Phase | Changes |
|---|---|---|
| `CMakeLists.txt` | 0 | Add `src/Streaming/` sources |
| `src/Scene.h` | 1 | Add `StreamingTexture` struct |
| `src/SceneLoader.h` | 1 | Add `FeedbackManager*` member (if needed) |
| `src/SceneLoader.cpp` | 1 | Streaming texture creation + packed mip pre-map |
| `src/Renderer.h` | 1 | Add `FeedbackManager*`, `StreamingContext`, `StreamingConfig` |
| `src/Renderer.cpp` | 1-4 | Per-frame streaming loop, `BuildTextureSets()` call, stats, profiling |
| `src/CommonResources.cpp` | 2 | Binding layout with feedback/MinMip slots |
| `src/BasePassRenderer.cpp` | 2 | Populate per-material feedback bindings |
| `src/shaders/Common.sr` | 2 | Register slots for feedback UAV, MinMip SRV, point sampler |
| `src/shaders/BasePass.hlsl` | 2 | Use `SampleStreamedTexture()` |
| `src/shaders/shaders.cfg` | 2 | Add Streaming.hlsli |
| `src/ImGuiLayer.cpp` | 4 | Streaming stats panel |
| `src/Config.h` | 4 | Add `StreamingConfig` struct |
| `src/Config.cpp` | 4 | Add streaming defaults |
| `src/pch.h` | 1 | Add `#include <rtxts-ttm/TiledTextureManager.h>` |

---

## Dependency Graph

```
Phase 0 (CMake)
  │
  └─► Phase 1 (Resource Setup)
        ├── 1.1 FeedbackTexture ──┐
        ├── 1.2 FeedbackTextureSet─┤
        ├── 1.3 HeapAllocator ─────┤
        └── 1.4 FeedbackManager ◄──┘
              │
              ▼
        1.5 SceneLoader (streaming path)
              │
              ▼
        1.6 StreamingContext::BuildTextureSets()
              │
              ▼
        Phase 2 (Shader + Feedback Loop)
        ├── 2.1 Streaming.hlsli ──┐
        ├── 2.2 Bindings ◄────────┘
        └── 2.3 Renderer loop
              │
              ▼
        Phase 3 (Tile Upload)
        ├── 3.1 Tile extraction
        ├── 3.2 writeTexture(TextureSlice) upload
        ├── 3.3 CPU scratch buffer
        ├── 3.4 Wire into loop
        └── 3.5 MinMip upload
              │
              ▼
        Phase 4 (Debug & Polish)
        ├── 4.1 ImGui stats
        ├── 4.2 Residency viz
        ├── 4.3 Profiling
        ├── 4.4 Config
        └── 4.5 Vulkan fallback
```

---

## Key Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Per-texture tiled resources vs atlas | Per-texture (follow reference) | Simpler initial implementation; atlas can be added later |
| Upload method | NVRHI `writeTexture(TextureSlice)` (uses `CopyTextureRegion` internally) | GPU converts linear→tiled automatically; no raw D3D12 in renderer; NVRHI handles staging & barriers |
| Async I/O | Synchronous stub in Phase 1-3, async in follow-up | Reduces initial complexity; packed mips mask latency |
| D3D12-only streaming | Yes, Vulkan falls back to committed | Sampler feedback is D3D12-only; TTM requires tiled resources (D3D12 sparse) |
| **How texture sets are grouped** | **One `FeedbackTextureSet` per `Scene::Material`** | Materials share UV coordinates → `MatchPrimaryTexture()` is valid; baseColor is always primary |
| **When texture sets are created** | **Post-load pass (Step 1.6), NOT in SceneLoader** | SceneLoader has no knowledge of FeedbackManager; separates I/O from streaming policy |
| **Materials without streaming textures** | **Silently skipped** — no set created | Non-BC textures or Vulkan fallback don't produce FeedbackTextures |
| **Followers larger than primary** | **Reject the entire set** — textures stream independently | `MatchPrimaryTexture()` can't request mips the primary doesn't have |
| FeedbackTextureSet usage | Deferred (Phase 1 creates, Phase 2+ uses) | Not needed until shader bindings are updated |
| Ring-buffer depth | 3 frames (`numFramesInFlight`) | Matches reference; balances latency vs memory |
| Heap size | 256 tiles (16MB default) | Matches reference; configurable via `StreamingConfig` |

---

## Risk Mitigation Checklist

| Risk | Check | When |
|---|---|---|
| `getTextureTiling()` returns zero tiles | Add validation assert after call | Phase 1.1 |
| `createSamplerFeedbackTexture()` fails on non-D3D12 | Graceful fallback to committed texture | Phase 1.1 |
| `updateTextureTileMappings()` called before tile upload | Ensure BeginFrame→Upload→UpdateMappings ordering | Phase 1.4 |
| `[earlydepthstencil]` missing on PS | Shader compilation error → add annotation | Phase 2.2 |
| `CheckAccessFullyMapped()` always true | Verify `isTiled=true` on reserved texture | Phase 2.2 |
| `CopyTextureRegion` to unmapped tile → GPU hang | Ensure `updateTextureTileMappings()` runs first; NVRHI barriers handled automatically | Phase 3.2 |
| DDS mip offset calculation wrong | Unit test with known DDS file | Phase 3.1 |
| Packed mips not providing fallback | Verify packed mip pre-map code | Phase 1.5 |

---

## Estimated Timeline

| Phase | Days | Cumulative |
|---|---|---|
| Phase 0 — CMake Integration | 0.5 | 0.5 |
| Phase 1 — Resource Setup & TTM Registration | 6 | 6.5 |
| Phase 2 — Shader Integration & Feedback Loop | 4 | 10.5 |
| Phase 3 — Tile Upload | 5 | 15.5 |
| Phase 4 — Debug, Polish & Tuning | 3 | 18.5 |
| **Total** | **18.5 days** | |

---

## References

- **Analysis document:** `docs/RTXTS_TTM_Integration_Analysis.md`
- **Reference FeedbackManager:** `REFERENCES/RTXTS/src/feedbackmanager/`
- **Reference shaders:** `REFERENCES/RTXTS/shaders/gbufferfeedback_ps.hlsl`
- **Reference frame loop:** `REFERENCES/RTXTS/src/main.cpp` (search `FeedbackManager`)
- **RTXTS-TTM API:** `external/RTXTS-TTM/include/rtxts-ttm/TiledTextureManager.h`
- **NVRHI tiled resources:** `external/nvrhi/include/nvrhi/nvrhi.h` (`getTextureTiling`, `updateTextureTileMappings`)
- **NVRHI sampler feedback:** D3D12 backend `external/nvrhi/src/d3d12/`
- **Current texture loading:** `src/SceneLoader.cpp`, `src/TextureLoader.cpp`
- **Current render loop:** `src/Renderer.cpp`
- **Current shader config:** `src/shaders/shaders.cfg`
