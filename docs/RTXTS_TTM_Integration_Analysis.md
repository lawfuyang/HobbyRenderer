# RTXTS-TTM Integration Analysis for HobbyRenderer

---

## 1. Reference Sample Overview

The [RTXTS reference sample](https://github.com/NVIDIA-RTX/RTXTS) demonstrates NVIDIA's recommended integration of `rtxts::TiledTextureManager` (TTM) with NVRHI and D3D12 Sampler Feedback.

**Key architecture decisions from the reference:**

- **Per-texture tiled resources** — each source texture is its own D3D12 reserved (tiled) resource. No texture atlas.
- **`nvfeedback::FeedbackManager`** — an NVRHI wrapper (NOT part of TTM) that orchestrates: reserved resource creation, heap allocation, feedback decode, tile mapping, and MinMip uploads.
- **`nvfeedback::FeedbackTexture`** — wraps: reserved texture + sampler feedback texture + readback buffers (ring-buffer, N frames deep) + MinMip texture (R32_FLOAT, per-tile residency).
- **`nvfeedback::FeedbackTextureSet`** — groups textures sharing UV coordinates (same material). Only the primary texture gets sampler feedback; followers mirror via `MatchPrimaryTexture()`.
- **`HeapAllocator`** — creates NVRHI heaps + virtual buffers bound to them (`isVirtual=true` → `bindBufferMemory()`).
- **Tile upload via `CopyTextureRegion`** — upload buffer (linear, CPU-writable) → reserved texture at tile subresource coordinates. GPU converts linear→tiled layout automatically.
- **Ring-buffer round-robin** — each frame, up to `maxTexturesToUpdate` textures get their feedback cleared and resolved. Limits per-frame CPU/GPU feedback bandwidth.

**NVRHI modification:** `ICommandList::writeHeap()` was added to enable direct heap-offset uploads as an alternative to raw D3D12 `CopyTextureRegion`. D3D12 impl, Vulkan/D3D11 stubs.

---

## 2. Current Texture Pipeline (Baseline)

### 2.1 Load Path

```
SceneLoader::LoadTexturesFromImages()
  └─ LoadTexture() → LoadDDSTexture() or LoadSTBITexture()
       └─ MemoryMappedDataReader(filePath)     ← maps entire file
       └─ device->createTexture(desc)          ← full mip chain, committed VRAM
       └─ UploadTexture(cl, ...)               ← uploads ALL mips upfront
       └─ RegisterTexture(handle)              → bindless table slot
```

### 2.2 What Must Change

| Currently | Streaming |
|---|---|
| `isVirtual=false`, `isTiled=false` | `isVirtual=true`, `isTiled=true` (reserved resource) |
| Full mip chain committed at load | Only packed mips committed; standard mips streamed on demand |
| Single `writeTexture()` for all mips | Per-tile upload via `CopyTextureRegion` or `writeHeap()` |
| Permanent bindless slot | Same — reserved resource handle is permanent; only tile mappings change |
| No sampler feedback | Paired `ISamplerFeedbackTexture` + `decodeSamplerFeedbackTexture()` |
| `ResourceStates::ShaderResource` always | Reserved texture transitions to `CopyDest` during tile uploads |

### 2.3 Texture Structure Changes

```cpp
// src/Scene.h — streaming-aware texture type
struct StreamingTexture {
    nvrhi::TextureHandle           reservedTexture;   // isTiled=true, isVirtual=true
    nvrhi::SamplerFeedbackTextureHandle feedbackTex;  // paired with reservedTexture
    std::vector<nvrhi::BufferHandle>    readbackBufs; // N frames deep
    nvrhi::TextureHandle           minMipTexture;     // R32_FLOAT, per-tile residency
    uint32_t                       ttmTextureId;      // TTM registration ID
    std::shared_ptr<TextureData>   sourceData;        // mmap handle + DDS metadata
    uint32_t                       bindlessIndex;     // unchanged from current
};
```

---

## 3. NVRHI API Audit

### 3.1 Tiled Resources (D3D12 — fully supported)

```cpp
// TextureDesc
bool isVirtual = false;  // reserved resource
bool isTiled   = false;  // sparse/tiled layout

// IDevice
void getTextureTiling(ITexture* tex, uint32_t* numTiles, PackedMipDesc* desc,
                      TileShape* shape, uint32_t* tilingsNum, SubresourceTiling* tilings);
void updateTextureTileMappings(ITexture* tex, const TextureTilesMapping* mappings,
                                uint32_t count, CommandQueue queue = Graphics);

// Key structs: PackedMipDesc, TileShape, SubresourceTiling,
//              TiledTextureCoordinate, TiledTextureRegion, TextureTilesMapping
```

### 3.2 Sampler Feedback (D3D12 only)

```cpp
// IDevice (D3D12 only)
SamplerFeedbackTextureHandle createSamplerFeedbackTexture(
    ITexture* pairedTexture, const SamplerFeedbackTextureDesc& desc);

// ICommandList
void clearSamplerFeedbackTexture(ISamplerFeedbackTexture*);
void decodeSamplerFeedbackTexture(IBuffer* dest, ISamplerFeedbackTexture* src, Format);
```

D3D12 backend: `CreateCommittedResource2(DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE)` + `ResolveSubresourceRegion(D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK)`.

### 3.3 Gaps Resolved

| Feature | Status |
|---|---|
| `isTiled` / `isVirtual` | ✅ Present |
| `getTextureTiling()` / `updateTextureTileMappings()` | ✅ Present |
| Sampler feedback create / clear / decode | ✅ D3D12 only |
| Per-tile heap upload | ✅ `writeHeap()` added ([rlaw]) |

---

## 4. RTXTS-TTM Library (`rtxts::TiledTextureManager`)

TTM is a **CPU-side scheduling library**. It does NOT create GPU resources or upload data — it tells the application **what** to map/unmap. The application executes those decisions via NVRHI.

### 4.1 Key Types

```cpp
namespace rtxts {
    struct TileCoord { uint32_t x, y; uint8_t mipLevel; };
    struct TiledLevelDesc { uint32_t widthInTiles, heightInTiles; };
    struct TiledTextureDesc {
        uint32_t textureWidth, textureHeight;
        TiledLevelDesc* tiledLevelDescs;
        uint32_t regularMipLevelsNum, packedMipLevelsNum, packedTilesNum;
        uint32_t tileWidth, tileHeight;
    };
    struct SamplerFeedbackDesc {
        uint8_t* pMinMipData;          // decoded feedback (uint8 per texel)
        uint32_t streamedMipLevelsNum;
        int32_t mipLevelBias;
    };
    struct TiledTextureManagerDesc { uint32_t heapTilesCapacity = 256; };
    struct TiledTextureManagerConfig { uint32_t numExtraStandbyTiles = 1000; };
    struct TileAllocation { uint32_t heapId, heapTileIndex; void* pHeap; };
    struct Statistics { uint32_t totalTilesNum, allocatedTilesNum, standbyTilesNum, heapFreeTilesNum; };
}
```

### 4.2 Core API

| Method | Purpose |
|---|---|
| `SetConfig(config)` | Runtime settings (standby tile count) |
| `AddTiledTexture(desc, outId)` | Register a texture |
| `RemoveTiledTexture(id)` | Unregister |
| `UpdateWithSamplerFeedback(id, fb, timestamp, timeout)` | Feed decoded feedback → internal tile state machine |
| `MatchPrimaryTexture(primaryId, followerId, ts, timeout)` | Mirror one texture's residency to another |
| `AllocateRequestedTiles()` | Assign physical tiles to pending requests |
| `GetTilesToMap(id, outIndices)` | Tiles needing `updateTextureTileMappings()` |
| `UpdateTilesMapping(id, indices)` | Acknowledge mapping complete |
| `GetTilesToUnmap(id, outIndices)` | Tiles to NULL-map |
| `TrimStandbyTiles()` | Evict excess standby |
| `GetNumDesiredHeaps()` / `AddHeap(id)` / `RemoveHeap(id)` | Heap lifecycle |
| `WriteMinMipData(id, data)` | uint8 per-tile residency |
| `GetTextureDesc(id, type)` | MinMip/feedback texture dimensions |
| `GetTileCoordinates(id)` / `GetTileAllocations(id)` | Tile layout & allocation state |
| `DefragmentTiles(n)` / `IsMovableTile(id, tile)` | Defrag support |
| `GetStatistics()` / `GetEmptyHeaps()` | Stats & cleanup |

### 4.3 Tile State Machine (Internal)

```
Free → Requested → Allocated → Mapped → Free
                        ↓           ↑
                     Standby  →─────┘
```

TTM internally drives all transitions based on feedback data. The application only executes the resulting map/unmap requests.

---

## 5. Integration Architecture (Reference Pattern)

### 5.1 FeedbackManager — Per-Frame Lifecycle

The reference's `nvfeedback::FeedbackManager` provides the NVRHI glue around TTM:

```
BeginFrame:                      (one command list execute)
  1. Readback feedback from N frames ago → mapBuffer(readbackBufN)
  2. Feed to TTM: UpdateWithSamplerFeedback() → MatchPrimaryTexture() (if TextureSets)
  3. TTM: TrimStandbyTiles() → AllocateRequestedTiles()
  4. TTM: GetTilesToUnmap() → device->updateTextureTileMappings(NULL heap) immediately
  5. TTM: GetTilesToMap() → return to application for tile data upload
  6. Heap management: GetNumDesiredHeaps() / GetEmptyHeaps() / AddHeap / RemoveHeap
  7. Clear feedback textures for next frame's render

[Application uploads tile data — raw D3D12 CopyTextureRegion]

UpdateTileMappings:              (one command list execute)
  1. TTM: UpdateTilesMapping()
  2. Group tiles by heap → device->updateTextureTileMappings() batched per heap
  3. WriteMinMipData() → upload MinMip textures for dirty textures

[Render frame — shaders write sampler feedback]

ResolveFeedback:                 (on same command list as render)
  1. decodeSamplerFeedbackTexture() for all readback textures this frame

EndFrame:
  1. Rotate ring buffer — updated textures go to back
  2. Collect statistics
```

**Ring-buffer design:** Textures are in a round-robin list. Each frame, `maxTexturesToUpdate` textures get their feedback resolved. This limits per-frame CPU overhead (feedback decode + readback) and GPU overhead (feedback texture resolve).

### 5.2 FeedbackTexture — Resource Bundle

One `FeedbackTexture` per source texture:

```
FeedbackTexture
├── m_reservedTexture          ← isTiled=true, isVirtual=true, created with source dimensions & mips
├── m_feedbackTexture          ← SamplerFeedbackTexture paired with reservedTexture
├── m_feedbackResolveBuffers[] ← N readback buffers (R8_UINT, CPU-readable), one per frame in flight
├── m_minMipTexture            ← R32_FLOAT, dimensions = tile count (e.g., 16×16 for 2048² at 128-texel tiles)
├── m_tiledTextureId           ← TTM registration ID
└── m_textureSets[]            ← which TextureSets this texture belongs to
```

**Creation flow (from reference `FeedbackTextureImpl` constructor):**

```cpp
// 1. Create reserved texture
nvrhi::TextureDesc texDesc = sourceDesc;
texDesc.isTiled = true;
texDesc.isVirtual = false;  // reserved = isTiled=true, isVirtual=false in NVRHI
texDesc.initialState = ResourceStates::ShaderResource;
texDesc.keepInitialState = true;
m_reservedTexture = device->createTexture(texDesc);

// 2. Query tiling info
device->getTextureTiling(m_reservedTexture, &numTiles, &packedMipDesc, &tileShape,
                          &mipLevels, tilingsInfo);

// 3. Register with TTM
rtxts::TiledTextureDesc ttmDesc;
ttmDesc.textureWidth = desc.width;
ttmDesc.textureHeight = desc.height;
ttmDesc.tiledLevelDescs = tiledLevelDescs;  // from tilingsInfo
ttmDesc.regularMipLevelsNum = packedMipDesc.numStandardMips;
ttmDesc.packedMipLevelsNum = packedMipDesc.numPackedMips;
ttmDesc.packedTilesNum = packedMipDesc.numTilesForPackedMips;
ttmDesc.tileWidth = tileShape.widthInTexels;
ttmDesc.tileHeight = tileShape.heightInTexels;
ttm->AddTiledTexture(ttmDesc, m_tiledTextureId);

// 4. Create sampler feedback texture (D3D12 only)
rtxts::TextureDesc fbDesc = ttm->GetTextureDesc(m_tiledTextureId, rtxts::eFeedbackTexture);
SamplerFeedbackTextureDesc sfDesc;
sfDesc.samplerFeedbackFormat = SamplerFeedbackFormat::MinMipOpaque;
sfDesc.samplerFeedbackMipRegionX = fbDesc.textureOrMipRegionWidth;
sfDesc.samplerFeedbackMipRegionY = fbDesc.textureOrMipRegionHeight;
m_feedbackTexture = deviceD3D12->createSamplerFeedbackTexture(m_reservedTexture, sfDesc);

// 5. Create readback buffers (N = numFramesInFlight)
for each frame i:
    m_feedbackResolveBuffers[i] = device->createBuffer({
        .byteSize = tileCountX * tileCountY,  // 1 uint8 per feedback texel
        .cpuAccess = CpuAccessMode::Read,
        .initialState = ResourceStates::ResolveDest,
    });

// 6. Create MinMip texture
rtxts::TextureDesc minMipDesc = ttm->GetTextureDesc(m_tiledTextureId, rtxts::eMinMipTexture);
m_minMipTexture = device->createTexture({
    .width = minMipDesc.textureOrMipRegionWidth,
    .height = minMipDesc.textureOrMipRegionHeight,
    .format = Format::R32_FLOAT,
    .initialState = ResourceStates::ShaderResource,
    .keepInitialState = true,
});
```

### 5.3 FeedbackTextureSet — Primary/Follower

Textures sharing UV coordinates (e.g., baseColor + normal + roughness of the same material) are grouped into a `FeedbackTextureSet`:

- **Primary texture** gets the sampler feedback UAV bound in shaders
- **Follower textures** share tile requests via `MatchPrimaryTexture()`
- Saves `WriteSamplerFeedback()` calls and `decodeSamplerFeedbackTexture()` invocations

```cpp
// In BeginFrame, after UpdateWithSamplerFeedback on the primary:
for each textureSet where this texture is primary:
    uint32_t primaryIdx = textureSet->GetPrimaryTextureIndex();
    for each other texture in the set:
        ttm->MatchPrimaryTexture(primaryId, followerId, timestamp, timeout);
```

**Limitation:** If a follower has higher-res mips than the primary, those extra mips won't be requested via matching — they'd need their own feedback.

### 5.4 HeapAllocator — Physical Tile Pool

```cpp
class HeapAllocator {
    void AllocateHeap(uint32_t& heapId) {
        // 1. Create NVRHI heap (device-local, capacity = heapSizeInTiles * 64KB)
        nvrhi::HeapDesc heapDesc;
        heapDesc.capacity = m_heapSizeInBytes;
        heapDesc.type = HeapType::DeviceLocal;
        nvrhi::HeapHandle heap = device->createHeap(heapDesc);

        // 2. Create virtual buffer bound to heap (for staging via CopyTextureRegion)
        nvrhi::BufferDesc bufDesc;
        bufDesc.byteSize = m_heapSizeInBytes;
        bufDesc.isVirtual = true;
        bufDesc.initialState = ResourceStates::CopySource;
        bufDesc.keepInitialState = true;
        nvrhi::BufferHandle buffer = device->createBuffer(bufDesc);
        device->bindBufferMemory(buffer, heap, 0);

        // 3. Register with TTM
        ttm->AddHeap(heapId);
    }

    void ReleaseHeap(uint32_t heapId, uint32_t frameIndex) {
        // Defer actual release by framesInFlight to avoid GPU-use-after-free
        m_buffersToRelease[frameIndex].push_back(m_buffers[heapId]);
        m_heapsToRelease[frameIndex].push_back(m_heaps[heapId]);
        ttm->RemoveHeap(heapId);
    }
};
```

---

## 6. Per-Frame Streaming Loop (Detailed)

### 6.1 Application Frame Loop

```cpp
void Renderer::RenderFrame() {
    auto& fbMgr = m_feedbackManager;

    // ── Phase A: BeginFrame ──────────────────────────────────────
    nvrhi::FeedbackTextureCollection updatedTextures;
    nvrhi::FeedbackUpdateConfig config;
    config.frameIndex = m_frameIndex;
    config.maxTexturesToUpdate = 8;       // per-frame feedback budget
    config.tileTimeoutSeconds = 0.0f;     // aggressive: evict immediately
    config.defragmentHeaps = true;
    config.trimStandbyTiles = true;
    config.releaseEmptyHeaps = true;
    config.numExtraStandbyTiles = 0;      // no standby — max VRAM savings

    m_commandList->open();
    fbMgr->BeginFrame(m_commandList, config, &updatedTextures);
    // → TTM processes feedback, returns tiles to map
    m_commandList->close();
    device->executeCommandList(m_commandList);

    // ── Phase B: Tile Data Upload (per-texture) ──────────────────
    std::queue<RequestedTile> requestQueue;
    for (auto& texUpdate : updatedTextures.textures) {
        for (auto tileIndex : texUpdate.tileIndices) {
            requestQueue.push({texUpdate.texture, tileIndex});
        }
    }

    uint32_t uploadBudget = std::min(m_ui.tilesPerFrame, requestQueue.size());
    FeedbackTextureCollection tilesThisFrame;

    for (uint32_t i = 0; i < uploadBudget; i++) {
        auto& req = requestQueue.front();
        // Upload tile data via raw D3D12 CopyTextureRegion (reference approach):
        //   ExtractTileFromLinearDDS() → CPU staging buffer →
        //   D3D12 CopyTextureRegion(stagingBuf → reservedTex at tile subresource coords)
        UploadTileData(req);
        tilesThisFrame[req.texture].push_back(req.tileIndex);
        requestQueue.pop();
    }

    // ── Phase C: UpdateTileMappings ──────────────────────────────
    m_commandList->open();
    fbMgr->UpdateTileMappings(m_commandList, &tilesThisFrame);
    // → TTM: UpdateTilesMapping()
    // → NVRHI: updateTextureTileMappings() per heap
    // → NVRHI: writeTexture() MinMip update for dirty textures
    m_commandList->close();
    device->executeCommandList(m_commandList);

    // ── Phase D: Render ──────────────────────────────────────────
    RenderGBuffer();   // shaders write sampler feedback
    RenderLighting();  // shaders use MinMip for fallback LOD

    // ── Phase E: ResolveFeedback ─────────────────────────────────
    m_commandList->open();
    fbMgr->ResolveFeedback(m_commandList);
    // → decodeSamplerFeedbackTexture() for readback textures this frame
    m_commandList->close();
    device->executeCommandList(m_commandList);

    // ── Phase F: EndFrame ────────────────────────────────────────
    fbMgr->EndFrame();  // rotate ring buffer, collect stats
    m_frameIndex++;
}
```

### 6.2 FeedbackManager::BeginFrame Internals

```cpp
void FeedbackManagerImpl::BeginFrame(CL* cmd, const FeedbackUpdateConfig& cfg,
                                      FeedbackTextureCollection* results) {
    m_frameIndex = cfg.frameIndex % m_numFramesInFlight;

    // 1. Readback feedback from N frames ago
    auto& readbackTextures = m_texturesToReadback[m_frameIndex];
    for (auto* tex : readbackTextures) {
        uint8_t* data = (uint8_t*)device->mapBuffer(
            tex->GetFeedbackResolveBuffer(m_frameIndex), CpuAccessMode::Read);
        rtxts::SamplerFeedbackDesc fbDesc;
        fbDesc.pMinMipData = data;
        ttm->UpdateWithSamplerFeedback(tex->GetTiledTextureId(), fbDesc,
                                        GetTimestamp(), cfg.tileTimeoutSeconds);
        device->unmapBuffer(tex->GetFeedbackResolveBuffer(m_frameIndex));

        // Match followers if this is a primary texture
        if (tex->IsPrimaryTexture()) {
            for (auto* set : tex->GetPrimaryTextureSets())
                for (uint32_t i = 0; i < set->GetNumTextures(); i++)
                    if (i != set->GetPrimaryTextureIndex())
                        ttm->MatchPrimaryTexture(tex->GetTiledTextureId(),
                            set->GetTexture(i)->GetTiledTextureId(),
                            GetTimestamp(), cfg.tileTimeoutSeconds);
        }
    }

    // 2. Prepare next batch of textures for feedback readback
    readbackTextures.clear();
    for (auto* tex : m_texturesRingbuffer) {
        if (updatesLeft-- == 0) break;
        cmd->clearSamplerFeedbackTexture(tex->GetSamplerFeedbackTexture());
        readbackTextures.push_back(tex);
    }

    // 3. TTM: trim, allocate, get tiles to map/unmap
    if (cfg.trimStandbyTiles) ttm->TrimStandbyTiles();
    ManageHeaps(cfg.releaseEmptyHeaps);
    ttm->AllocateRequestedTiles();

    // 4. Unmap tiles immediately
    for (auto* tex : m_textures) {
        std::vector<uint32_t> toUnmap;
        ttm->GetTilesToUnmap(tex->GetTiledTextureId(), toUnmap);
        if (!toUnmap.empty())
            device->updateTextureTileMappings(tex->GetReservedTexture(), NULL, ...);

        // Collect tiles to map → returned to application
        std::vector<uint32_t> toMap;
        ttm->GetTilesToMap(tex->GetTiledTextureId(), toMap);
        if (!toMap.empty())
            results->textures.push_back({tex, toMap});
    }

    ttm->DefragmentTiles(16); // 16 tiles/frame defrag budget
}
```

---

## 7. Tile Data Upload

### 7.1 DDS Row-Skipping Problem

DDS stores mip subresources in **row-major (linear) order**. Extracting a single tile requires row-by-row copy with pitch skipping:

```
DDS BC7 mip 0 (2048×2048):
  Row 0: [block0..block511]    ← rowPitch = 512 blocks × 16 bytes = 8192 bytes
  Row 1: [block0..block511]
  ...

To extract tile at (512, 0) = 128×128 pixels = 32×32 BC7 blocks:
  For each of 32 rows:
    Copy 32 blocks × 16 bytes = 512 bytes    ← tile data
    Skip  (512 - 32) × 16 = 7680 bytes       ← other tiles' data
  Total: 32 × 512 = 16 KB copied, 32 × 7680 = 240 KB skipped
```

### 7.2 Reference Upload Approach: CopyTextureRegion

The reference uploads tile data **directly to the reserved (tiled) texture** using raw D3D12 `CopyTextureRegion`. The GPU automatically converts linear→tiled layout and routes data to the mapped physical tile.

```cpp
// From reference TileUploadHelper::UploadTile():
// 1. Extract tile rows from mmap (row-by-row copy) → CPU staging buffer
uint32_t rowPitchTile = tileBlocksWidth * bytesPerBlock;
for (uint32_t row = 0; row < tileBlocksHeight; row++) {
    uint32_t readOffset = (sourceBlockY + row) * rowPitchSource + sourceBlockX * bytesPerBlock;
    memcpy(mappedData + row * rowPitchTile, dataMipBase + readOffset, rowPitchTile);
}

// 2. Raw D3D12 CopyTextureRegion: staging buffer → reserved texture subresource
D3D12_TEXTURE_COPY_LOCATION srcLocation;
srcLocation.pResource = stagingBuffer;
srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
srcLocation.PlacedFootprint.Offset = bufferOffset;
srcLocation.PlacedFootprint.Footprint.Format = DXGI_FORMAT_BC7_UNORM;
srcLocation.PlacedFootprint.Footprint.Width = tile.widthInTexels;
srcLocation.PlacedFootprint.Footprint.Height = tile.heightInTexels;
srcLocation.PlacedFootprint.Footprint.RowPitch = rowPitchTile;

D3D12_TEXTURE_COPY_LOCATION dstLocation;
dstLocation.pResource = reservedTexture;
dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
dstLocation.SubresourceIndex = tile.mip;

pCommandList->CopyTextureRegion(&dstLocation, tile.xInTexels, tile.yInTexels, 0,
                                 &srcLocation, nullptr);
```

**Important:** Must call `updateTextureTileMappings()` BEFORE `CopyTextureRegion()` so the tile is mapped when the copy executes. The reference calls `requireTextureState(CopyDest)` + `commitBarriers()` on the raw D3D12 command list for each texture before upload — this bypasses NVRHI's automatic barriers.

### 7.3 Alternative: writeHeap (Our NVRHI Addition)

Our added `writeHeap()` enables upload without raw D3D12:

```cpp
// Upload pre-swizzled tile data directly to the heap at tile offset
cmd->writeHeap(tilePoolHeap, alloc.heapTileIndex * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES,
               tileData, tileSizeBytes);
// Then map the tile via updateTextureTileMappings()
```

This requires tile data to already be in GPU-tiled layout, or a compute shader to swizzle it.

---

## 8. Packed Mip Strategy

### 8.1 What Are Packed Mips?

D3D12 tiled resources split the mip chain into two regions:

```
Standard Mips (individually mappable per tile)
│ Mip0 tiles │ Mip1 tiles │ Mip2 tiles │ ...
├────────────────────────────────────────────
Packed Mips (all-or-nothing, single tile allocation)
│ Mip N, Mip N+1, ..., Mip M (shared tiles) │
```

`PackedMipDesc` from `getTextureTiling()` gives: `numStandardMips`, `numPackedMips`, `numTilesForPackedMips`, `startTileIndexInOverallResource`.

### 8.2 Pre-Map Packed Mips at Load

Packed mips are pre-loaded at scene init and never evicted:

```cpp
// For each streaming texture at scene load:
// 1. Create a small committed texture just for packed mips
nvrhi::TextureDesc packedDesc;
packedDesc.width = std::max(sourceWidth >> packedMipStart, 1u);
packedDesc.height = std::max(sourceHeight >> packedMipStart, 1u);
packedDesc.mipLevels = packedMipDesc.numPackedMips;
packedDesc.isTiled = false;   // committed, not reserved
auto packedTex = device->createTexture(packedDesc);

// 2. Upload packed mip data from DDS
UploadTextureRegion(packedTex, ddsPackedMipData);

// 3. Copy to physical tiles and map permanently
CopyTextureRegion(/*committed packedTex → reservedTexture at packed mip coords*/);
device->updateTextureTileMappings(reservedTex, &packedMipMapping);
// These tiles are never added to TTM's eviction — they stay mapped permanently
```

### 8.3 Non-Square Textures

Non-square textures (e.g., 1024×128 decals) work naturally:

```
1024×128 BC7, 11 mips:
  Mip 0: 1024×128 → 256×32 blocks → 8×1 tiles
  Mip 1: 512×64   → 128×16 blocks → 4×1 tiles
  Mip 2: 256×32   → 64×8  blocks  → 2×1 tiles
  Mip 3: 128×16   → 32×4  blocks  → 1×1 tile   (smaller than tile → packs earlier)
  ...
```

`getTextureTiling()` returns the correct tile layout for any aspect ratio. Row extraction logic handles arbitrary mip widths/heights naturally.

---

## 9. Shader Integration

### 9.1 Required HLSL Patterns

```hlsl
// Per-texture bindings (reference approach):
// - tiledTexture (SRV)           : the reserved resource
// - regularSampler (Sampler)     : for main texture sampling
// - pointSampler (Sampler)       : for MinMip lookup (point sample)
// - minMipTexture (SRV)          : R32_FLOAT, per-tile lowest resident mip
// - feedbackTexture (UAV)        : SamplerFeedback UAV for WriteSamplerFeedback

float4 SampleWithFallback(
    Texture2D tex, SamplerState samp, SamplerState pointSamp,
    Texture2D<float> minMipTex, float2 uv)
{
    uint2 feedbackCoord = uint2(uv * float2(feedbackWidth, feedbackHeight));
    float minResidentMip = minMipTex.SampleLevel(pointSamp, uv, 0);

    // Try sampling at the computed LOD, fall back if not resident
    float lod = tex.CalculateLevelOfDetail(samp, uv);
    float status;
    float4 color = tex.Sample(samp, uv, 0, status);

    if (!CheckAccessFullyMapped(status)) {
        // Walk down mip pyramid until we find a resident level
        for (int mip = 1; mip <= (int)minResidentMip; mip++) {
            color = tex.SampleLevel(samp, uv, mip, status);
            if (CheckAccessFullyMapped(status))
                break;
        }
    }
    return color;
}
```

### 9.2 Critical Requirements

- **`[earlydepthstencil]`** on pixel shader entry — required when using `WriteSamplerFeedback()`. Without it, Early-Z is disabled and performance tanks.
- **Use `Sample()` with `status` out-parameter**, not `SampleLevel()`, to get native access tracking.
- **Write feedback at original requested LOD**, not the fallback:
  ```hlsl
  color.WriteSamplerFeedback(feedbackTex, samp, uv, originalLod);
  ```
- **MinMip fallback**: Packed mips guarantee at least the lowest mips are always resident, so the fallback loop always terminates.

### 9.3 NVRHI Binding Setup

```cpp
// Per-texture binding (reference approach):
auto bindingSet = device->createBindingSet({
    BindingSetItem::Texture_SRV(0, feedbackTexture->GetReservedTexture()),
    BindingSetItem::Sampler(0, regularSampler),
    BindingSetItem::Sampler(1, pointSampler),
    BindingSetItem::Texture_SRV(1, feedbackTexture->GetMinMipTexture()),
    BindingSetItem::SamplerFeedbackTexture_UAV(0, 
        feedbackTexture->GetSamplerFeedbackTexture()),
});
```

---

## 10. Eviction & Memory Strategy

### 10.1 TTM-Driven Eviction

All eviction is handled by TTM internally. Application control points:

| Control | Effect |
|---|---|
| `numExtraStandbyTiles = 0` | Tiles evicted immediately when not in feedback |
| `timeout = 0.0f` | No grace period before standby |
| `TrimStandbyTiles()` | Evict excess standby to target |
| `releaseEmptyHeaps` | Release heaps with no allocations |

### 10.2 Aggressive Eviction for Max VRAM Savings

```cpp
rtxts::TiledTextureManagerConfig config;
config.numExtraStandbyTiles = 0;       // no standby cache
ttm->SetConfig(config);

// In BeginFrame:
fbDesc.timeout = 0.0f;                  // evict as soon as tile drops from feedback
ttm->UpdateWithSamplerFeedback(id, fbDesc, timestamp, 0.0f);
config.trimStandbyTiles = true;         // call TrimStandbyTiles() every frame
config.releaseEmptyHeaps = true;        // release unused heaps
```

### 10.3 Pool Pressure

TTM's heap system scales dynamically: `GetNumDesiredHeaps()` returns the current demand. If the pool fills, the standby queue tail is evicted. Packed mips (pre-mapped, outside TTM's eviction cycle) always provide fallback.

---

## 11. Implementation Roadmap

### Phase 1 — TTM Integration + Resource Setup (5-7 days)

1. Integrate RTXTS-TTM library into CMake build
2. Port `FeedbackManager` + `FeedbackTexture` + `HeapAllocator` from reference
3. Scene loader: create `StreamingTexture` instead of committed `TextureHandle` for DDS textures
4. Pre-map packed mips at scene load, register each texture with TTM
5. Create paired `ISamplerFeedbackTexture` per streaming texture (D3D12 only path)
6. Create readback buffer ring (N = framesInFlight) per texture
7. Create MinMip texture per texture

### Phase 2 — Shader Integration + Feedback Loop (4-5 days)

8. Implement `SampleWithFallback()` HLSL helper using `CheckAccessFullyMapped()`
9. Bind feedback UAV + MinMip SRV in material bindings
10. Add `WriteSamplerFeedback()` to pixel shader, annotate with `[earlydepthstencil]`
11. Implement per-frame `BeginFrame` / `ResolveFeedback` / `EndFrame` loop
12. Ring-buffer round-robin for feedback resolution budget

### Phase 3 — Tile Upload (4-6 days)

13. Implement `ExtractTileFromLinearDDS()` — row-by-row copy with pitch skipping
14. Implement tile upload: CPU staging buffer → raw D3D12 `CopyTextureRegion` to reserved texture (reference approach)
15. Or: implement `writeHeap()`-based upload path (our NVRHI addition)
16. Execute `UpdateTileMappings()`: batch per-heap `updateTextureTileMappings()` calls
17. Implement MinMip dirty-texture tracking: `WriteMinMipData()` → `writeTexture()` upload

### Phase 4 — Debug & Polish (2-3 days)

18. ImGui stats panel using `rtxts::GetStatistics()`
19. Per-texture residency visualization (green/red tile map overlay)
20. Perf profiling: BeginFrame time, UpdateTileMappings time, tiles uploaded/frame, heap count
21. Tuning: `maxTexturesToUpdate`, `tilesPerFrame`, `timeout`, `numExtraStandbyTiles`

**Total estimate: 15-21 days**

---

## 12. Async I/O & DirectStorage

### 12.1 Async I/O Thread

Disk reads must not block the render thread:

```
Render Thread                    I/O Thread
─────────────                    ─────────
Analyze feedback                 while(running):
  ↓                                request = dequeue()
For each needed tile:              mmap DDS
  enqueue(tileReq) ──────→        extractTile() → staging buffer
                                   signalCompletion(req)
                                   
Next frame:
  for each completed:
    uploadTile(staging) ←───────  (staging buffer ready)
    updateTileMappings()
```

**Key design:**
- Single I/O thread, not thread pool per request
- Ring buffer of pre-allocated staging buffers (e.g., 64 × 64KB = 4MB)
- Completion via atomics, not mutexes — if not ready, defer to next frame
- Keep one mmap handle per DDS file open (don't open/close per tile)

**Phase 1 stub:** Synchronous fallback on render thread, structured for later async migration.

### 12.2 DirectStorage Stub

Create an `ITileIOReader` interface:

```cpp
class ITileIOReader {
public:
    virtual bool ReadTileData(const TileRequest& req, void* stagingBuffer) = 0;
};

// Phase 1: Memory-mapped (synchronous)
class MMapTileReader : public ITileIOReader { ... };

// Phase 2 stub: DirectStorage (not functional, falls back to mmap)
class DirectStorageTileReader : public ITileIOReader {
    IDStorageFactory* m_factory = nullptr;
    IDStorageQueue*   m_queue   = nullptr;
    bool ReadTileData(...) override { return m_fallback.ReadTileData(...); }
};
```

---

## 13. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| **sampler feedback D3D12-only** — no Vulkan support | Target D3D12 exclusively for streaming; Vulkan path falls back to committed textures |
| **`[earlydepthstencil]` + WriteSamplerFeedback required** | Enforce in shader compilation |
| **Raw D3D12 CopyTextureRegion bypasses NVRHI validation** | Use `writeHeap()` alternative, or wrap in NVRHI properly |
| **Ring-buffer round-robin latency** — new textures wait N frames for first feedback | Acceptable 1-2 frame pop-in; packed mips always provide fallback |
| **TTM license** (NVIDIA proprietary) | Verify license allows usage; RTXTS SDK distributed under NVIDIA RTX SDKs LICENSE |
| **BC-only formats** — reference only supports BC compressed textures | Extend tile extraction for uncompressed formats (RGBA8, etc.) |
| **Tile pool exhaustion** | TTM handles via standby eviction; packed mips always available |
| **Pop-in on slow storage (HDD)** | Async I/O prevents render thread blocking; packed mips mask latency |

---

## 14. Future: Atlas Indirection (Post-Reference)

> The reference uses per-texture tiled resources. An **atlas indirection layer** — packing multiple source textures into a single reserved resource — can be added later for:
> - Single `decodeSamplerFeedbackTexture()` per frame (vs. N separate decodes)
> - Single `updateTextureTileMappings()` batched call
> - One bindless slot for all streamed textures
>
> Cost: page table indirection in every texture sample (~2-3 ALU ops). This is a future optimization; the initial implementation should follow the reference's per-texture approach.

---

## 15. References

- **RTXTS Reference Sample**: `REFERENCES/RTXTS/` — `FeedbackManager.h/cpp`, `FeedbackTexture.h/cpp`, `main.cpp` (frame loop)
- **RTXTS-TTM Library**: `REFERENCES/RTXTS/libraries/rtxts-ttm/include/rtxts-ttm/TiledTextureManager.h`
- **NVRHI**: `external/nvrhi/include/nvrhi/nvrhi.h` — tiled resource + sampler feedback API
- **NVRHI D3D12 backend**: `external/nvrhi/src/d3d12/` — `createSamplerFeedbackTexture`, `updateTextureTileMappings`
- **Current texture loading**: `src/SceneLoader.cpp` (`LoadTexturesFromImages`), `src/TextureLoader.cpp` (`UploadTexture`)
- **Bindless registration**: `src/Renderer.cpp` (`RegisterTexture`)
- **MemoryMappedDataReader**: `src/Utilities.h`
