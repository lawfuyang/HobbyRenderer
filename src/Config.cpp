#include "Config.h"

void Config::ParseCommandLine(int argc, char* argv[])
{
    

    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];

        if (std::strcmp(arg, "--rhidebug") == 0)
        {
            s_Instance.m_EnableValidation = true;
            SDL_Log("[Config] Validation enabled via command line");
        }
        else if (std::strcmp(arg, "--rhidebug-gpu") == 0)
        {
            s_Instance.m_EnableGPUAssistedValidation = true;
            SDL_Log("[Config] GPU-assisted validation enabled via command line");
        }
        else if (std::strcmp(arg, "--scene") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_ScenePath = argv[++i];
                SDL_Log("[Config] Scene set via command line: %s", s_Instance.m_ScenePath.c_str());
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --scene", "[Config] Missing value for --scene");
            }
        }
        else if (std::strcmp(arg, "--execute-per-pass") == 0)
        {
            s_Instance.ExecutePerPass = true;
            SDL_Log("[Config] Execute per pass enabled via command line");
        }
        else if (std::strcmp(arg, "--execute-per-pass-and-wait") == 0)
        {
            s_Instance.ExecutePerPassAndWait = true;
            SDL_Log("[Config] Execute per pass and wait enabled via command line");
        }
        else if (std::strcmp(arg, "--disable-rendergraph-aliasing") == 0)
        {
            s_Instance.m_EnableRenderGraphAliasing = false;
            SDL_Log("[Config] Render graph aliasing disabled via command line");
        }
        else if (std::strcmp(arg, "--disable-sampler-feedback") == 0)
        {
            s_Instance.m_DisableSamplerFeedback = true;
            SDL_Log("[Config] Sampler feedback disabled via command line");
        }
        else if (std::strcmp(arg, "--disable-omm") == 0)
        {
            s_Instance.m_DisableOmm = true;
            SDL_Log("[Config] Opacity micromaps disabled via command line");
        }
        else if (std::strcmp(arg, "--renderdoc") == 0)
        {
            s_Instance.m_bRenderDoc = true;
            SDL_Log("[Config] RenderDoc enabled via command line");
        }
        else if (std::strncmp(arg, "--renderdoc=", 12) == 0)
        {
            s_Instance.m_bRenderDoc = true;
            s_Instance.m_RenderDocDllPath = arg + 12;
            SDL_Log("[Config] RenderDoc enabled via command line, dll: %s", s_Instance.m_RenderDocDllPath.c_str());
        }
        else if (std::strcmp(arg, "--renderdoc-capture-frame") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_bRenderDoc = true;
                s_Instance.m_RenderDocCaptureFrame = std::atoi(argv[++i]);
                SDL_Log("[Config] RenderDoc will capture frame %d and exit", s_Instance.m_RenderDocCaptureFrame);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --renderdoc-capture-frame", "[Config] Missing value for --renderdoc-capture-frame");
            }
        }
        else if (std::strcmp(arg, "--screenshot-frame") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_ScreenshotFrame = std::atoi(argv[++i]);
                SDL_Log("[Config] Screenshot will be written on frame %d and exit", s_Instance.m_ScreenshotFrame);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --screenshot-frame", "[Config] Missing value for --screenshot-frame");
            }
        }
        else if (std::strcmp(arg, "--camera-spin") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_CameraSpinDegreesPerSecond = static_cast<float>(std::atof(argv[++i]));
                SDL_Log("[Config] Camera will spin at %.1f deg/s", s_Instance.m_CameraSpinDegreesPerSecond);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --camera-spin", "[Config] Missing value for --camera-spin");
            }
        }
        else if (std::strcmp(arg, "--hitch-log") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_HitchLogThresholdMs = static_cast<float>(std::atof(argv[++i]));
                SDL_Log("[Config] Frames slower than %.1f ms will be logged", s_Instance.m_HitchLogThresholdMs);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --hitch-log", "[Config] Missing value for --hitch-log");
            }
        }
        else if (std::strcmp(arg, "--hitch-dump") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_HitchDumpThresholdMs = static_cast<float>(std::atof(argv[++i]));
                SDL_Log("[Config] Profiler will be dumped on the first frame slower than %.1f ms", s_Instance.m_HitchDumpThresholdMs);
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --hitch-dump", "[Config] Missing value for --hitch-dump");
            }
        }
        else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0)
        {
            SDL_Log("Hobby Renderer - Command Line Options:");
            SDL_Log("  --rhidebug                       Enable graphics API validation layers");
            SDL_Log("  --rhidebug-gpu                   Enable GPU-assisted validation (requires --rhidebug)");
            SDL_Log("  --execute-per-pass               Execute command lists per pass");
            SDL_Log("  --execute-per-pass-and-wait      Wait for idle after each pass execution");
            SDL_Log("  --disable-rendergraph-aliasing   Disable render graph aliasing");
            SDL_Log("  --disable-sampler-feedback       Disable sampler feedback (streaming requests the finest mip)");
            SDL_Log("  --disable-omm                    Start with opacity micromaps switched off (panel toggle)");
            SDL_Log("  --renderdoc[=<path>]             Load renderdoc.dll for in-app captures (before the device exists)");
            SDL_Log("  --renderdoc-capture-frame <n>    Capture frame n and exit (implies --renderdoc)");
            SDL_Log("  --screenshot-frame <n>           Write screenshot.jpg on frame n and exit");
            SDL_Log("  --camera-spin <deg/s>            Rotate the view automatically, for reproduction runs");
            SDL_Log("  --hitch-log <ms>                 Log every frame slower than this with streaming/VRAM state");
            SDL_Log("  --hitch-dump <ms>                Dump the profiler and exit on the first frame slower than this");
            SDL_Log("  --scene <path>                   Load the specified scene file");
            SDL_Log("  --help, -h                       Show this help message");
        }
        else
        {
            SDL_Log("[Config] Unknown command line argument: %s", arg);
        }
    }
}
