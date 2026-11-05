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

    // Start with opacity micromaps switched off. The panel toggle does the same thing at runtime, but the A/B
    // comparison runs have no way to press it.
    bool m_DisableOmm = false;

    // RenderDoc in-app capture (see RenderDocCapture.h). The DLL has to be loaded before the D3D12 device
    // is created, so this is a start-up decision rather than a runtime toggle.
    bool m_bRenderDoc = false;
    std::string m_RenderDocDllPath = ""; // empty = the usual locations (exe directory, then Program Files)
    int m_RenderDocCaptureFrame = -1;     // capture this frame number, then exit; -1 = never

    // Write bin/screenshot.jpg on this frame number and stop; -1 = never. Comparison runs use it so an A/B
    // pair does not depend on somebody pressing the screenshot key at the right moment.
    int m_ScreenshotFrame = -1;

    // Rotate the view by this many degrees per second. Streaming and shadow problems that only appear while the
    // camera moves need a scripted camera to be reproducible.
    float m_CameraSpinDegreesPerSecond = 0.0f;

    // Log one line per frame slower than this, with the streaming and VRAM state that frame saw, so a hitch can
    // be attributed to a subsystem after the fact. 0 = off.
    float m_HitchLogThresholdMs = 0.0f;

    // Write profiler_dump.html and stop on the first frame slower than this. The dump is what says which pass
    // absorbed the spike, and a hitch is far too rare to catch by hand. 0 = off.
    float m_HitchDumpThresholdMs = 0.0f;

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
