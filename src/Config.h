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

    // RenderDoc in-app capture (see RenderDocCapture.h). The DLL has to be loaded before the D3D12 device
    // is created, so this is a start-up decision rather than a runtime toggle.
    bool m_bRenderDoc = false;
    std::string m_RenderDocDllPath = ""; // empty = the usual locations (exe directory, then Program Files)
    int m_RenderDocCaptureFrame = -1;     // capture this frame number, then exit; -1 = never

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
