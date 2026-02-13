#include "Renderer.h"
#include "Utilities.h"
#include "Config.h"
#include "CommonResources.h"

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

#include <imgui.h>
#include <imgui_impl_sdl3.h>

// ============================================================================
// Global Render Graph Handles for Transient Resources
// ============================================================================

// Depth & GBuffer textures (transient, frame-local)
RGTextureHandle g_RG_DepthTexture;
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
public:
    void Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();
        const uint32_t width = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;
        
        // Declare transient depth texture
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = nvrhi::Format::D32;
            desc.m_NvrhiDesc.isRenderTarget = true;
            desc.m_NvrhiDesc.debugName = "DepthBuffer_RG";
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::DepthWrite;
            desc.m_NvrhiDesc.keepInitialState = true;
            desc.m_NvrhiDesc.setClearValue(nvrhi::Color{ Renderer::DEPTH_FAR, 0.0f, 0.0f, 0.0f });
            
            g_RG_DepthTexture = renderGraph.DeclareTexture(desc, g_RG_DepthTexture);
        }

        // HDR Color Texture
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = Renderer::HDR_COLOR_FORMAT;
            desc.m_NvrhiDesc.debugName = "HDRColorTexture_RG";
            desc.m_NvrhiDesc.isRenderTarget = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::RenderTarget;
            desc.m_NvrhiDesc.setClearValue(Renderer::kHDROutputClearColor);
            g_RG_HDRColor = renderGraph.DeclareTexture(desc, g_RG_HDRColor);
        }
        
        // Declare transient GBuffer textures
        RGTextureDesc gbufferDesc;
        gbufferDesc.m_NvrhiDesc.width = width;
        gbufferDesc.m_NvrhiDesc.height = height;
        gbufferDesc.m_NvrhiDesc.isRenderTarget = true;
        gbufferDesc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::RenderTarget;
        gbufferDesc.m_NvrhiDesc.keepInitialState = true;
        gbufferDesc.m_NvrhiDesc.setClearValue(nvrhi::Color{});
        
        // Albedo: RGBA8
        gbufferDesc.m_NvrhiDesc.format = nvrhi::Format::RGBA8_UNORM;
        gbufferDesc.m_NvrhiDesc.debugName = "GBufferAlbedo_RG";
        g_RG_GBufferAlbedo = renderGraph.DeclareTexture(gbufferDesc, g_RG_GBufferAlbedo);
        
        // Normals: RG16_FLOAT
        gbufferDesc.m_NvrhiDesc.format = nvrhi::Format::RG16_FLOAT;
        gbufferDesc.m_NvrhiDesc.debugName = "GBufferNormals_RG";
        g_RG_GBufferNormals = renderGraph.DeclareTexture(gbufferDesc, g_RG_GBufferNormals);
        
        // ORM: RGBA8
        gbufferDesc.m_NvrhiDesc.format = nvrhi::Format::RGBA8_UNORM;
        gbufferDesc.m_NvrhiDesc.debugName = "GBufferORM_RG";
        g_RG_GBufferORM = renderGraph.DeclareTexture(gbufferDesc, g_RG_GBufferORM);
        
        // Emissive: RGBA8
        gbufferDesc.m_NvrhiDesc.format = nvrhi::Format::RGBA8_UNORM;
        gbufferDesc.m_NvrhiDesc.debugName = "GBufferEmissive_RG";
        g_RG_GBufferEmissive = renderGraph.DeclareTexture(gbufferDesc, g_RG_GBufferEmissive);
        
        // Motion Vectors: RG16_FLOAT
        gbufferDesc.m_NvrhiDesc.format = nvrhi::Format::RG16_FLOAT;
        gbufferDesc.m_NvrhiDesc.debugName = "GBufferMotion_RG";
        g_RG_GBufferMotionVectors = renderGraph.DeclareTexture(gbufferDesc, g_RG_GBufferMotionVectors);

        renderGraph.WriteTexture(g_RG_DepthTexture);
        renderGraph.WriteTexture(g_RG_HDRColor);
        renderGraph.WriteTexture(g_RG_GBufferAlbedo);
        renderGraph.WriteTexture(g_RG_GBufferNormals);
        renderGraph.WriteTexture(g_RG_GBufferORM);
        renderGraph.WriteTexture(g_RG_GBufferEmissive);
        renderGraph.WriteTexture(g_RG_GBufferMotionVectors);
    }
    
    void Render(nvrhi::CommandListHandle commandList) override
    {
        Renderer* renderer = Renderer::GetInstance();

        // Get transient resources from render graph
        nvrhi::TextureHandle depthTexture = renderer->m_RenderGraph.GetTexture(g_RG_DepthTexture);
        nvrhi::TextureHandle hdrColor = renderer->m_RenderGraph.GetTexture(g_RG_HDRColor);
        nvrhi::TextureHandle gbufferAlbedo = renderer->m_RenderGraph.GetTexture(g_RG_GBufferAlbedo);
        nvrhi::TextureHandle gbufferNormals = renderer->m_RenderGraph.GetTexture(g_RG_GBufferNormals);
        nvrhi::TextureHandle gbufferORM = renderer->m_RenderGraph.GetTexture(g_RG_GBufferORM);
        nvrhi::TextureHandle gbufferEmissive = renderer->m_RenderGraph.GetTexture(g_RG_GBufferEmissive);
        nvrhi::TextureHandle gbufferMotion = renderer->m_RenderGraph.GetTexture(g_RG_GBufferMotionVectors);

        // Clear depth for reversed-Z (clear to 0.0f, no stencil)
        commandList->clearDepthStencilTexture(depthTexture, nvrhi::AllSubresources, true, Renderer::DEPTH_FAR, false, 0);

        // Clear HDR color
        commandList->clearTextureFloat(hdrColor, nvrhi::AllSubresources, Renderer::kHDROutputClearColor);

        // clear gbuffers
        commandList->clearTextureFloat(gbufferAlbedo, nvrhi::AllSubresources, nvrhi::Color{});
        commandList->clearTextureFloat(gbufferNormals, nvrhi::AllSubresources, nvrhi::Color{});
        commandList->clearTextureFloat(gbufferORM, nvrhi::AllSubresources, nvrhi::Color{});
        commandList->clearTextureFloat(gbufferEmissive, nvrhi::AllSubresources, nvrhi::Color{});
        commandList->clearTextureFloat(gbufferMotion, nvrhi::AllSubresources, nvrhi::Color{});
    }
    const char* GetName() const override { return "Clear"; }
};

class TLASRenderer : public IRenderer
{
public:

    void Render(nvrhi::CommandListHandle commandList) override
    {
        Renderer* renderer = Renderer::GetInstance();
        Scene& scene = renderer->m_Scene;

        if (!scene.m_TLAS || !scene.m_RTInstanceDescBuffer || scene.m_RTInstanceDescs.empty())
            return;

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
    CommonResources::GetInstance().Initialize();
    CommonResources::GetInstance().RegisterDefaultTextures();
    m_BasePassResources.Initialize();
    CreateSceneResources();
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
    m_Scene.LoadScene();

    // Initialize renderers with scene-dependent resources
    for (const auto& renderer : m_Renderers)
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
        }

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

        // Update animations
        if (m_EnableAnimations)
        {
            PROFILE_SCOPED("Update Animations");

            m_Scene.Update(static_cast<float>(m_FrameTime / 1000.0));
            if (m_Scene.m_InstanceDirtyRange.first <= m_Scene.m_InstanceDirtyRange.second)
            {
                nvrhi::CommandListHandle cmd = AcquireCommandList("Upload Animated Instances");
                ScopedCommandList scopedCmd{ cmd };
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

        // Prepare ImGui UI (NewFrame + UI creation + ImGui::Render)
        m_ImGuiLayer.UpdateFrame();

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
            nvrhi::CommandListHandle cmd = AcquireCommandList("GPU Frame Start");
            ScopedCommandList scopedCmd{ cmd };
            m_RHI->m_NvrhiDevice->resetTimerQuery(m_GPUQueries[readIndex]);
            scopedCmd->beginTimerQuery(m_GPUQueries[writeIndex]);
        }
        
        m_RenderGraph.Reset();

        #define ADD_RENDER_PASS(rendererName) \
        { \
            extern IRenderer* rendererName; \
            IRenderer* pRenderer = rendererName; \
            m_RenderGraph.BeginPass(pRenderer->GetName()); \
            pRenderer->Setup(m_RenderGraph); \
            nvrhi::CommandListHandle cmd = AcquireCommandList(pRenderer->GetName()); \
            const bool bImmediateExecute = false; /* defer execution until after render graph compiles */ \
            m_TaskScheduler->ScheduleTask([this, pRenderer, cmd, readIndex, writeIndex]() { \
                PROFILE_SCOPED(pRenderer->GetName()) \
                ScopedCommandList scopedCmd{ cmd }; \
                if (m_RHI->m_NvrhiDevice->pollTimerQuery(pRenderer->m_GPUQueries[readIndex])) \
                { \
                    pRenderer->m_GPUTime = SimpleTimer::SecondsToMilliseconds(m_RHI->m_NvrhiDevice->getTimerQueryTime(pRenderer->m_GPUQueries[readIndex])); \
                } \
                m_RHI->m_NvrhiDevice->resetTimerQuery(pRenderer->m_GPUQueries[readIndex]); \
                SimpleTimer cpuTimer; \
                scopedCmd->beginTimerQuery(pRenderer->m_GPUQueries[writeIndex]); \
                pRenderer->Render(scopedCmd); \
                scopedCmd->endTimerQuery(pRenderer->m_GPUQueries[writeIndex]); \
                pRenderer->m_CPUTime = static_cast<float>(cpuTimer.TotalMilliseconds()); \
            }, bImmediateExecute); \
        }

        ADD_RENDER_PASS(g_TLASRenderer);
        ADD_RENDER_PASS(g_ClearRenderer);
        ADD_RENDER_PASS(g_OpaquePhase1Renderer);
        ADD_RENDER_PASS(g_HZBGenerator);
        ADD_RENDER_PASS(g_OpaquePhase2Renderer);
        ADD_RENDER_PASS(g_MaskedPassRenderer);
        ADD_RENDER_PASS(g_HZBGeneratorPhase2);
        ADD_RENDER_PASS(g_DeferredRenderer);
        ADD_RENDER_PASS(g_TransparentPassRenderer);
        ADD_RENDER_PASS(g_BloomRenderer);
        ADD_RENDER_PASS(g_HDRRenderer);
        ADD_RENDER_PASS(g_ImGuiRenderer);

        #undef ADD_RENDER_PASS

        // Compile render graph: compute lifetimes and allocate resources
        m_RenderGraph.Compile();
        m_RenderGraph.Execute();

        // Wait for all render passes to finish recording
        m_TaskScheduler->ExecuteAllScheduledTasks();

        // GPU query for frame timer is super expensive on the CPU for some reason. i give up using it
        if constexpr (false)
        {
            PROFILE_SCOPED("GPU Frame End");
            nvrhi::CommandListHandle cmd = AcquireCommandList("GPU Frame End");
            ScopedCommandList scopedCmd{ cmd };
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
    m_BasePassResources.~BasePassResources(); // explicit destructor call to ensure release of GPU resources

    // Shutdown global bindless texture system
    m_GlobalTextureDescriptorTable = nullptr;
    m_GlobalTextureBindingLayout = nullptr;
    m_NextTextureIndex = 0;

    // Shutdown scene and free its GPU resources
    m_Scene.Shutdown();

    DestroySceneResources();

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
    for (const auto& layout : bindingLayouts)
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

    if (params.useBindlessTextures)
    {
        desc.bindingLayouts.push_back(GetGlobalTextureBindingLayout());
        bindingSets.push_back(GetGlobalTextureDescriptorTable());
    }

    const uint32_t registerSpace = params.useBindlessTextures ? (m_RHI->GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN ? 0 : 1) : 0;
    const nvrhi::BindingLayoutHandle layout = GetOrCreateBindingLayoutFromBindingSetDesc(params.bindingSetDesc, registerSpace);
    const nvrhi::BindingSetHandle bindingSet = m_RHI->m_NvrhiDevice->createBindingSet(params.bindingSetDesc, layout);

    desc.bindingLayouts.push_back(layout);
    bindingSets.push_back(bindingSet);

    desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    desc.renderState.depthStencilState.depthTestEnable = false;
    desc.renderState.depthStencilState.depthWriteEnable = false;

    nvrhi::MeshletPipelineHandle pipeline = GetOrCreateMeshletPipeline(desc, params.framebuffer->getFramebufferInfo());

    nvrhi::MeshletState state;
    state.pipeline = pipeline;
    for (const auto& bindingSet : bindingSets)
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

    if (params.useBindlessTextures)
    {
        layouts.push_back(GetGlobalTextureBindingLayout());
        bindingSets.push_back(GetGlobalTextureDescriptorTable());
    }

    const uint32_t registerSpace = params.useBindlessTextures ? (m_RHI->GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN ? 0 : 1) : 0;
    const nvrhi::BindingLayoutHandle layout = GetOrCreateBindingLayoutFromBindingSetDesc(params.bindingSetDesc, registerSpace);
    const nvrhi::BindingSetHandle bindingSet = m_RHI->m_NvrhiDevice->createBindingSet(params.bindingSetDesc, layout);
    
    layouts.push_back(layout);
    bindingSets.push_back(bindingSet);

    nvrhi::ComputeState state;
    state.pipeline = GetOrCreateComputePipeline(GetShaderHandle(params.shaderName), layouts);
    for (const auto& bindingSet : bindingSets)
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

void Renderer::GenerateMipsUsingSPD(nvrhi::TextureHandle texture, nvrhi::CommandListHandle commandList, const char* markerName, SpdReductionType reductionType)
{
    nvrhi::utils::ScopedMarker spdMarker{ commandList, markerName };

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

    // Constant buffer matching HZBDownsampleSPD.hlsl
    SpdConstants spdData;
    spdData.m_Mips = numWorkGroupsAndMips[1];
    spdData.m_NumWorkGroups = numWorkGroupsAndMips[0];
    spdData.m_WorkGroupOffset.x = workGroupOffset[0];
    spdData.m_WorkGroupOffset.y = workGroupOffset[1];
    spdData.m_ReductionType = reductionType;

    // Clear atomic counter
    commandList->clearBufferUInt(m_SPDAtomicCounter, 0);

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
    spdBset.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_UAV(12, m_SPDAtomicCounter));

    Renderer::RenderPassParams params{
        .commandList = commandList,
        .shaderName = "HZBDownsampleSPD_HZBDownsampleSPD_CSMain",
        .bindingSetDesc = spdBset,
        .pushConstants = &spdData,
        .pushConstantsSize = sizeof(spdData),
        .dispatchParams = { .x = dispatchThreadGroupCountXY[0], .y = dispatchThreadGroupCountXY[1], .z = 1 }
    };

    AddComputePass(params);
}

nvrhi::CommandListHandle Renderer::AcquireCommandList(std::string_view markerName, bool bImmediatelyQueue)
{
    PROFILE_FUNCTION();
    SINGLE_THREAD_GUARD();

    nvrhi::CommandListHandle handle;
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

    handle->open();
    handle->beginMarker(markerName.data());

    if (bImmediatelyQueue)
    {
        m_PendingCommandLists.push_back(handle);
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

void Renderer::CreateSceneResources()
{
    SDL_Log("[Init] Creating Depth textures");
    
    // Calculate HZB resolution - use next lower power of 2 for each dimension
    uint32_t sw = m_RHI->m_SwapchainExtent.x;
    uint32_t sh = m_RHI->m_SwapchainExtent.y;

    uint32_t hzbWidth = NextLowerPow2(sw);
    uint32_t hzbHeight = NextLowerPow2(sh);
    hzbWidth = hzbWidth > sw ? hzbWidth >> 1 : hzbWidth;
    hzbHeight = hzbHeight > sh ? hzbHeight >> 1 : hzbHeight;

    // Calculate number of mip levels
    uint32_t maxDim = std::max(hzbWidth, hzbHeight);
    uint32_t mipLevels = 0;
    while (maxDim > 0) {
        mipLevels++;
        maxDim >>= 1;
    }

    // Create current HZB texture
    nvrhi::TextureDesc hzbDesc;
    hzbDesc.width = hzbWidth;
    hzbDesc.height = hzbHeight;
    hzbDesc.arraySize = 1;
    hzbDesc.mipLevels = mipLevels;
    hzbDesc.format = nvrhi::Format::R32_FLOAT;
    hzbDesc.debugName = "HZB";
    hzbDesc.isRenderTarget = false;
    hzbDesc.isUAV = true;
    hzbDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    hzbDesc.dimension = nvrhi::TextureDimension::Texture2D;
    
    m_HZBTexture = m_RHI->m_NvrhiDevice->createTexture(hzbDesc);
    if (!m_HZBTexture)
    {
        SDL_LOG_ASSERT_FAIL("Failed to create HZB texture", "[Init] Failed to create HZB texture");
        return;
    }

    // Create atomic counter buffer for SPD
    nvrhi::BufferDesc counterDesc;
    counterDesc.structStride = sizeof(uint32_t);
    counterDesc.byteSize = sizeof(uint32_t);
    counterDesc.canHaveUAVs = true;
    counterDesc.debugName = "SPD Atomic Counter";
    counterDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    m_SPDAtomicCounter = m_RHI->m_NvrhiDevice->createBuffer(counterDesc);

    nvrhi::CommandListHandle cmd = AcquireCommandList("HZB_Clear");
    ScopedCommandList scopedCmd{ cmd };
    scopedCmd->clearTextureFloat(m_HZBTexture, nvrhi::AllSubresources, DEPTH_FAR);

    SDL_Log("[Init] Created HZB texture (%ux%u, %u mips)", hzbWidth, hzbHeight, mipLevels);
}

void Renderer::DestroySceneResources()
{
    SDL_Log("[Shutdown] Destroying Depth textures");

    m_HZBTexture = nullptr;
    m_SPDAtomicCounter = nullptr;
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