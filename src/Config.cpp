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
        else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0)
        {
            SDL_Log("Hobby Renderer - Command Line Options:");
            SDL_Log("  --rhidebug                       Enable graphics API validation layers");
            SDL_Log("  --rhidebug-gpu                   Enable GPU-assisted validation (requires --rhidebug)");
            SDL_Log("  --execute-per-pass               Execute command lists per pass");
            SDL_Log("  --execute-per-pass-and-wait      Wait for idle after each pass execution");
            SDL_Log("  --disable-rendergraph-aliasing   Disable render graph aliasing");
            SDL_Log("  --disable-sampler-feedback       Disable sampler feedback (streaming requests the finest mip)");
            SDL_Log("  --scene <path>                   Load the specified scene file");
            SDL_Log("  --help, -h                       Show this help message");
        }
        else
        {
            SDL_Log("[Config] Unknown command line argument: %s", arg);
        }
    }
}
