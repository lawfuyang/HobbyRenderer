#include "Renderer.h"
#include "Utilities.h"
#include "Config.h"
#include "CommonResources.h"
#include "SceneLoader.h"

#define FFX_CPU
#define FFX_STATIC static
using FfxUInt32 = uint32_t;
using FfxInt32 = int32_t;
using FfxFloat32 = float;
using FfxUInt32x2 = uint32_t[2];
using FfxUInt32x4 = uint32_t[4];
#define ffxMax(a, b) std::max(a, b)
#define ffxMin(a, b) std::min(a, b)
#include "shaders/ffx_spd.h"

// ============================================================================
// Global Render Graph Handles for Transient Resources
// ============================================================================

// Depth & GBuffer textures (transient, frame-local)
RGTextureHandle g_RG_DepthTexture;
RGTextureHandle g_RG_HZBTexture;
RGTextureHandle g_RG_GBufferAlbedo;
RGTextureHandle g_RG_GBufferNormals;
RGTextureHandle g_RG_GBufferORM;
RGTextureHandle g_RG_GBufferEmissive;
RGTextureHandle g_RG_GBufferMotionVectors;
RGTextureHandle g_RG_HDRColor;

// ============================================================================
// Renderer Implementations
// ============================================================================

class ClearRenderer : public IRenderer
{
    bool m_NeedHZBClear = false;
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        const uint32_t width = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;
        
        // Declare transient depth texture
        if (renderer->m_Mode != RenderingMode::ReferencePathTracer)
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = Renderer::DEPTH_FORMAT;
            desc.m_NvrhiDesc.isRenderTarget = true;
            desc.m_NvrhiDesc.debugName = "DepthBuffer_RG";
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::DepthWrite;
            desc.m_NvrhiDesc.keepInitialState = true;
            desc.m_NvrhiDesc.setClearValue(nvrhi::Color{ Renderer::DEPTH_FAR, 0.0f, 0.0f, 0.0f });
            
            renderGraph.DeclareTexture(desc, g_RG_DepthTexture);
        }

        // HDR Color Texture
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = Renderer::HDR_COLOR_FORMAT;
            desc.m_NvrhiDesc.debugName = "HDRColorTexture_RG";
            desc.m_NvrhiDesc.isRenderTarget = true;
            desc.m_NvrhiDesc.isUAV = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::RenderTarget;
            desc.m_NvrhiDesc.setClearValue(nvrhi::Color{});
            renderGraph.DeclareTexture(desc, g_RG_HDRColor);
        }
        
        // Declare transient GBuffer textures
        if (renderer->m_Mode != RenderingMode::ReferencePathTracer)
        {
            RGTextureDesc gbufferDesc;
            gbufferDesc.m_NvrhiDesc.width = width;
            gbufferDesc.m_NvrhiDesc.height = height;
            gbufferDesc.m_NvrhiDesc.isRenderTarget = true;
            gbufferDesc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::RenderTarget;
            gbufferDesc.m_NvrhiDesc.keepInitialState = true;
            gbufferDesc.m_NvrhiDesc.setClearValue(nvrhi::Color{});
            
            // Albedo: RGBA8
            gbufferDesc.m_NvrhiDesc.format = Renderer::GBUFFER_ALBEDO_FORMAT;
            gbufferDesc.m_NvrhiDesc.debugName = "GBufferAlbedo_RG";
            renderGraph.DeclareTexture(gbufferDesc, g_RG_GBufferAlbedo);
            
            // Normals: RG16_FLOAT
            gbufferDesc.m_NvrhiDesc.format = Renderer::GBUFFER_NORMALS_FORMAT;
            gbufferDesc.m_NvrhiDesc.debugName = "GBufferNormals_RG";
            renderGraph.DeclareTexture(gbufferDesc, g_RG_GBufferNormals);
            
            // ORM: RGBA8
            gbufferDesc.m_NvrhiDesc.format = Renderer::GBUFFER_ORM_FORMAT;
            gbufferDesc.m_NvrhiDesc.debugName = "GBufferORM_RG";
            renderGraph.DeclareTexture(gbufferDesc, g_RG_GBufferORM);
            
            // Emissive: RGBA8
            gbufferDesc.m_NvrhiDesc.format = Renderer::GBUFFER_EMISSIVE_FORMAT;
            gbufferDesc.m_NvrhiDesc.debugName = "GBufferEmissive_RG";
            renderGraph.DeclareTexture(gbufferDesc, g_RG_GBufferEmissive);
            
            // Motion Vectors: RG16_FLOAT
            gbufferDesc.m_NvrhiDesc.format = Renderer::GBUFFER_MOTION_FORMAT;
            gbufferDesc.m_NvrhiDesc.debugName = "GBufferMotion_RG";
            renderGraph.DeclareTexture(gbufferDesc, g_RG_GBufferMotionVectors);
        }

        // HZB Texture (Persistent)
        if (renderer->m_Mode != RenderingMode::ReferencePathTracer)
        {
            uint32_t sw = width;
            uint32_t sh = height;
            uint32_t hzbWidth = NextLowerPow2(sw);
            uint32_t hzbHeight = NextLowerPow2(sh);
            hzbWidth = hzbWidth > sw ? hzbWidth >> 1 : hzbWidth;
            hzbHeight = hzbHeight > sh ? hzbHeight >> 1 : hzbHeight;
            uint32_t maxDim = std::max(hzbWidth, hzbHeight);
            uint32_t mipLevels = 0;
            while (maxDim > 0) {
                mipLevels++;
                maxDim >>= 1;
            }

            RGTextureDesc hzbDesc;
            hzbDesc.m_NvrhiDesc.width = hzbWidth;
            hzbDesc.m_NvrhiDesc.height = hzbHeight;
            hzbDesc.m_NvrhiDesc.mipLevels = mipLevels;
            hzbDesc.m_NvrhiDesc.format = nvrhi::Format::R32_FLOAT;
            hzbDesc.m_NvrhiDesc.debugName = "HZB";
            hzbDesc.m_NvrhiDesc.isUAV = true;
            hzbDesc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            
            m_NeedHZBClear = renderGraph.DeclarePersistentTexture(hzbDesc, g_RG_HZBTexture);
        }

        return true;
    }
    
    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();

        // Get transient resources from render graph
        nvrhi::TextureHandle hdrColor = renderGraph.GetTexture(g_RG_HDRColor, RGResourceAccessMode::Write);
        commandList->clearTextureFloat(hdrColor, nvrhi::AllSubresources, nvrhi::Color{});

        if (renderer->m_Mode != RenderingMode::ReferencePathTracer)
        {
            nvrhi::TextureHandle depthTexture = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Write);
            nvrhi::TextureHandle gbufferAlbedo = renderGraph.GetTexture(g_RG_GBufferAlbedo, RGResourceAccessMode::Write);
            nvrhi::TextureHandle gbufferNormals = renderGraph.GetTexture(g_RG_GBufferNormals, RGResourceAccessMode::Write);
            nvrhi::TextureHandle gbufferORM = renderGraph.GetTexture(g_RG_GBufferORM, RGResourceAccessMode::Write);
            nvrhi::TextureHandle gbufferEmissive = renderGraph.GetTexture(g_RG_GBufferEmissive, RGResourceAccessMode::Write);
            nvrhi::TextureHandle gbufferMotion = renderGraph.GetTexture(g_RG_GBufferMotionVectors, RGResourceAccessMode::Write);

            // Clear depth for reversed-Z (clear to 0.0f) and stencil to 0
            commandList->clearDepthStencilTexture(depthTexture, nvrhi::AllSubresources, true, Renderer::DEPTH_FAR, true, 0);

            // clear gbuffers
            commandList->clearTextureFloat(gbufferAlbedo, nvrhi::AllSubresources, nvrhi::Color{});
            commandList->clearTextureFloat(gbufferNormals, nvrhi::AllSubresources, nvrhi::Color{});
            commandList->clearTextureFloat(gbufferORM, nvrhi::AllSubresources, nvrhi::Color{});
            commandList->clearTextureFloat(gbufferEmissive, nvrhi::AllSubresources, nvrhi::Color{});
            commandList->clearTextureFloat(gbufferMotion, nvrhi::AllSubresources, nvrhi::Color{});

            if (m_NeedHZBClear)
            {
                nvrhi::TextureHandle hzbTexture = renderGraph.GetTexture(g_RG_HZBTexture, RGResourceAccessMode::Write);
                commandList->clearTextureFloat(hzbTexture, nvrhi::AllSubresources, nvrhi::Color{ Renderer::DEPTH_FAR, 0.0f, 0.0f, 0.0f });
            }
        }
    }
    const char* GetName() const override { return "Clear"; }
};

class TLASRenderer : public IRenderer
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        Scene& scene = renderer->m_Scene;

        if (!scene.m_TLAS || !scene.m_RTInstanceDescBuffer || scene.m_RTInstanceDescs.empty())
            return false;

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        Scene& scene = renderer->m_Scene;

        // Perform GPU TLAS update
        commandList->buildTopLevelAccelStructFromBuffer(
            scene.m_TLAS,
            scene.m_RTInstanceDescBuffer,
            0, // offset
            scene.m_RTInstanceDescs.size()
        );
    }

    const char* GetName() const override { return "TLAS Update"; }
};

REGISTER_RENDERER(ClearRenderer);
REGISTER_RENDERER(TLASRenderer);

namespace
{
    // Read a binary file into memory
    std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
    {
        std::ifstream file{path, std::ios::binary | std::ios::ate};
        if (!file.is_open())
        {
            SDL_Log("[Shader] Failed to open file: %s", path.string().c_str());
            return {};
        }

        const std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> buffer(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(buffer.data()), size))
        {
            SDL_Log("[Shader] Failed to read file: %s", path.string().c_str());
            return {};
        }

        SDL_Log("[Shader] Loaded %zu bytes from %s", buffer.size(), path.string().c_str());
        return buffer;
    }

    // Parse a single line from shaders.cfg and extract shader metadata
    struct ShaderMetadata
    {
        std::filesystem::path sourcePath;
        std::string entryPoint;
        std::string suffix;
        std::vector<std::string> defines;
        nvrhi::ShaderType shaderType = nvrhi::ShaderType::None;
    };

    uint32_t HashToUint(size_t hash)
    {
        return uint32_t(hash ^ (hash >> 32));
    }

    // ShaderMake naming: <sourcefile_without_ext>_<EntryPoint><Suffix>[_<Hash>].spirv
    std::filesystem::path GetShaderOutputPath(const std::filesystem::path& exeDir, const ShaderMetadata& metadata, nvrhi::GraphicsAPI api)
    {
        std::filesystem::path filename = metadata.sourcePath.stem();
        std::string outName = filename.string() + "_" + metadata.entryPoint + metadata.suffix;

        if (!metadata.defines.empty())
        {
            std::vector<std::string> sortedDefines = metadata.defines;
            std::stable_sort(sortedDefines.begin(), sortedDefines.end());

            std::string combinedDefines;
            for (const std::string& d : sortedDefines)
            {
                if (!combinedDefines.empty()) combinedDefines += " ";
                combinedDefines += d;
            }

            const size_t hash = std::hash<std::string>()(combinedDefines);
            const uint32_t permutationHash = HashToUint(hash);

            std::stringstream ss;
            ss << "_" << std::uppercase << std::setfill('0') << std::setw(8) << std::hex << permutationHash;
            outName += ss.str();
        }

        std::string subDir = "spirv";
        std::string extension = ".spirv";

        if (api == nvrhi::GraphicsAPI::D3D12)
        {
            subDir = "dxil";
            extension = ".dxil";
        }

        return exeDir / "shaders" / subDir / (outName + extension);
    }

    // Parse shaders.cfg to extract shader entries
    std::vector<ShaderMetadata> ParseShaderConfig(std::string_view configPath)
    {
        std::vector<ShaderMetadata> shaders;
        std::ifstream configFile{std::filesystem::path{configPath}};

        if (!configFile.is_open())
        {
            SDL_LOG_ASSERT_FAIL("Failed to open shader config", "[Shader] Failed to open shader config: %.*s", static_cast<int>(configPath.size()), configPath.data());
            return shaders;
        }

        std::string line;
        while (std::getline(configFile, line))
        {
            // Skip empty lines and comments
            if (line.empty() || line[0] == '/' || line[0] == '#')
                continue;

            // Trim leading/trailing whitespace
            const size_t start = line.find_first_not_of(" \t\r\n");
            const size_t end = line.find_last_not_of(" \t\r\n");
            if (start == std::string::npos)
                continue;

            line = line.substr(start, end - start + 1);

            // Parse: <shader_path> -T <profile> -E <entry> [other options]
            std::istringstream iss{line};
            std::string token;
            ShaderMetadata metadata;

            iss >> token;
            metadata.sourcePath = std::filesystem::path{token};

            while (iss >> token)
            {
                if (token == "-T" || token == "--profile")
                {
                    iss >> token;
                    if (token.find("vs") != std::string::npos)
                        metadata.shaderType = nvrhi::ShaderType::Vertex;
                    else if (token.find("ps") != std::string::npos)
                        metadata.shaderType = nvrhi::ShaderType::Pixel;
                    else if (token.find("gs") != std::string::npos)
                        metadata.shaderType = nvrhi::ShaderType::Geometry;
                    else if (token.find("cs") != std::string::npos)
                        metadata.shaderType = nvrhi::ShaderType::Compute;
                    else if (token.find("hs") != std::string::npos)
                        metadata.shaderType = nvrhi::ShaderType::Hull;
                    else if (token.find("ds") != std::string::npos)
                        metadata.shaderType = nvrhi::ShaderType::Domain;
                    else if (token.find("as") != std::string::npos)
                        metadata.shaderType = nvrhi::ShaderType::Amplification;
                    else if (token.find("ms") != std::string::npos)
                        metadata.shaderType = nvrhi::ShaderType::Mesh;
                }
                else if (token == "-E" || token == "--entryPoint")
                {
                    iss >> metadata.entryPoint;
                }
                else if (token == "-s" || token == "--outputSuffix")
                {
                    iss >> metadata.suffix;
                }
                else if (token == "-D" || token == "--define")
                {
                    iss >> token;
                    metadata.defines.push_back(token);
                }
            }

            SDL_assert(metadata.shaderType != nvrhi::ShaderType::None && !metadata.entryPoint.empty() && "Failed to parse shader entry from config");

            shaders.push_back(metadata);

            const char* typeStr = "Unknown";
            switch (metadata.shaderType)
            {
            case nvrhi::ShaderType::Vertex:        typeStr = "VS"; break;
            case nvrhi::ShaderType::Pixel:         typeStr = "PS"; break;
            case nvrhi::ShaderType::Geometry:      typeStr = "GS"; break;
            case nvrhi::ShaderType::Compute:       typeStr = "CS"; break;
            case nvrhi::ShaderType::Hull:          typeStr = "HS"; break;
            case nvrhi::ShaderType::Domain:        typeStr = "DS"; break;
            case nvrhi::ShaderType::Amplification: typeStr = "AS"; break;
            case nvrhi::ShaderType::Mesh:          typeStr = "MS"; break;
            default: break;
            }

            SDL_Log("[Shader] Parsed: %s (%s) -> entry: %s", metadata.sourcePath.generic_string().c_str(),
                typeStr, metadata.entryPoint.c_str());
        }

        SDL_Log("[Shader] Parsed %zu shader entries from config", shaders.size());
        return shaders;
    }

    void InitSDL()
    {
        SDL_Log("[Init] Starting SDL initialization");
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            SDL_LOG_ASSERT_FAIL("SDL_Init failed", "SDL_Init failed: %s", SDL_GetError());
            return;
        }
        SDL_Log("[Init] SDL initialized");
    }

    void ChooseWindowSize(int* outWidth, int* outHeight)
    {
        int windowW = 1280;
        int windowH = 720;

        const SDL_DisplayID primaryDisplay = SDL_GetPrimaryDisplay();
        SDL_Rect usableBounds{};
        if (!SDL_GetDisplayUsableBounds(primaryDisplay, &usableBounds))
        {
            SDL_LOG_ASSERT_FAIL("SDL_GetDisplayUsableBounds failed", "SDL_GetDisplayUsableBounds failed: %s", SDL_GetError());
            *outWidth = windowW;
            *outHeight = windowH;
            return;
        }

        int maxFitW = usableBounds.w;
        int maxFitH = usableBounds.h;
        if (static_cast<int64_t>(maxFitW) * 9 > static_cast<int64_t>(maxFitH) * 16)
        {
            maxFitW = (maxFitH * 16) / 9;
        }
        else
        {
            maxFitH = (maxFitW * 9) / 16;
        }

        constexpr int kStandard16x9[][2] = {
            {3840, 2160},
            {2560, 1440},
            {1920, 1080},
            {1600, 900},
            {1280, 720},
        };
        constexpr int kStandard16x9Count = static_cast<int>(sizeof(kStandard16x9) / sizeof(kStandard16x9[0]));

        windowW = maxFitW;
        windowH = maxFitH;
        int firstFitIndex = -1;
        for (int i = 0; i < kStandard16x9Count; ++i)
        {
            if (kStandard16x9[i][0] <= maxFitW && kStandard16x9[i][1] <= maxFitH)
            {
                firstFitIndex = i;
                break;
            }
        }

        if (firstFitIndex >= 0)
        {
            int chosenIndex = firstFitIndex;
            const bool fillsUsableWidth  = kStandard16x9[firstFitIndex][0] == maxFitW;
            const bool fillsUsableHeight = kStandard16x9[firstFitIndex][1] == maxFitH;
            if (fillsUsableWidth && fillsUsableHeight && firstFitIndex + 1 < kStandard16x9Count)
            {
                chosenIndex = firstFitIndex + 1;
            }

            windowW = kStandard16x9[chosenIndex][0];
            windowH = kStandard16x9[chosenIndex][1];
        }

        SDL_Log("[Init] Usable bounds: %dx%d, max 16:9 fit: %dx%d, chosen: %dx%d", usableBounds.w, usableBounds.h, maxFitW, maxFitH, windowW, windowH);
        *outWidth = windowW;
        *outHeight = windowH;
    }

    SDL_Window* CreateWindowScaled()
    {
        int windowW = 0;
        int windowH = 0;
        ChooseWindowSize(&windowW, &windowH);

        SDL_Log("[Init] Creating window");
        SDL_Window* window = SDL_CreateWindow("Agentic Renderer", windowW, windowH, SDL_WINDOW_VULKAN);
        if (!window)
        {
            SDL_LOG_ASSERT_FAIL("SDL_CreateWindow failed", "SDL_CreateWindow failed: %s", SDL_GetError());
            return nullptr;
        }

        SDL_Log("[Init] Window created (%dx%d)", windowW, windowH);
        return window;
    }
}

Renderer* Renderer::s_Instance = nullptr;

void Renderer::SetInstance(Renderer* instance)
{
    s_Instance = instance;
}

Renderer* Renderer::GetInstance()
{
    SDL_assert(s_Instance && "Renderer instance is not set");
    return s_Instance;
}

void Renderer::LoadShaders()
{
    SDL_Log("[Init] Loading compiled shaders from config");

    // Get executable directory
    const char* basePathCStr = SDL_GetBasePath();
    if (!basePathCStr)
    {
        SDL_Log("[Init] Failed to get base path");
        return;
    }
    const std::filesystem::path exeDir = basePathCStr;

    // Parse shaders.cfg to get list of shaders to load
    const std::filesystem::path configPath = exeDir / ".." / "src" / "shaders" / "shaders.cfg";
    std::vector<ShaderMetadata> shaderMetadata = ParseShaderConfig(configPath.generic_string());
    if (shaderMetadata.empty())
    {
        SDL_Log("[Init] No shaders to load from config");
        return; // Not an error, just no shaders defined yet
    }

    // Load each shader
    for (const ShaderMetadata& metadata : shaderMetadata)
    {
        // Determine output filename based on ShaderMake naming convention
        const std::filesystem::path outputPath = GetShaderOutputPath(exeDir, metadata, m_RHI->GetGraphicsAPI());

        // Read the compiled binary
        const std::vector<uint8_t> binary = ReadBinaryFile(outputPath);
        if (binary.empty())
        {
            SDL_Log("[Init] Failed to load compiled shader: %s", outputPath.generic_string().c_str());
            return;
        }

        // Create shader descriptor
        nvrhi::ShaderDesc desc;
        desc.shaderType = metadata.shaderType;
        desc.entryName = metadata.entryPoint;
        desc.debugName = outputPath.generic_string();

        // Create shader handle
        const nvrhi::ShaderHandle handle = m_RHI->m_NvrhiDevice->createShader(desc, binary.data(), binary.size());
        if (!handle)
        {
            SDL_Log("[Init] Failed to create shader handle: %s", outputPath.generic_string().c_str());
            return;
        }

        // Keyed by logical name (e.g., "ForwardLighting_PSMain_AlphaTest") for easy retrieval
        const std::string key = metadata.sourcePath.stem().string() + "_" + metadata.entryPoint + metadata.suffix;
        m_ShaderCache[key] = handle;
        SDL_Log("[Init] Loaded shader: %s (key=%s)", outputPath.generic_string().c_str(), key.c_str());
    }

    SDL_Log("[Init] All %zu shader(s) loaded successfully", shaderMetadata.size());
}

void Renderer::UnloadShaders()
{
    SDL_Log("[Shutdown] Unloading shaders");
    m_ShaderCache.clear();
    SDL_Log("[Shutdown] Shaders unloaded");
}

void Renderer::ReloadShaders()
{
    SDL_Log("[HotReload] Reloading shaders requested...");

    // Wait for device to be idle before destroying resources that might be in use
    m_RHI->m_NvrhiDevice->waitForIdle();

    // Clear all caches
    m_GraphicsPipelineCache.clear();
    m_MeshletPipelineCache.clear();
    m_ComputePipelineCache.clear();
    m_BindingLayoutCache.clear();
    m_ShaderCache.clear();

    SDL_Log("[HotReload] Caches cleared. Re-loading shaders from disk...");

    LoadShaders();

    SDL_Log("[HotReload] Shader reload complete.");
    m_RequestedShaderReload = false;
}

nvrhi::ShaderHandle Renderer::GetShaderHandle(std::string_view name) const
{
    std::unordered_map<std::string, nvrhi::ShaderHandle>::const_iterator it = m_ShaderCache.find(name.data());
    if (it != m_ShaderCache.end())
        return it->second;
    
    SDL_LOG_ASSERT_FAIL("Requested shader not found in cache", "[Error] Shader '%s' not found in cache!", name.data());
    return {};
}

nvrhi::TextureHandle Renderer::GetCurrentBackBufferTexture() const
{
    return m_RHI->m_NvrhiSwapchainTextures[m_SwapChainImageIdx];
}

void Renderer::Initialize()
{
    ScopedTimerLog initScope{"[Timing] Init phase:"};

    MicroProfileOnThreadCreate("Main");
	MicroProfileSetEnableAllGroups(true);
	MicroProfileSetForceMetaCounters(true);

    m_TaskScheduler = std::make_unique<TaskScheduler>();

    InitSDL();

    m_Window = CreateWindowScaled();
    if (!m_Window)
    {
        SDL_Quit();
        return;
    }

    m_RHI = CreateGraphicRHI(Config::Get().m_GraphicsAPI);
    m_RHI->Initialize(m_Window);

    SDL_assert(m_RHI->m_NvrhiDevice && "NVRHI device is null after RHI initialization");
    SDL_assert(m_RHI->m_NvrhiDevice->queryFeatureSupport(nvrhi::Feature::HeapDirectlyIndexed));
    SDL_assert(m_RHI->m_NvrhiDevice->queryFeatureSupport(nvrhi::Feature::Meshlets));
    SDL_assert(m_RHI->m_NvrhiDevice->queryFeatureSupport(nvrhi::Feature::RayQuery));
    SDL_assert(m_RHI->m_NvrhiDevice->queryFeatureSupport(nvrhi::Feature::RayTracingAccelStruct));

    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);

    if (!m_RHI->CreateSwapchain(static_cast<uint32_t>(windowWidth), static_cast<uint32_t>(windowHeight)))
    {
        Shutdown();
        return;
    }

    InitializeGlobalBindlessTextures();
    InitializeGlobalBindlessSamplers();
    CommonResources::GetInstance().Initialize();
    CommonResources::GetInstance().RegisterDefaultTextures();
    LoadShaders();

    m_ImGuiLayer.Initialize();

    // Initialize renderers now that shaders and device are ready
    for (const RendererRegistry::Creator& creator : RendererRegistry::GetCreators())
    {
        std::shared_ptr<IRenderer> renderer = creator();
        renderer->Initialize();
        
        m_Renderers.push_back(renderer);
        renderer->m_GPUQueries[0] = m_RHI->m_NvrhiDevice->createTimerQuery();
        renderer->m_GPUQueries[1] = m_RHI->m_NvrhiDevice->createTimerQuery();
    }

    m_GPUQueries[0] = m_RHI->m_NvrhiDevice->createTimerQuery();
    m_GPUQueries[1] = m_RHI->m_NvrhiDevice->createTimerQuery();

    // Load scene (if configured) after all renderer resources are ready
    m_TaskScheduler->SetThreadCount(std::thread::hardware_concurrency());
    m_Scene.LoadScene();
    m_TaskScheduler->SetThreadCount(TaskScheduler::kRuntimeThreadCount);

    // Initialize renderers with scene-dependent resources
    for (const std::shared_ptr<IRenderer>& renderer : m_Renderers)
    {
        renderer->PostSceneLoad();
    }

    ExecutePendingCommandLists();
}

void Renderer::Run()
{
    ScopedTimerLog runScope{"[Timing] Run phase:"};

    SDL_Log("[Run ] Entering main loop");

    while (m_Running)
    {
        PROFILE_SCOPED("Frame");
        const uint64_t frameStart = SDL_GetTicksNS();
        const uint32_t kFrameDurationNs = SDL_NS_PER_SECOND / m_TargetFPS;

        {
            PROFILE_SCOPED("Event Polling");

            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                m_ImGuiLayer.ProcessEvent(event);
                m_Camera.ProcessEvent(event);

                if (event.type == SDL_EVENT_QUIT)
                {
                    SDL_Log("[Run ] Received quit event");
                    m_Running = false;
                    break;
                }

                if (event.type == SDL_EVENT_KEY_DOWN)
                {
                    if (event.key.scancode == SDL_SCANCODE_F5)
                    {
                        m_RequestedShaderReload = true;
                    }
                }
            }
        };

        if (m_RequestedShaderReload)
        {
            ReloadShaders();
        }

        bool bSwapChainImageAcquireSuccess = true;

        // Vulkan's implementation can be particularly heavy...
        if (m_RHI->GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
        {
            m_TaskScheduler->ScheduleTask([this, &bSwapChainImageAcquireSuccess]()
            {
                bSwapChainImageAcquireSuccess = m_RHI->AcquireNextSwapchainImage(&m_AcquiredSwapchainImageIdx);
            });
        }
        else
        {
            bSwapChainImageAcquireSuccess = m_RHI->AcquireNextSwapchainImage(&m_AcquiredSwapchainImageIdx);
        }

        m_TaskScheduler->ScheduleTask([this]() {
            PROFILE_SCOPED("Garbage Collection");
            m_RHI->m_NvrhiDevice->runGarbageCollection();
        });

        // Prepare ImGui UI (NewFrame + UI creation + ImGui::Render)
        m_ImGuiLayer.UpdateFrame();

        // Update animations
        if (m_EnableAnimations)
        {
            PROFILE_SCOPED("Update Animations");

            m_Scene.Update(static_cast<float>(m_FrameTime / 1000.0));
            if (m_Scene.m_InstanceDirtyRange.first <= m_Scene.m_InstanceDirtyRange.second)
            {
                nvrhi::CommandListHandle cmd = AcquireCommandList();
                ScopedCommandList scopedCmd{ cmd, "Upload Animated Instances" };
                uint32_t startIdx = m_Scene.m_InstanceDirtyRange.first;
                uint32_t count = m_Scene.m_InstanceDirtyRange.second - startIdx + 1;
                scopedCmd->writeBuffer(m_Scene.m_InstanceDataBuffer,
                    &m_Scene.m_InstanceData[startIdx],
                    count * sizeof(PerInstanceData),
                    startIdx * sizeof(PerInstanceData));

                if (m_Scene.m_RTInstanceDescBuffer)
                {
                    scopedCmd->writeBuffer(m_Scene.m_RTInstanceDescBuffer,
                        &m_Scene.m_RTInstanceDescs[startIdx],
                        count * sizeof(nvrhi::rt::InstanceDesc),
                        startIdx * sizeof(nvrhi::rt::InstanceDesc));
                }
            }
        }

        if (m_Scene.m_LightsDirty)
        {
            SceneLoader::CreateAndUploadLightBuffer(m_Scene, this);
            m_Scene.m_LightsDirty = false;
        }

        // Update camera (camera retrieves frame time internally)
        m_ViewPrev = m_View;
        m_Camera.Update();

        int windowW, windowH;
        SDL_GetWindowSize(m_Window, &windowW, &windowH);
        m_Camera.FillPlanarViewConstants(m_View, (float)windowW, (float)windowH);

        const int readIndex = m_FrameNumber % 2;
        const int writeIndex = (m_FrameNumber + 1) % 2;

        // GPU query for frame timer is super expensive on the CPU for some reason. i give up using it
        if constexpr (false)
        {
            PROFILE_SCOPED("GPU Frame Start");
            nvrhi::CommandListHandle cmd = AcquireCommandList();
            ScopedCommandList scopedCmd{ cmd, "GPU Frame Start" };
            m_RHI->m_NvrhiDevice->resetTimerQuery(m_GPUQueries[readIndex]);
            scopedCmd->beginTimerQuery(m_GPUQueries[writeIndex]);
        }
        
        m_RenderGraph.Reset();

        extern IRenderer* g_TLASRenderer;
        extern IRenderer* g_ClearRenderer;
        extern IRenderer* g_OpaquePhase1Renderer;
        extern IRenderer* g_HZBGenerator;
        extern IRenderer* g_OpaquePhase2Renderer;
        extern IRenderer* g_MaskedPassRenderer;
        extern IRenderer* g_HZBGeneratorPhase2;
        extern IRenderer* g_DeferredRenderer;
        extern IRenderer* g_SkyRenderer;
        extern IRenderer* g_TransparentPassRenderer;
        extern IRenderer* g_BloomRenderer;
        extern IRenderer* g_HDRRenderer;
        extern IRenderer* g_ImGuiRenderer;
        extern IRenderer* g_PathTracerRenderer;

        m_RenderGraph.ScheduleRenderer(g_TLASRenderer);
        m_RenderGraph.ScheduleRenderer(g_ClearRenderer);

        if (m_Mode == RenderingMode::ReferencePathTracer)
        {
            m_RenderGraph.ScheduleRenderer(g_PathTracerRenderer);
        }
        else
        {
            m_RenderGraph.ScheduleRenderer(g_OpaquePhase1Renderer);
            m_RenderGraph.ScheduleRenderer(g_HZBGenerator);
            m_RenderGraph.ScheduleRenderer(g_OpaquePhase2Renderer);
            m_RenderGraph.ScheduleRenderer(g_MaskedPassRenderer);
            m_RenderGraph.ScheduleRenderer(g_HZBGeneratorPhase2);
            m_RenderGraph.ScheduleRenderer(g_DeferredRenderer);
            m_RenderGraph.ScheduleRenderer(g_SkyRenderer);
            m_RenderGraph.ScheduleRenderer(g_TransparentPassRenderer);
            m_RenderGraph.ScheduleRenderer(g_BloomRenderer);
        }

        m_RenderGraph.ScheduleRenderer(g_HDRRenderer);
        m_RenderGraph.ScheduleRenderer(g_ImGuiRenderer);

        // Compile render graph: compute lifetimes and allocate resources
        m_RenderGraph.Compile();

        // Wait for all render passes to finish recording
        m_TaskScheduler->ExecuteAllScheduledTasks();

        // GPU query for frame timer is super expensive on the CPU for some reason. i give up using it
        if constexpr (false)
        {
            PROFILE_SCOPED("GPU Frame End");
            nvrhi::CommandListHandle cmd = AcquireCommandList();
            ScopedCommandList scopedCmd{ cmd, "GPU Frame End" };
            scopedCmd->endTimerQuery(m_GPUQueries[writeIndex]);
        }

        // Execute any queued GPU work in submission order
        ExecutePendingCommandLists();

        // Present swapchain
        SDL_assert(bSwapChainImageAcquireSuccess);
        SDL_assert(m_SwapChainImageIdx == m_SwapChainImageIdx);
        if (!m_RHI->PresentSwapchain(m_SwapChainImageIdx))
        {
            SDL_LOG_ASSERT_FAIL("PresentSwapchain failed", "[Run ] PresentSwapchain failed");
            break;
        }
        m_SwapChainImageIdx = 1 - m_SwapChainImageIdx;

        const uint64_t workTimeNs = SDL_GetTicksNS() - frameStart;

        // Sleep to maintain target framerate (if needed)
        if (workTimeNs < kFrameDurationNs)
        {
            PROFILE_SCOPED("Sleep");
            SDL_Delay(static_cast<uint32_t>(SDL_NS_TO_MS(kFrameDurationNs  - workTimeNs)));
        }

        // Recompute total frame time (including any sleep) so reported FPS matches ImGui's DeltaTime
        const uint64_t totalFrameTime = SDL_GetTicksNS() - frameStart;

        // Calculate frame time (ms) and FPS
        m_FrameTime = SDL_NS_TO_MS(static_cast<double>(totalFrameTime));
        if (m_FrameTime > 0.0)
            m_FPS = 1000.0 / m_FrameTime;

        // Increment frame number for double buffering
        m_FrameNumber++;

        MicroProfileFlip(nullptr);
    }
}

void Renderer::Shutdown()
{
    ScopedTimerLog shutdownScope{"[Timing] Shutdown phase:"};

    MicroProfileShutdown();

    m_RHI->m_NvrhiDevice->waitForIdle();
    m_RHI->m_NvrhiDevice->runGarbageCollection();

    m_RenderGraph.Shutdown();

    m_InFlightCommandLists.clear();
    m_PendingCommandLists.clear();
    m_CommandListFreeList.clear();

    m_ImGuiLayer.Shutdown();
    CommonResources::GetInstance().Shutdown();

    // Shutdown global bindless systems
    m_GlobalTextureDescriptorTable = nullptr;
    m_GlobalTextureBindingLayout = nullptr;
    m_NextTextureIndex = 0;

    m_GlobalSamplerDescriptorTable = nullptr;
    m_GlobalSamplerBindingLayout = nullptr;

    // Shutdown scene and free its GPU resources
    m_Scene.Shutdown();

    // Free renderer instances
    m_Renderers.clear();

    m_GPUQueries[0] = nullptr;
    m_GPUQueries[1] = nullptr;

    m_BindingLayoutCache.clear();
    m_GraphicsPipelineCache.clear();
    m_ComputePipelineCache.clear();
    m_MeshletPipelineCache.clear();

    UnloadShaders();

    m_RHI->m_NvrhiDevice->waitForIdle();
    m_RHI->m_NvrhiDevice->runGarbageCollection();

    m_RHI->Shutdown();

    if (m_Window)
    {
        SDL_DestroyWindow(m_Window);
        m_Window = nullptr;
    }

    SDL_Quit();
    SDL_Log("[Shutdown] Clean exit");
}

void Renderer::SetCameraFromSceneCamera(const Scene::Camera& sceneCam)
{
    if (sceneCam.m_NodeIndex >= 0 && sceneCam.m_NodeIndex < static_cast<int>(m_Scene.m_Nodes.size()))
    {
        const Matrix& worldTransform = m_Scene.m_Nodes[sceneCam.m_NodeIndex].m_WorldTransform;
        m_Camera.SetFromMatrix(worldTransform);
        m_Camera.SetProjection(sceneCam.m_Projection);

        m_Camera.m_ExposureValue = sceneCam.m_ExposureValue;
        m_Camera.m_ExposureCompensation = sceneCam.m_ExposureCompensation;
        m_Camera.m_ExposureValueMin = sceneCam.m_ExposureValueMin;
        m_Camera.m_ExposureValueMax = sceneCam.m_ExposureValueMax;
    }
}

// Hash helper for BindingLayoutDesc
static size_t HashBindingLayoutDesc(const nvrhi::BindingLayoutDesc& d)
{
    size_t h = std::hash<uint32_t>()(static_cast<uint32_t>(d.visibility));
    h = h * 1315423911u + std::hash<uint32_t>()(d.registerSpace);
    h = h * 1315423911u + std::hash<uint32_t>()(d.registerSpaceIsDescriptorSet ? 1u : 0u);
    h = h * 1315423911u + std::hash<uint32_t>()(d.bindingOffsets.shaderResource);
    h = h * 1315423911u + std::hash<uint32_t>()(d.bindingOffsets.sampler);
    h = h * 1315423911u + std::hash<uint32_t>()(d.bindingOffsets.constantBuffer);
    h = h * 1315423911u + std::hash<uint32_t>()(d.bindingOffsets.unorderedAccess);
    for (const nvrhi::BindingLayoutItem& it : d.bindings)
    {
        // combine slot, type and size
        uint64_t v = (uint64_t(it.slot) << 32) | (uint64_t(it.type) << 16) | uint64_t(it.size);
        h = h * 1315423911u + std::hash<uint64_t>()(v);
    }
    return h;
}

static size_t HashBindlessLayoutDesc(const nvrhi::BindlessLayoutDesc& d)
{
    size_t h = std::hash<uint32_t>()(static_cast<uint32_t>(d.visibility));
    h = h * 1315423911u + std::hash<uint32_t>()(d.firstSlot);
    h = h * 1315423911u + std::hash<uint32_t>()(d.maxCapacity);
    h = h * 1315423911u + std::hash<uint32_t>()(static_cast<uint32_t>(d.layoutType));
    for (const nvrhi::BindingLayoutItem& it : d.registerSpaces)
    {
        // combine slot, type and size
        uint64_t v = (uint64_t(it.slot) << 32) | (uint64_t(it.type) << 16) | uint64_t(it.size);
        h = h * 1315423911u + std::hash<uint64_t>()(v);
    }
    return h;
}

static bool BindingLayoutDescEqual(const nvrhi::BindingLayoutDesc& a, const nvrhi::BindingLayoutDesc& b)
{
    if (a.visibility != b.visibility) return false;
    if (a.registerSpace != b.registerSpace) return false;
    if (a.registerSpaceIsDescriptorSet != b.registerSpaceIsDescriptorSet) return false;
    if (a.bindingOffsets.shaderResource != b.bindingOffsets.shaderResource) return false;
    if (a.bindingOffsets.sampler != b.bindingOffsets.sampler) return false;
    if (a.bindingOffsets.constantBuffer != b.bindingOffsets.constantBuffer) return false;
    if (a.bindingOffsets.unorderedAccess != b.bindingOffsets.unorderedAccess) return false;
    if (a.bindings.size() != b.bindings.size()) return false;
    for (size_t i = 0; i < a.bindings.size(); ++i)
    {
        if (a.bindings[i] != b.bindings[i])
            return false;
    }
    return true;
}

nvrhi::BindingLayoutHandle Renderer::GetOrCreateBindingLayoutFromBindingSetDesc(const nvrhi::BindingSetDesc& setDesc, uint32_t registerSpace)
{
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.registerSpace = registerSpace;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets.shaderResource = SPIRV_TEXTURE_SHIFT;
    layoutDesc.bindingOffsets.sampler = SPIRV_SAMPLER_SHIFT;
    layoutDesc.bindingOffsets.constantBuffer = SPIRV_CBUFFER_SHIFT;
    layoutDesc.bindingOffsets.unorderedAccess = SPIRV_UAV_SHIFT;

    for (const nvrhi::BindingSetItem& item : setDesc.bindings)
    {
        nvrhi::BindingLayoutItem b{};
        b.slot = item.slot;
        b.type = item.type;
        if (item.type == nvrhi::ResourceType::PushConstants)
            b.size = uint16_t(item.range.byteSize);
        else
            b.size = 1;
        layoutDesc.bindings.push_back(b);
    }

    // Sort deterministically: slot then type
    std::sort(layoutDesc.bindings.begin(), layoutDesc.bindings.end(), [](const nvrhi::BindingLayoutItem& a, const nvrhi::BindingLayoutItem& b){
        if (a.slot != b.slot) return a.slot < b.slot;
        return a.type < b.type;
    });

    // Hash and lookup in cache
    const size_t h = HashBindingLayoutDesc(layoutDesc);

    std::lock_guard<std::mutex> lock(m_CacheMutex);

    auto cacheIt = m_BindingLayoutCache.find(h);
    if (cacheIt != m_BindingLayoutCache.end())
    {
        return cacheIt->second;
    }

    // Not found - create it and cache it
    nvrhi::BindingLayoutHandle handle = m_RHI->m_NvrhiDevice->createBindingLayout(layoutDesc);
    if (handle)
    {
        m_BindingLayoutCache.emplace(h, handle);
    }

    return handle;
}

nvrhi::BindingLayoutHandle Renderer::GetOrCreateBindlessLayout(const nvrhi::BindlessLayoutDesc& desc)
{
    SINGLE_THREAD_GUARD();

    // Hash and lookup in cache
    const size_t h = HashBindlessLayoutDesc(desc);
        auto cacheIt = m_BindingLayoutCache.find(h);
        if (cacheIt != m_BindingLayoutCache.end())
        {
            return cacheIt->second;
    }

    // Not found - create it and cache it
    nvrhi::BindingLayoutHandle handle = m_RHI->m_NvrhiDevice->createBindlessLayout(desc);
    if (handle)
    {
        m_BindingLayoutCache.emplace(h, handle);
    }

    return handle;
}

void Renderer::InitializeGlobalBindlessTextures()
{
    static const uint32_t kInitialTextureCapacity = 1024;

    // Create bindless layout for global textures
    nvrhi::BindlessLayoutDesc bindlessDesc;
    bindlessDesc.visibility = nvrhi::ShaderType::All;
    bindlessDesc.maxCapacity = kInitialTextureCapacity; // Large capacity for many textures
    bindlessDesc.layoutType = nvrhi::BindlessLayoutDesc::LayoutType::MutableSrvUavCbv;

    m_GlobalTextureBindingLayout = GetOrCreateBindlessLayout(bindlessDesc);
    if (!m_GlobalTextureBindingLayout)
    {
        SDL_LOG_ASSERT_FAIL("Failed to create global bindless layout for textures", "[Renderer] Failed to create global bindless layout for textures");
        return;
    }

    // Create descriptor table
    m_GlobalTextureDescriptorTable = m_RHI->m_NvrhiDevice->createDescriptorTable(m_GlobalTextureBindingLayout);
    if (!m_GlobalTextureDescriptorTable)
    {
        SDL_LOG_ASSERT_FAIL("Failed to create global texture descriptor table", "[Renderer] Failed to create global texture descriptor table");
        return;
    }

    m_RHI->m_NvrhiDevice->resizeDescriptorTable(m_GlobalTextureDescriptorTable, bindlessDesc.maxCapacity, false);
    
    SDL_Log("[Renderer] Global bindless texture system initialized");
}

uint32_t Renderer::RegisterTexture(nvrhi::TextureHandle texture)
{
    if (!texture || !m_GlobalTextureDescriptorTable)
    {
        SDL_LOG_ASSERT_FAIL("Invalid texture or descriptor table not initialized", "[Renderer] Invalid texture or descriptor table not initialized");
        return UINT32_MAX;
    }

    SINGLE_THREAD_GUARD();

    const uint32_t index = m_NextTextureIndex++;
    const bool bResult = RegisterTextureAtIndex(index, texture);
    if (!bResult)
    {
        SDL_LOG_ASSERT_FAIL("Failed to register texture in global descriptor table", "[Renderer] Failed to register texture at index %u", index);
        return UINT32_MAX;
    }
    return index;
}

bool Renderer::RegisterTextureAtIndex(uint32_t index, nvrhi::TextureHandle texture)
{
    if (!texture || !m_GlobalTextureDescriptorTable)
    {
        return false;
    }

    SINGLE_THREAD_GUARD();

    const nvrhi::BindingSetItem item = nvrhi::BindingSetItem::Texture_SRV(index, texture);
    if (!m_RHI->m_NvrhiDevice->writeDescriptorTable(m_GlobalTextureDescriptorTable, item))
    {
        SDL_LOG_ASSERT_FAIL("Failed to register texture in global descriptor table", "[Renderer] Failed to register texture at index %u", index);
        return false;
    }
    SDL_Log("[Renderer] Registered texture (%s) at index %u", texture->getDesc().debugName.c_str(), index);
    return true;
}

void Renderer::InitializeGlobalBindlessSamplers()
{
    static const uint32_t kInitialSamplerCapacity = 128;

    // Create bindless layout for global samplers
    nvrhi::BindlessLayoutDesc bindlessDesc;
    bindlessDesc.visibility = nvrhi::ShaderType::All;
    bindlessDesc.maxCapacity = kInitialSamplerCapacity;
    bindlessDesc.layoutType = nvrhi::BindlessLayoutDesc::LayoutType::MutableSampler;

    m_GlobalSamplerBindingLayout = GetOrCreateBindlessLayout(bindlessDesc);
    if (!m_GlobalSamplerBindingLayout)
    {
        SDL_LOG_ASSERT_FAIL("Failed to create global bindless layout for samplers", "[Renderer] Failed to create global bindless layout for samplers");
        return;
    }

    // Create descriptor table
    m_GlobalSamplerDescriptorTable = m_RHI->m_NvrhiDevice->createDescriptorTable(m_GlobalSamplerBindingLayout);
    if (!m_GlobalSamplerDescriptorTable)
    {
        SDL_LOG_ASSERT_FAIL("Failed to create global sampler descriptor table", "[Renderer] Failed to create global sampler descriptor table");
        return;
    }

    m_RHI->m_NvrhiDevice->resizeDescriptorTable(m_GlobalSamplerDescriptorTable, bindlessDesc.maxCapacity, false);

    SDL_Log("[Renderer] Global bindless sampler system initialized");
}

bool Renderer::RegisterSamplerAtIndex(uint32_t index, nvrhi::SamplerHandle sampler)
{
    if (!sampler || !m_GlobalSamplerDescriptorTable)
    {
        return false;
    }

    SINGLE_THREAD_GUARD();

    const nvrhi::BindingSetItem item = nvrhi::BindingSetItem::Sampler(index, sampler);
    if (!m_RHI->m_NvrhiDevice->writeDescriptorTable(m_GlobalSamplerDescriptorTable, item))
    {
        SDL_LOG_ASSERT_FAIL("Failed to register sampler in global descriptor table", "[Renderer] Failed to register sampler at index %u", index);
        return false;
    }
    SDL_Log("[Renderer] Registered sampler at index %u", index);
    return true;
}

void Renderer::HashPipelineCommonState(size_t& h, const nvrhi::RenderState& renderState, const nvrhi::FramebufferInfoEx& fbInfo, const nvrhi::BindingLayoutVector& bindingLayouts)
{
    // Raster State
    const nvrhi::RasterState& rs = renderState.rasterState;
    h = h * 1099511628211u + std::hash<int>()((int)rs.fillMode);
    h = h * 1099511628211u + std::hash<int>()((int)rs.cullMode);
    h = h * 1099511628211u + std::hash<bool>()(rs.frontCounterClockwise);
    h = h * 1099511628211u + std::hash<int>()(rs.depthBias);
    h = h * 1099511628211u + std::hash<float>()(rs.slopeScaledDepthBias);
    h = h * 1099511628211u + std::hash<bool>()(rs.depthClipEnable);
    h = h * 1099511628211u + std::hash<bool>()(rs.scissorEnable);
    h = h * 1099511628211u + std::hash<bool>()(rs.multisampleEnable);
    h = h * 1099511628211u + std::hash<bool>()(rs.conservativeRasterEnable);

    // Depth Stencil State
    const nvrhi::DepthStencilState& dss = renderState.depthStencilState;
    h = h * 1099511628211u + std::hash<bool>()(dss.depthTestEnable);
    h = h * 1099511628211u + std::hash<bool>()(dss.depthWriteEnable);
    h = h * 1099511628211u + std::hash<int>()((int)dss.depthFunc);
    h = h * 1099511628211u + std::hash<bool>()(dss.stencilEnable);
    h = h * 1099511628211u + std::hash<uint32_t>()(dss.stencilReadMask);
    h = h * 1099511628211u + std::hash<uint32_t>()(dss.stencilWriteMask);

    // Blend State
    const nvrhi::BlendState& bs = renderState.blendState;
    h = h * 1099511628211u + std::hash<bool>()(bs.alphaToCoverageEnable);
    for (const nvrhi::BlendState::RenderTarget& target : bs.targets)
    {
        h = h * 1099511628211u + std::hash<bool>()(target.blendEnable);
        h = h * 1099511628211u + std::hash<int>()((int)target.srcBlend);
        h = h * 1099511628211u + std::hash<int>()((int)target.destBlend);
        h = h * 1099511628211u + std::hash<int>()((int)target.blendOp);
        h = h * 1099511628211u + std::hash<int>()((int)target.srcBlendAlpha);
        h = h * 1099511628211u + std::hash<int>()((int)target.destBlendAlpha);
        h = h * 1099511628211u + std::hash<int>()((int)target.blendOpAlpha);
        h = h * 1099511628211u + std::hash<int>()((int)target.colorWriteMask);
    }

    // Framebuffer Info
    h = h * 1099511628211u + std::hash<int>()((int)fbInfo.depthFormat);
    for (nvrhi::Format format : fbInfo.colorFormats)
    {
        h = h * 1099511628211u + std::hash<int>()((int)format);
    }
    h = h * 1099511628211u + std::hash<uint32_t>()(fbInfo.sampleCount);

    // Binding Layouts
    for (const nvrhi::BindingLayoutHandle& bl : bindingLayouts)
    {
        h = h * 1099511628211u + std::hash<const void*>()(bl.Get());
    }
}

nvrhi::GraphicsPipelineHandle Renderer::GetOrCreateGraphicsPipeline(const nvrhi::GraphicsPipelineDesc& pipelineDesc, const nvrhi::FramebufferInfoEx& fbInfo)
{
    // Hash shaders and input-related properties
    size_t h = 1469598103934665603ull;
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.VS.Get());
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.PS.Get());
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.HS.Get());
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.DS.Get());
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.GS.Get());
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.inputLayout.Get());
    h = h * 1099511628211u + std::hash<int>()(static_cast<int>(pipelineDesc.primType));
    h = h * 1099511628211u + std::hash<uint32_t>()(pipelineDesc.patchControlPoints);

    // Hash common state: RenderState, FramebufferInfo, BindingLayouts
    HashPipelineCommonState(h, pipelineDesc.renderState, fbInfo, pipelineDesc.bindingLayouts);

    std::lock_guard<std::mutex> lock(m_CacheMutex);

    auto it = m_GraphicsPipelineCache.find(h);
    if (it != m_GraphicsPipelineCache.end())
        return it->second;

    SDL_Log("[Pipeline Cache] Creating new graphics pipeline (hash=%zu)", h);

    // Create pipeline and cache it
    nvrhi::GraphicsPipelineHandle pipeline = m_RHI->m_NvrhiDevice->createGraphicsPipeline(pipelineDesc, fbInfo);
    SDL_assert(pipeline && "Failed to create graphics pipeline");
    if (pipeline)
    {
        m_GraphicsPipelineCache.emplace(h, pipeline);
    }

    return pipeline;
}

nvrhi::MeshletPipelineHandle Renderer::GetOrCreateMeshletPipeline(const nvrhi::MeshletPipelineDesc& pipelineDesc, const nvrhi::FramebufferInfoEx& fbInfo)
{
    // Hash shaders and meshlet-specific properties
    size_t h = 1469598103934665603ull;
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.AS.Get());
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.MS.Get());
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.PS.Get());
    h = h * 1099511628211u + std::hash<int>()(static_cast<int>(pipelineDesc.primType));

    // Hash common state: RenderState, FramebufferInfo, BindingLayouts
    HashPipelineCommonState(h, pipelineDesc.renderState, fbInfo, pipelineDesc.bindingLayouts);

    std::lock_guard<std::mutex> lock(m_CacheMutex);

    auto it = m_MeshletPipelineCache.find(h);
    if (it != m_MeshletPipelineCache.end())
        return it->second;

    SDL_Log("[Pipeline Cache] Creating new meshlet pipeline (hash=%zu)", h);

    // Create pipeline and cache it
    nvrhi::MeshletPipelineHandle pipeline = m_RHI->m_NvrhiDevice->createMeshletPipeline(pipelineDesc, fbInfo);
    SDL_assert(pipeline && "Failed to create meshlet pipeline");
    if (pipeline)
    {
        m_MeshletPipelineCache.emplace(h, pipeline);
    }

    return pipeline;
}

nvrhi::ComputePipelineHandle Renderer::GetOrCreateComputePipeline(nvrhi::ShaderHandle shader, const nvrhi::BindingLayoutVector& bindingLayouts)
{
    // Hash relevant pipeline properties: CS shader handle, binding layout pointers
    size_t h = 1469598103934665603ull;
    h = h * 1099511628211u + std::hash<const void*>()(shader.Get());
    for (const nvrhi::BindingLayoutHandle& layout : bindingLayouts)
    {
        h = h * 1099511628211u + std::hash<const void*>()(layout.Get());
    }

    std::lock_guard<std::mutex> lock(m_CacheMutex);

    auto it = m_ComputePipelineCache.find(h);
    if (it != m_ComputePipelineCache.end())
        return it->second;

    SDL_Log("[Pipeline Cache] Creating new compute pipeline (hash=%zu)", h);

    // Create pipeline and cache it
    nvrhi::ComputePipelineDesc desc;
    desc.CS = shader;
    desc.bindingLayouts = bindingLayouts;
    nvrhi::ComputePipelineHandle pipeline = m_RHI->m_NvrhiDevice->createComputePipeline(desc);
    SDL_assert(pipeline && "Failed to create compute pipeline");
    if (pipeline)
    {
        m_ComputePipelineCache.emplace(h, pipeline);
    }

    return pipeline;
}

void Renderer::AddFullScreenPass(const RenderPassParams& params)
{
    PROFILE_SCOPED(params.shaderName.data());
    nvrhi::utils::ScopedMarker scopedMarker{ params.commandList, params.shaderName.data() };

    nvrhi::MeshletPipelineDesc desc;
    desc.MS = GetShaderHandle("FullScreen_MSMain");
    desc.PS = GetShaderHandle(params.shaderName);

    std::vector<nvrhi::BindingSetHandle> bindingSets;

    const nvrhi::BindingLayoutHandle layout = GetOrCreateBindingLayoutFromBindingSetDesc(params.bindingSetDesc);
    const nvrhi::BindingSetHandle bindingSet = m_RHI->m_NvrhiDevice->createBindingSet(params.bindingSetDesc, layout);

    desc.bindingLayouts.push_back(layout);
    bindingSets.push_back(bindingSet);

    if (params.useBindlessResources)
    {
        desc.bindingLayouts.push_back(GetGlobalTextureBindingLayout());
        bindingSets.push_back(GetGlobalTextureDescriptorTable());

        desc.bindingLayouts.push_back(GetGlobalSamplerBindingLayout());
        bindingSets.push_back(GetGlobalSamplerDescriptorTable());
    }

    desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    desc.renderState.depthStencilState = params.depthStencilState ? *params.depthStencilState : CommonResources::GetInstance().DepthDisabled;

    nvrhi::MeshletPipelineHandle pipeline = GetOrCreateMeshletPipeline(desc, params.framebuffer->getFramebufferInfo());

    nvrhi::MeshletState state;
    if (params.depthStencilState)
    {
        state.dynamicStencilRefValue = params.depthStencilState->stencilRefValue;
    }
    state.pipeline = pipeline;
    for (const nvrhi::BindingSetHandle& bindingSet : bindingSets)
    {
        state.bindings.push_back(bindingSet.Get());
    }
    state.framebuffer = params.framebuffer;

    const nvrhi::FramebufferDesc& fbDesc = params.framebuffer->getDesc();
    nvrhi::TextureDesc texDesc = fbDesc.colorAttachments[0].texture->getDesc();
    uint32_t mipLevel = fbDesc.colorAttachments[0].subresources.baseMipLevel;
    uint32_t width = std::max(1u, texDesc.width >> mipLevel);
    uint32_t height = std::max(1u, texDesc.height >> mipLevel);

    state.viewport.viewports.push_back(nvrhi::Viewport(0, (float)width, 0, (float)height, 0, 1));
    state.viewport.scissorRects.push_back(nvrhi::Rect(0, (int)width, 0, (int)height));

    params.commandList->setMeshletState(state);
    if (params.pushConstants && params.pushConstantsSize > 0)
    {
        params.commandList->setPushConstants(params.pushConstants, params.pushConstantsSize);
    }
    params.commandList->dispatchMesh(1, 1, 1);
}

void Renderer::AddComputePass(const RenderPassParams& params)
{
    nvrhi::utils::ScopedMarker scopedMarker{ params.commandList, params.shaderName.data() };

    nvrhi::BindingLayoutVector layouts;
    std::vector<nvrhi::BindingSetHandle> bindingSets;

    const nvrhi::BindingLayoutHandle layout = GetOrCreateBindingLayoutFromBindingSetDesc(params.bindingSetDesc);
    const nvrhi::BindingSetHandle bindingSet = m_RHI->m_NvrhiDevice->createBindingSet(params.bindingSetDesc, layout);

    layouts.push_back(layout);
    bindingSets.push_back(bindingSet);

    if (params.useBindlessResources)
    {
        layouts.push_back(GetGlobalTextureBindingLayout());
        bindingSets.push_back(GetGlobalTextureDescriptorTable());

        layouts.push_back(GetGlobalSamplerBindingLayout());
        bindingSets.push_back(GetGlobalSamplerDescriptorTable());
    }

    nvrhi::ComputeState state;
    state.pipeline = GetOrCreateComputePipeline(GetShaderHandle(params.shaderName), layouts);
    for (const nvrhi::BindingSetHandle& bindingSet : bindingSets)
    {
        state.bindings.push_back(bindingSet.Get());
    }

    if (params.dispatchParams.indirectBuffer)
    {
        state.indirectParams = params.dispatchParams.indirectBuffer;
    }

    params.commandList->setComputeState(state);

    if (params.pushConstants && params.pushConstantsSize > 0)
    {
        params.commandList->setPushConstants(params.pushConstants, params.pushConstantsSize);
    }

    if (!params.dispatchParams.indirectBuffer)
    {
        params.commandList->dispatch(params.dispatchParams.x, params.dispatchParams.y, params.dispatchParams.z);
    }
    else
    {
        params.commandList->dispatchIndirect(params.dispatchParams.indirectOffsetBytes);
    }
}

void Renderer::GenerateMipsUsingSPD(nvrhi::TextureHandle texture, nvrhi::BufferHandle spdAtomicCounter, nvrhi::CommandListHandle commandList, const char* markerName, SpdReductionType reductionType)
{
    nvrhi::utils::ScopedMarker spdMarker{ commandList, markerName };

    const nvrhi::FormatInfo& formatInfo = nvrhi::getFormatInfo(texture->getDesc().format);
    const uint32_t numChannels = formatInfo.hasBlue ? 3 : 1;

    const uint32_t numMips = texture->getDesc().mipLevels;

    // We generate mips 1..N. SPD will be configured to take mip 0 as source.
    // So SPD "mips" count is numMips - 1. 
    // Note: SPD refers to how many downsample steps to take.
    const uint32_t spdmips = numMips - 1;

    FfxUInt32x2 dispatchThreadGroupCountXY;
    FfxUInt32x2 workGroupOffset;
    FfxUInt32x2 numWorkGroupsAndMips;
    FfxUInt32x4 rectInfo = { 0, 0, texture->getDesc().width, texture->getDesc().height };

    ffxSpdSetup(dispatchThreadGroupCountXY, workGroupOffset, numWorkGroupsAndMips, rectInfo, spdmips);

    // Constant buffer matching SPD.hlsl
    SpdConstants spdData;
    spdData.m_Mips = numWorkGroupsAndMips[1];
    spdData.m_NumWorkGroups = numWorkGroupsAndMips[0];
    spdData.m_WorkGroupOffset.x = workGroupOffset[0];
    spdData.m_WorkGroupOffset.y = workGroupOffset[1];
    spdData.m_ReductionType = reductionType;

    // Clear atomic counter
    commandList->clearBufferUInt(spdAtomicCounter, 0);

    nvrhi::BindingSetDesc spdBset;
    spdBset.bindings.push_back(nvrhi::BindingSetItem::PushConstants(0, sizeof(SpdConstants)));
    spdBset.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(0, texture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet{ 0, 1, 0, 1 }));

    // Bind mips 1..N to UAV slots 0..N-1
    for (uint32_t i = 1; i < numMips; ++i)
    {
        spdBset.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(i - 1, texture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet{ i, 1, 0, 1 }));
    }
    for (uint32_t i = numMips; i <= 12; ++i)
    {
        // Fill remaining UAV slots with a dummy to satisfy binding layout
        spdBset.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(i - 1, CommonResources::GetInstance().DummyUAVTexture));
    }

    // Atomic counter always at slot 12
    spdBset.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_UAV(12, spdAtomicCounter));

    Renderer::RenderPassParams params{
        .commandList = commandList,
        .shaderName = (numChannels == 3) ? "SPD_SPD_CSMain_3Channel" : "SPD_SPD_CSMain_1Channel",
        .bindingSetDesc = spdBset,
        .pushConstants = &spdData,
        .pushConstantsSize = sizeof(spdData),
        .dispatchParams = { .x = dispatchThreadGroupCountXY[0], .y = dispatchThreadGroupCountXY[1], .z = 1 }
    };

    AddComputePass(params);
}

nvrhi::CommandListHandle Renderer::AcquireCommandList(bool bImmediatelyQueue)
{
    PROFILE_FUNCTION();

    nvrhi::CommandListHandle handle;
    {
        std::lock_guard<std::mutex> lock(m_CommandListMutex);
        if (!m_CommandListFreeList.empty())
        {
            handle = m_CommandListFreeList.back();
            m_CommandListFreeList.pop_back();
        }
        else
        {
            const nvrhi::CommandListParameters params{ .enableImmediateExecution = false, .queueType = nvrhi::CommandQueue::Graphics };
            handle = m_RHI->m_NvrhiDevice->createCommandList(params);
        }

        SDL_assert(handle && "Failed to acquire command list");

        if (bImmediatelyQueue)
        {
            m_PendingCommandLists.push_back(handle);
        }
    }

    return handle;
}

void Renderer::ExecutePendingCommandLists()
{
    PROFILE_FUNCTION();
     SINGLE_THREAD_GUARD();

    // Wait for GPU to finish all work before presenting
    {
        PROFILE_SCOPED("WaitForIdle");
        m_RHI->m_NvrhiDevice->waitForIdle();
     }

    m_CommandListFreeList.insert(m_CommandListFreeList.end(), m_InFlightCommandLists.begin(), m_InFlightCommandLists.end());
    m_InFlightCommandLists.clear();

    if (!m_PendingCommandLists.empty())
    {
        if (Config::Get().ExecutePerPass || Config::Get().ExecutePerPassAndWait)
        {
            for (const nvrhi::CommandListHandle& handle : m_PendingCommandLists)
            {
                m_RHI->m_NvrhiDevice->executeCommandList(handle);

                if (Config::Get().ExecutePerPassAndWait)
                {
                    m_RHI->m_NvrhiDevice->waitForIdle();
                }
            }
        }
        else
        {
            std::vector<nvrhi::ICommandList*> rawLists;
            rawLists.reserve(m_PendingCommandLists.size());
            for (const nvrhi::CommandListHandle& handle : m_PendingCommandLists)
            {
                rawLists.push_back(handle.Get());
            }

            m_RHI->m_NvrhiDevice->executeCommandLists(rawLists.data(), rawLists.size());
        }

        m_InFlightCommandLists.insert(m_InFlightCommandLists.end(), m_PendingCommandLists.begin(), m_PendingCommandLists.end());
        m_PendingCommandLists.clear();
    }
}


int main(int argc, char* argv[])
{
    Renderer renderer{};
    Renderer::SetInstance(&renderer);
    Config::ParseCommandLine(argc, argv);

    renderer.Initialize();

    renderer.Run();
    renderer.Shutdown();
    Renderer::SetInstance(nullptr);
    return 0;
}