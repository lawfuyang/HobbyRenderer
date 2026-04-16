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
        device->executeCommandList(cmd);
        device->waitForIdle();
    }

    return tex;
}