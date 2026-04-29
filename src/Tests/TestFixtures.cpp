#include "TestFixtures.h"

std::string GltfSampleModel(const char* relPath)
{
    const std::string root = GltfSamplesRoot();
    if (root.empty()) return "";
    // Normalise trailing slash
    std::filesystem::path p = std::filesystem::path(root) / "Models" / relPath;
    return p.string();
}

bool SampleModelExists(const char* relPath)
{
    const std::string path = GltfSampleModel(relPath);
    if (path.empty()) return false;
    return std::filesystem::exists(path);
}

SceneScope::SceneScope(const char* modelRelPath)
{
    const std::string path = GltfSampleModel(modelRelPath);
    if (path.empty() || !std::filesystem::exists(path))
        return;

    // LoadScene() appends into the existing Scene, so drop any previous scene first.
    if (DEV())
        DEV()->waitForIdle();
    g_Renderer.m_Scene.Shutdown();

    // Temporarily override Config scene path
    Config& cfg = const_cast<Config&>(Config::Get());
    const std::string prevPath = cfg.m_ScenePath;

    cfg.m_ScenePath = path;

    // Re-create the default cube and preallocate geometry buffers after Shutdown cleared them.
    g_Renderer.m_Scene.InitializeDefaultCube(0, 0);
    g_Renderer.ExecutePendingCommandLists();

    g_Renderer.m_Scene.LoadScene();

    // Scene file loads are async by default; drain both background queues and
    // consume pending update commands so tests can safely assert on texture/mesh validity.
    g_Renderer.m_AsyncTextureQueue.Flush();
    g_Renderer.m_AsyncMeshQueue.Flush();

    size_t pendingTextures = 1;
    size_t pendingMeshes = 1;
    while (pendingTextures > 0 || pendingMeshes > 0)
    {
        {
            std::lock_guard<std::mutex> lk(g_Renderer.m_Scene.m_PendingTextureMutex);
            pendingTextures = g_Renderer.m_Scene.m_PendingTextureUpdates.size();
        }
        {
            std::lock_guard<std::mutex> lk(g_Renderer.m_Scene.m_PendingMeshMutex);
            pendingMeshes = g_Renderer.m_Scene.m_PendingMeshUpdates.size();
        }

        g_Renderer.m_Scene.ApplyPendingUpdates();
        g_Renderer.ExecutePendingCommandLists();

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    loaded = !g_Renderer.m_Scene.m_Meshes.empty() ||
        !g_Renderer.m_Scene.m_Nodes.empty();

    // Restore config
    cfg.m_ScenePath = prevPath;
}

SceneScope::~SceneScope()
{
    // Wait for GPU idle before releasing scene GPU resources
    if (DEV())
        DEV()->waitForIdle();
    g_Renderer.m_Scene.Shutdown();
}

std::filesystem::path ReferenceImagePath(const char* filename)
{
    // Try: <exe_dir>/../../src/Tests/ReferenceImages/<filename>
    // (works when running from build/Debug or build/Release)
    const char* base = SDL_GetBasePath();
    if (base)
    {
        std::filesystem::path p =
            std::filesystem::path(base) / ".." / ".." / "src" / "Tests" / "ReferenceImages" / filename;
        p = std::filesystem::weakly_canonical(p);
        if (std::filesystem::exists(p))
            return p;
    }

    // Fallback: relative to CWD
    return std::filesystem::path("src") / "Tests" / "ReferenceImages" / filename;
}

nvrhi::TextureHandle CreateTestTexture2D(
    uint32_t width, uint32_t height,
    nvrhi::Format format,
    const void* initialData,
    size_t rowPitch,
    const char* debugName)
{
    nvrhi::IDevice* device = DEV();
    if (!device) return nullptr;

    nvrhi::TextureDesc desc;
    desc.width            = width;
    desc.height           = height;
    desc.depth            = 1;
    desc.arraySize        = 1;
    desc.mipLevels        = 1;
    desc.format           = format;
    desc.dimension        = nvrhi::TextureDimension::Texture2D;
    desc.initialState     = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.isShaderResource = true;
    desc.debugName        = debugName;

    nvrhi::TextureHandle tex = device->createTexture(desc);
    if (!tex) return nullptr;

    if (initialData)
    {
        nvrhi::CommandListHandle cmd = g_Renderer.AcquireCommandList();
        cmd->open();
        cmd->writeTexture(tex, 0, 0, initialData, rowPitch);
        cmd->close();
        g_Renderer.ExecutePendingCommandLists();
    }

    return tex;
}

IRenderer* FindRendererByName(const char* name)
{
    for (const auto& r : g_Renderer.m_Renderers)
        if (r && std::string_view(r->GetName()) == name)
            return r.get();
    return nullptr;
}

// ============================================================================
// MinimalSceneFixture
// ============================================================================

MinimalSceneFixture::MinimalSceneFixture()
{
    // Shut down any previously loaded scene so we start from a clean slate.
    if (DEV())
        DEV()->waitForIdle();
    g_Renderer.m_Scene.Shutdown();
    g_Renderer.m_RenderGraph.Shutdown();

    // Invariant: Shutdown() must leave the dirty ranges clean so the warm-up
    // RunOneFrame() calls below do not attempt to upload a stale dirty range
    // against a not-yet-allocated GPU buffer.
    SDL_assert(!g_Renderer.m_Scene.AreInstanceTransformsDirty() &&
        "MinimalSceneFixture: m_InstanceDirtyRange is dirty after Shutdown() — "
        "a previous test left stale state that Shutdown() failed to clear");

    // Recreate default cube + GPU geometry buffers that the sync upload path appends into.
    g_Renderer.m_Scene.InitializeDefaultCube(0, 0);
    g_Renderer.ExecutePendingCommandLists();

    g_Renderer.m_PrevFrameExposure = 1.0f;
    // Ensure animations are enabled — a previous test using ReferencePathTracer
    // mode may have left m_EnableAnimations=false (PathTracerRenderer::Render sets
    // it to false to pause animations during path tracing).
    g_Renderer.m_EnableAnimations = true;

    // Minimal valid glTF 2.0: a single triangle with 3 POSITION vertices.
    // All buffer data is embedded as a base64 data URI so no file I/O is needed.
    // The scene has no lights — SceneLoader::LoadGLTFSceneFromMemory will call
    // EnsureDefaultDirectionalLight to inject the default directional light.
    static constexpr const char k_MinimalGltf[] = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 } } ] } ],
  "accessors": [ {
    "bufferView": 0, "byteOffset": 0,
    "componentType": 5126, "count": 3, "type": "VEC3",
    "max": [ 1.0, 1.0, 0.0 ], "min": [ 0.0, 0.0, 0.0 ]
  } ],
  "bufferViews": [ {
    "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962
  } ],
  "buffers": [ {
    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA",
    "byteLength": 36
  } ]
})";

    std::vector<srrhi::VertexQuantized> vertices;
    std::vector<uint32_t> indices;

    const bool ok = SceneLoader::LoadGLTFSceneFromMemory(
        g_Renderer.m_Scene,
        k_MinimalGltf, sizeof(k_MinimalGltf) - 1, // exclude null terminator
        {},   // no sceneDir — all data is embedded
        vertices, indices);

    SDL_assert(ok && "MinimalSceneFixture: failed to load hardcoded glTF from memory");

    g_Renderer.m_Scene.FinalizeLoadedScene();
    SceneLoader::LoadTexturesFromImages(g_Renderer.m_Scene, {});

    SDL_assert(g_Renderer.m_Scene.m_VertexBufferQuantized &&
        "MinimalSceneFixture: vertex buffer missing before UploadGeometryBuffers");
    SDL_assert(g_Renderer.m_Scene.m_IndexBuffer &&
        "MinimalSceneFixture: index buffer missing before UploadGeometryBuffers");
    
    g_Renderer.m_Scene.UploadGeometryBuffers(vertices, indices);
    SceneLoader::CreateAndUploadLightBuffer(g_Renderer.m_Scene);
    {
        nvrhi::CommandListHandle cmd = g_Renderer.AcquireCommandList();
        ScopedCommandList scopedCmd{ cmd, "Scene_BuildInitialBLAS" };
        SceneLoader::UpdateMaterialsAndCreateConstants(g_Renderer.m_Scene, cmd);
        g_Renderer.m_Scene.BuildAccelerationStructures(cmd);
    }

    for (const auto& r : g_Renderer.m_Renderers)
        if (r) r->PostSceneLoad();
    g_Renderer.ExecutePendingCommandLists();

    // RenderGraph::Shutdown() sets m_ForceInvalidateFramesRemaining=2, which
    // keeps m_bForceInvalidateAllResources true for the first 2 frames after
    // construction.  Run those 2 frames here so every test body starts with
    // the flag already cleared and persistent handles are stable from frame 1.
    RunOneFrame();
    RunOneFrame();
}

MinimalSceneFixture::~MinimalSceneFixture()
{
    if (DEV())
        DEV()->waitForIdle();
    // Restore global renderer state that the warm-up frames or test bodies may
    // have modified, so subsequent bare TEST_CASEs see the expected defaults.
    g_Renderer.m_EnableAnimations = true;
    g_Renderer.m_PrevFrameExposure = 1.0f;
    g_Renderer.m_Scene.Shutdown();
    g_Renderer.m_RenderGraph.Shutdown();
}

bool RunOneFrame()
{
    DEV()->runGarbageCollection();

    // Update camera view constants (required by culling shaders)
    int windowW = 0, windowH = 0;
    SDL_GetWindowSize(g_Renderer.m_Window, &windowW, &windowH);
    g_Renderer.m_Scene.m_ViewPrev = g_Renderer.m_Scene.m_View;
    g_Renderer.m_Scene.m_Camera.FillPlanarViewConstants(
        g_Renderer.m_Scene.m_View,
        static_cast<float>(windowW),
        static_cast<float>(windowH));

    // Drive the ImGui layer so that ImGui_ImplSDL3_NewFrame() is called each
    // frame.  This sets ImGuiIO::DisplaySize (queried from the SDL window) and
    // advances ImGui's internal frame state.  Without this call, tests that
    // inspect io.DisplaySize (TC-IMGUI-FONT-04) or that exercise ImGuiRenderer
    // would see a zero-sized display and potentially crash.
    g_Renderer.m_ImGuiLayer.UpdateFrame();

    // Upload any dirty instance transforms before renderers run, mirroring the
    // call in RenderFrame().  Both paths call the same helper so the logic stays
    // in one place (Renderer::UploadDirtyInstanceTransforms).
    g_Renderer.UploadDirtyInstanceTransforms();

    // Upload any dirty material constants (animated emissive, test mutations).
    // Mirrors the call in RenderFrame() so the unit-test path exercises the same
    // GPU upload logic as the main game loop.
    g_Renderer.UploadDirtyMaterialConstants();

    g_Renderer.ScheduleAndRunAllRenderers();

    // Submit to GPU and wait for completion.
    // waitForIdle() is mandatory here — not just for correctness of readback
    // tests, but to prevent D3D12 ERROR #921 (OBJECT_DELETED_WHILE_STILL_IN_USE).
    //
    // Root cause: RenderGraph::Compile() may recreate a transient texture handle
    // (aliased or desc-changed path) by move-assigning a new RefCountPtr into
    // texture.m_PhysicalTexture.  The move-assign drops the old handle, which
    // decrements its refcount.  If that refcount reaches zero while the GPU is
    // still executing work that references the resource, D3D12 fires ERROR #921.
    //
    // The deferred-release list in RenderGraph (FlushDeferredReleases, called at
    // the top of Reset()) is the primary fix: it defers the actual drop until the
    // GPU is known to be idle.  This waitForIdle() is the secondary guarantee:
    // it ensures that by the time the *next* frame's Reset() runs, all in-flight
    // GPU work from this frame has already completed, so FlushDeferredReleases()
    // finds the GPU idle and its internal waitForIdle() is a no-op.
    //
    // Without this call, tight loops (e.g. TC-MF-06: 7 frames back-to-back)
    // could submit frame N's commands and immediately start frame N+1's
    // Compile(), triggering the recreation path while frame N is still running.
    g_Renderer.ExecutePendingCommandLists();
    DEV()->waitForIdle();
    DEV()->runGarbageCollection();

    g_Renderer.m_FrameTime = 16.6f; // assume 60 FPS for testing purposes
    ++g_Renderer.m_FrameNumber;

    return true;
}

bool RunNFrames(int n)
    {
        for (int i = 0; i < n; ++i)
            if (!RunOneFrame()) return false;
        return true;
    }

float ReadbackTexelFloat(nvrhi::TextureHandle tex, uint32_t x, uint32_t y)
{
    if (!tex) return -1.0f;

    nvrhi::IDevice* device = DEV();
    const nvrhi::TextureDesc& d = tex->getDesc();

    // Create a 1x1 staging texture (CPU-readable) with the same format
    nvrhi::TextureDesc stagingDesc = d;
    stagingDesc.width = 1;
    stagingDesc.height = 1;
    stagingDesc.mipLevels = 1;
    stagingDesc.isRenderTarget = false;
    stagingDesc.isUAV = false;
    stagingDesc.initialState = nvrhi::ResourceStates::Common;
    stagingDesc.keepInitialState = false;
    stagingDesc.debugName = "ReadbackStaging";

    nvrhi::StagingTextureHandle staging = device->createStagingTexture(
        stagingDesc, nvrhi::CpuAccessMode::Read);
    if (!staging) return -1.0f;

    nvrhi::CommandListHandle cmd = g_Renderer.AcquireCommandList();
    cmd->open();

    nvrhi::TextureSlice srcSlice;
    srcSlice.x = x;
    srcSlice.y = y;
    srcSlice.width = 1;
    srcSlice.height = 1;
    srcSlice.mipLevel = 0;
    srcSlice.arraySlice = 0;

    nvrhi::TextureSlice dstSlice;
    dstSlice.x = 0;
    dstSlice.y = 0;
    dstSlice.width = 1;
    dstSlice.height = 1;
    dstSlice.mipLevel = 0;
    dstSlice.arraySlice = 0;

    cmd->copyTexture(staging, dstSlice, tex, srcSlice);
    cmd->close();
    g_Renderer.ExecutePendingCommandLists();
    device->waitForIdle();

    // Map and read
    size_t rowPitch = 0;
    void* mapped = device->mapStagingTexture(staging, dstSlice, nvrhi::CpuAccessMode::Read, &rowPitch);
    if (!mapped) return -1.0f;

    float value = 0.0f;
    if (d.format == nvrhi::Format::R32_FLOAT)
    {
        value = *reinterpret_cast<const float*>(mapped);
    }
    else if (d.format == nvrhi::Format::R16_FLOAT)
    {
        // Simple half-to-float conversion
        const uint16_t h = *reinterpret_cast<const uint16_t*>(mapped);
        const uint32_t sign = (h >> 15) & 0x1;
        const uint32_t exponent = (h >> 10) & 0x1F;
        const uint32_t mantissa = h & 0x3FF;
        uint32_t f32 = 0;
        if (exponent == 0)
            f32 = (sign << 31) | (mantissa << 13);
        else if (exponent == 31)
            f32 = (sign << 31) | (0xFF << 23) | (mantissa << 13);
        else
            f32 = (sign << 31) | ((exponent + 112) << 23) | (mantissa << 13);
        std::memcpy(&value, &f32, sizeof(float));
    }

    device->unmapStagingTexture(staging);
    return value;
}

uint32_t ReadbackTexelRGBA8(nvrhi::TextureHandle tex, uint32_t x, uint32_t y)
{
    if (!tex) return 0xDEADBEEF;

    nvrhi::IDevice* device = DEV();
    const nvrhi::TextureDesc& d = tex->getDesc();

    nvrhi::TextureDesc stagingDesc = d;
    stagingDesc.width = 1;
    stagingDesc.height = 1;
    stagingDesc.mipLevels = 1;
    stagingDesc.isRenderTarget = false;
    stagingDesc.isUAV = false;
    stagingDesc.initialState = nvrhi::ResourceStates::Common;
    stagingDesc.keepInitialState = false;
    stagingDesc.debugName = "ReadbackStagingRGBA8";

    nvrhi::StagingTextureHandle staging = device->createStagingTexture(
        stagingDesc, nvrhi::CpuAccessMode::Read);
    if (!staging) return 0xDEADBEEF;

    nvrhi::CommandListHandle cmd = g_Renderer.AcquireCommandList();
    cmd->open();

    nvrhi::TextureSlice srcSlice;
    srcSlice.x = x; srcSlice.y = y;
    srcSlice.width = 1; srcSlice.height = 1;
    srcSlice.mipLevel = 0; srcSlice.arraySlice = 0;

    nvrhi::TextureSlice dstSlice;
    dstSlice.x = 0; dstSlice.y = 0;
    dstSlice.width = 1; dstSlice.height = 1;
    dstSlice.mipLevel = 0; dstSlice.arraySlice = 0;

    cmd->copyTexture(staging, dstSlice, tex, srcSlice);
    cmd->close();
    g_Renderer.ExecutePendingCommandLists();
    device->waitForIdle();

    size_t rowPitch = 0;
    void* mapped = device->mapStagingTexture(staging, dstSlice, nvrhi::CpuAccessMode::Read, &rowPitch);
    if (!mapped) return 0xDEADBEEF;

    uint32_t value = *reinterpret_cast<const uint32_t*>(mapped);
    device->unmapStagingTexture(staging);
    return value;
}

RGTextureDesc MakeTexDesc(uint32_t w, uint32_t h, nvrhi::Format fmt, bool isUAV, const char* name)
{
    RGTextureDesc d;
    d.m_NvrhiDesc = nvrhi::TextureDesc()
        .setWidth(w).setHeight(h)
        .setFormat(fmt)
        .setDimension(nvrhi::TextureDimension::Texture2D)
        .setDebugName(name)
        .setInitialState(nvrhi::ResourceStates::ShaderResource)
        .setIsUAV(isUAV);
    return d;
}

RGBufferDesc MakeBufDesc(uint64_t byteSize, bool isUAV, const char* name)
{
    RGBufferDesc d;
    d.m_NvrhiDesc.byteSize    = byteSize;
    d.m_NvrhiDesc.canHaveUAVs = isUAV;
    d.m_NvrhiDesc.debugName   = name;
    d.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    return d;
}

// Declare a texture inside an already-open BeginSetup block, register a write
// access, and return the handle.  Caller must call EndSetup + BeginPass.
RGTextureHandle DeclareAndWriteTex(RenderGraph& rg, const RGTextureDesc& desc)
{
    RGTextureHandle h;
    rg.DeclareTexture(desc, h);
    // DeclareTexture implicitly calls WriteTexture, so no extra call needed.
    return h;
}

// Declare a buffer inside an already-open BeginSetup block.
RGBufferHandle DeclareAndWriteBuf(RenderGraph& rg, const RGBufferDesc& desc)
{
    RGBufferHandle h;
    rg.DeclareBuffer(desc, h);
    return h;
}

// Run a single minimal pass: Reset → BeginSetup → declare → BeginPass → EndSetup → Compile.
// Owns the full frame lifecycle so callers don't need to call Reset() themselves.
// Returns the allocated texture handle.
RGTextureHandle RunSingleTexPass(RenderGraph& rg, const RGTextureDesc& desc, const char* passName)
{
    rg.Reset();
    rg.BeginSetup();
    RGTextureHandle h = DeclareAndWriteTex(rg, desc);
    rg.BeginPass(passName);
    rg.EndSetup();
    rg.Compile();
    return h;
}

// Run a single minimal pass for a buffer.
// Owns the full frame lifecycle (Reset → BeginSetup → declare → BeginPass → EndSetup → Compile).
RGBufferHandle RunSingleBufPass(RenderGraph& rg, const RGBufferDesc& desc, const char* passName)
{
    rg.Reset();
    rg.BeginSetup();
    RGBufferHandle h = DeclareAndWriteBuf(rg, desc);
    rg.BeginPass(passName);
    rg.EndSetup();
    rg.Compile();
    return h;
}
