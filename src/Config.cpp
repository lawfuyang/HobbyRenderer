#include "Config.h"

void Config::ParseCommandLine(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];

        if (std::strcmp(arg, "--gpu-validation") == 0)
        {
            s_Instance.m_EnableGPUValidation = true;
            SDL_Log("[Config] GPU validation enabled via command line");
        }
        else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0)
        {
            SDL_Log("Agentic Renderer - Command Line Options:");
            SDL_Log("  --gpu-validation         Enable Vulkan validation layers");
            SDL_Log("  --help, -h               Show this help message");
        }
        else
        {
            SDL_Log("[Config] Unknown command line argument: %s", arg);
        }
    }
}
