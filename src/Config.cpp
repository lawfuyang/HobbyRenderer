#include "Config.h"

void Config::ParseCommandLine(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];

        if (std::strcmp(arg, "--vulkandebug") == 0)
        {
            s_Instance.m_EnableValidation = true;
            SDL_Log("[Config] Vulkan validation enabled via command line");
        }
        else if (std::strcmp(arg, "--vulkandebug-gpu-assisted") == 0)
        {
            s_Instance.m_EnableGPUAssistedValidation = true;
            SDL_Log("[Config] GPU-assisted Vulkan validation enabled via command line");
        }
        else if (std::strcmp(arg, "--d3d12") == 0)
        {
            s_Instance.m_GraphicsAPI = nvrhi::GraphicsAPI::D3D12;
            SDL_Log("[Config] D3D12 graphics API selected via command line");
        }
        else if (std::strcmp(arg, "--gltf") == 0)
        {
            if (i + 1 < argc)
            {
                s_Instance.m_GltfScene = argv[++i];
                SDL_Log("[Config] GLTF scene set via command line: %s", s_Instance.m_GltfScene.c_str());
            }
            else
            {
                SDL_Log("[Config] Missing value for --gltf");
            }
        }
        else if (std::strcmp(arg, "--skip-textures") == 0)
        {
            s_Instance.m_SkipTextures = true;
            SDL_Log("[Config] Skipping GLTF texture loading via command line");
        }
        else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0)
        {
            SDL_Log("Agentic Renderer - Command Line Options:");
            SDL_Log("  --vulkandebug                 Enable Vulkan validation layers");
            SDL_Log("  --vulkandebug-gpu-assisted    Enable GPU-assisted Vulkan validation (requires --vulkandebug)");
            SDL_Log("  --d3d12                       Use D3D12 graphics API");
            SDL_Log("  --gltf <path>                 Load the specified glTF scene file");
            SDL_Log("  --skip-textures               Skip loading textures from glTF scene");
            SDL_Log("  --help, -h                    Show this help message");
        }
        else
        {
            SDL_Log("[Config] Unknown command line argument: %s", arg);
        }
    }
}
