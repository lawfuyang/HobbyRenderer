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
