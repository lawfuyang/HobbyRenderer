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

SceneScope::SceneScope(const char* modelRelPath, bool skipCache)
{
    const std::string path = GltfSampleModel(modelRelPath);
    if (path.empty() || !std::filesystem::exists(path))
        return;

    // LoadScene() appends into the existing Scene, so drop any previous scene first.
    if (DEV())
        DEV()->waitForIdle();
    g_Renderer.m_Scene.Shutdown();

    // Temporarily override Config scene path and cache flag
    Config& cfg = const_cast<Config&>(Config::Get());
    const std::string prevPath = cfg.m_ScenePath;
    const bool        prevSkip = cfg.m_SkipCache;

    cfg.m_ScenePath = path;
    cfg.m_SkipCache = skipCache; // avoid polluting the sample-assets dir with .bin files

    g_Renderer.m_Scene.LoadScene();
    loaded = !g_Renderer.m_Scene.m_Meshes.empty() ||
        !g_Renderer.m_Scene.m_Nodes.empty();

    // Restore config
    cfg.m_ScenePath = prevPath;
    cfg.m_SkipCache = prevSkip;
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
    SceneLoader::UpdateMaterialsAndCreateConstants(g_Renderer.m_Scene);
    SceneLoader::CreateAndUploadGpuBuffers(g_Renderer.m_Scene, vertices, indices);
    SceneLoader::CreateAndUploadLightBuffer(g_Renderer.m_Scene);
    g_Renderer.m_Scene.BuildAccelerationStructures();

    for (const auto& r : g_Renderer.m_Renderers)
        if (r) r->PostSceneLoad();
    g_Renderer.ExecutePendingCommandLists();
}

MinimalSceneFixture::~MinimalSceneFixture()
{
    if (DEV())
        DEV()->waitForIdle();
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

    g_Renderer.ScheduleAndRunAllRenderers();

    // Submit to GPU and wait for completion
    g_Renderer.ExecutePendingCommandLists();

    g_Renderer.m_FrameTime = 16.6f; // assume 60 FPS for testing purposes
    ++g_Renderer.m_FrameNumber;

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
