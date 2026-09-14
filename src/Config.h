#pragma once



struct Config
{
    // Graphics settings
    bool m_EnableValidation = false;
    bool m_EnableGPUAssistedValidation = false;
    // Path to a scene to load (empty = none)
    std::string m_ScenePath = "";

    bool ExecutePerPass = false;
    bool ExecutePerPassAndWait = false;

    // Enable render graph aliasing
    bool m_EnableRenderGraphAliasing = true;

    // Disable sampler feedback entirely. Needed under tools that do not implement
    // it (e.g. RenderDoc 1.46). Texture streaming then falls back to requesting
    // the most detailed mip of every texture instead of the observed mips.
    bool m_DisableSamplerFeedback = false;

    // Add more configuration options here as needed
    // int renderWidth = 1920;
    // int renderHeight = 1080;
    // float renderScale = 1.0f;

    static Config& Get() { return s_Instance; }
    static void ParseCommandLine(int argc, char* argv[]);

private:
    static Config s_Instance;
};

inline Config Config::s_Instance{};
