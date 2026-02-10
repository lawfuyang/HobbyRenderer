#include "Config.h"
#include "Renderer.h"

void Config::ParseCommandLine(int argc, char* argv[])
{
    Renderer* renderer = Renderer::GetInstance();

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
        else if (std::strcmp(arg, "--vulkan") == 0)
        {
            s_Instance.m_GraphicsAPI = nvrhi::GraphicsAPI::VULKAN;
            SDL_Log("[Config] Vulkan graphics API selected via command line");
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
        else if (std::strcmp(arg, "--skip-textures") == 0)
        {
            s_Instance.m_SkipTextures = true;
            SDL_Log("[Config] Skipping GLTF texture loading via command line");
        }
        else if (std::strcmp(arg, "--skip-cache") == 0)
        {
            s_Instance.m_SkipCache = true;
            SDL_Log("[Config] Skipping scene cache via command line");
        }
        else if (std::strcmp(arg, "--irradiance") == 0)
        {
            if (i + 1 < argc)
            {
                renderer->m_IrradianceTexture = argv[++i];
                SDL_Log("[Config] Irradiance texture set via command line: %s", renderer->m_IrradianceTexture.c_str());
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --irradiance", "[Config] Missing value for --irradiance");
            }
        }
        else if (std::strcmp(arg, "--radiance") == 0)
        {
            if (i + 1 < argc)
            {
                renderer->m_RadianceTexture = argv[++i];
                SDL_Log("[Config] Radiance texture set via command line: %s", renderer->m_RadianceTexture.c_str());
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --radiance", "[Config] Missing value for --radiance");
            }
        }
        else if (std::strcmp(arg, "--envmap") == 0)
        {
            if (i + 1 < argc)
            {
                std::filesystem::path envMapPath = argv[++i];
                std::string stem = envMapPath.stem().string();
                std::filesystem::path parent = envMapPath.parent_path();
                renderer->m_IrradianceTexture = (parent / (stem + "_irradiance.dds")).string();
                renderer->m_RadianceTexture = (parent / (stem + "_radiance.dds")).string();

                SDL_Log("[Config] Environment map set via command line: %s", envMapPath.string().c_str());
                SDL_Log("[Config] Irradiance: %s", renderer->m_IrradianceTexture.c_str());
                SDL_Log("[Config] Radiance: %s", renderer->m_RadianceTexture.c_str());

                if (!std::filesystem::exists(renderer->m_IrradianceTexture)) {
                    SDL_LOG_ASSERT_FAIL("Irradiance map not found", "Irradiance map not found: %s", renderer->m_IrradianceTexture.c_str());
                }
                if (!std::filesystem::exists(renderer->m_RadianceTexture)) {
                    SDL_LOG_ASSERT_FAIL("Radiance map not found", "Radiance map not found: %s", renderer->m_RadianceTexture.c_str());
                }
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --envmap", "[Config] Missing value for --envmap");
            }
        }
        else if (std::strcmp(arg, "--brdflut") == 0)
        {
            if (i + 1 < argc)
            {
                renderer->m_BRDFLutTexture = argv[++i];
                SDL_Log("[Config] BRDF LUT texture set via command line: %s", renderer->m_BRDFLutTexture.c_str());
                
                if (!std::filesystem::exists(renderer->m_BRDFLutTexture)) {
                    SDL_LOG_ASSERT_FAIL("BRDF LUT not found", "BRDF LUT not found: %s", renderer->m_BRDFLutTexture.c_str());
                }
            }
            else
            {
                SDL_LOG_ASSERT_FAIL("Missing value for --brdflut", "[Config] Missing value for --brdflut");
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
        else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0)
        {
            SDL_Log("Agentic Renderer - Command Line Options:");
            SDL_Log("  --rhidebug                    Enable graphics API validation layers");
            SDL_Log("  --rhidebug-gpu                Enable GPU-assisted validation (requires --rhidebug)");
            SDL_Log("  --vulkan                       Use Vulkan graphics API");
            SDL_Log("  --execute-per-pass            Execute command lists per pass");
            SDL_Log("  --execute-per-pass-and-wait   Wait for idle after each pass execution");
            SDL_Log("  --scene <path>                Load the specified scene file");
            SDL_Log("  --skip-textures               Skip loading textures from scene");
            SDL_Log("  --skip-cache                  Skip loading/saving scene cache");
            SDL_Log("  --irradiance <path>           Path to irradiance cubemap texture (DDS)");
            SDL_Log("  --radiance <path>             Path to radiance cubemap texture (DDS)");
            SDL_Log("  --envmap <path>               Path to environment map (.hdr/.exr for auto-inference of DDS)");
            SDL_Log("  --brdflut <path>              Path to BRDF LUT texture (DDS)");
            SDL_Log("  --help, -h                    Show this help message");
        }
        else
        {
            SDL_Log("[Config] Unknown command line argument: %s", arg);
        }
    }
}
