#include "Renderer.h"
#include "Utilities.h"
#include "Config.h"
#include "CommonResources.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>

class ClearRenderer : public IRenderer
{
public:
    bool Initialize() override { return true; }
    void Render(nvrhi::CommandListHandle commandList) override
    {
        Renderer* renderer = Renderer::GetInstance();
        // Clear color
        commandList->clearTextureFloat(renderer->GetCurrentBackBufferTexture(), nvrhi::AllSubresources, nvrhi::Color(0.14f, 0.23f, 0.33f, 1.0f));
        // Clear depth for reversed-Z (clear to 0.0f, no stencil)
        commandList->clearDepthStencilTexture(renderer->m_DepthTexture, nvrhi::AllSubresources, true, 0.0f, false, 0);
    }
    const char* GetName() const override { return "Clear"; }
};

REGISTER_RENDERER(ClearRenderer);

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
    std::filesystem::path GetShaderOutputPath(const std::filesystem::path& exeDir, const ShaderMetadata& metadata)
    {
        std::filesystem::path filename = metadata.sourcePath.stem();
        std::string outName = filename.string() + "_" + metadata.entryPoint + metadata.suffix;

        if (!metadata.defines.empty())
        {
            std::vector<std::string> sortedDefines = metadata.defines;
            std::stable_sort(sortedDefines.begin(), sortedDefines.end());

            std::string combinedDefines;
            for (const auto& d : sortedDefines)
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

        outName += ".spirv";
        return exeDir / "shaders" / outName;
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

bool Renderer::LoadShaders()
{
    SDL_Log("[Init] Loading compiled shaders from config");

    // Get executable directory
    const char* basePathCStr = SDL_GetBasePath();
    if (!basePathCStr)
    {
        SDL_Log("[Init] Failed to get base path");
        return false;
    }
    const std::filesystem::path exeDir = basePathCStr;

    // Parse shaders.cfg to get list of shaders to load
    const std::filesystem::path configPath = exeDir / ".." / "src" / "shaders" / "shaders.cfg";
    std::vector<ShaderMetadata> shaderMetadata = ParseShaderConfig(configPath.generic_string());
    if (shaderMetadata.empty())
    {
        SDL_Log("[Init] No shaders to load from config");
        return true; // Not an error, just no shaders defined yet
    }

    // Load each shader
    for (const ShaderMetadata& metadata : shaderMetadata)
    {
        // Determine output filename based on ShaderMake naming convention
        const std::filesystem::path outputPath = GetShaderOutputPath(exeDir, metadata);

        // Read the compiled binary
        const std::vector<uint8_t> binary = ReadBinaryFile(outputPath);
        if (binary.empty())
        {
            SDL_Log("[Init] Failed to load compiled shader: %s", outputPath.generic_string().c_str());
            return false;
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
            return false;
        }

        // Keyed by logical name (e.g., "ForwardLighting_PSMain_AlphaTest") for easy retrieval
        const std::string key = metadata.sourcePath.stem().string() + "_" + metadata.entryPoint + metadata.suffix;
        m_ShaderCache[key] = handle;
        SDL_Log("[Init] Loaded shader: %s (key=%s)", outputPath.generic_string().c_str(), key.c_str());
    }

    SDL_Log("[Init] All %zu shader(s) loaded successfully", shaderMetadata.size());
    return true;
}

void Renderer::UnloadShaders()
{
    SDL_Log("[Shutdown] Unloading shaders");
    m_ShaderCache.clear();
    SDL_Log("[Shutdown] Shaders unloaded");
}

nvrhi::ShaderHandle Renderer::GetShaderHandle(std::string_view name) const
{
    std::unordered_map<std::string, nvrhi::ShaderHandle>::const_iterator it = m_ShaderCache.find(std::string{name});
    if (it != m_ShaderCache.end())
        return it->second;
    
    SDL_LOG_ASSERT_FAIL("Requested shader not found in cache", "[Error] Shader '%s' not found in cache!", std::string{name}.c_str());
    return {};
}

nvrhi::TextureHandle Renderer::GetCurrentBackBufferTexture() const
{
    return m_RHI->m_NvrhiSwapchainTextures[m_CurrentSwapchainImageIdx];
}

bool Renderer::Initialize()
{
    ScopedTimerLog initScope{"[Timing] Init phase:"};

    MicroProfileOnThreadCreate("Main");
	MicroProfileSetEnableAllGroups(true);
	MicroProfileSetForceMetaCounters(true);

    InitSDL();

    m_Window = CreateWindowScaled();
    if (!m_Window)
    {
        SDL_Quit();
        return false;
    }

    m_RHI = CreateGraphicRHI(Config::Get().m_GraphicsAPI);
    if (!m_RHI->Initialize(m_Window))
    {
        Shutdown();
        return false;
    }

    SDL_assert(m_RHI->m_NvrhiDevice && "NVRHI device is null after RHI initialization");

    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);

    if (!m_RHI->CreateSwapchain(static_cast<uint32_t>(windowWidth), static_cast<uint32_t>(windowHeight)))
    {
        Shutdown();
        return false;
    }

    if (!CommonResources::GetInstance().Initialize())
    {
        SDL_Log("[Init] Failed to initialize common resources");
        Shutdown();
        return false;
    }

    if (!InitializeGlobalBindlessTextures())
    {
        SDL_Log("[Init] Failed to initialize global bindless textures");
        Shutdown();
        return false;
    }

    // Register default textures with the global bindless system
    if (!CommonResources::GetInstance().RegisterDefaultTextures())
    {
        SDL_Log("[Init] Failed to register default textures");
        Shutdown();
        return false;
    }

    if (!CreateDepthTextures())
    {
        Shutdown();
        return false;
    }

    if (!LoadShaders())
    {
        Shutdown();
        return false;
    }

    if (!m_ImGuiLayer.Initialize())
    {
        Shutdown();
        return false;
    }

    // Initialize renderers now that shaders and device are ready
    for (const RendererRegistry::Creator& creator : RendererRegistry::GetCreators())
    {
        auto renderer = creator();
        if (!renderer || !renderer->Initialize())
        {
            SDL_LOG_ASSERT_FAIL("Renderer initialization failed", "[Init] Failed to initialize a renderer");
            Shutdown();
            return false;
        }
        m_Renderers.push_back(renderer);
        renderer->m_GPUQueries[0] = m_RHI->m_NvrhiDevice->createTimerQuery();
        renderer->m_GPUQueries[1] = m_RHI->m_NvrhiDevice->createTimerQuery();
    }

    m_GPUQueries[0] = m_RHI->m_NvrhiDevice->createTimerQuery();
    m_GPUQueries[1] = m_RHI->m_NvrhiDevice->createTimerQuery();

    // Load scene (if configured) after all renderer resources are ready
    if (!m_Scene.LoadScene())
    {
        SDL_Log("[Init] Failed to load scene");
        Shutdown();
        return false;
    }

    ExecutePendingCommandLists();

    return true;
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

        if (!m_RHI->AcquireNextSwapchainImage(&m_CurrentSwapchainImageIdx))
        {
            SDL_LOG_ASSERT_FAIL("AcquireNextSwapchainImage failed", "[Run ] AcquireNextSwapchainImage failed");
            break;
        }

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
            }
        }

        {
            PROFILE_SCOPED("Garbage Collection");
            m_RHI->m_NvrhiDevice->runGarbageCollection();
        }

        // Prepare ImGui UI (NewFrame + UI creation + ImGui::Render)
        m_ImGuiLayer.UpdateFrame();

        // Update camera (camera retrieves frame time internally)
        m_Camera.Update();

        const int readIndex = m_FrameNumber % 2;
        const int writeIndex = (m_FrameNumber + 1) % 2;

        if (m_RHI->m_NvrhiDevice->pollTimerQuery(m_GPUQueries[readIndex]))
        {
            m_GPUTime = SimpleTimer::SecondsToMilliseconds(m_RHI->m_NvrhiDevice->getTimerQueryTime(m_GPUQueries[readIndex]));
            m_RHI->m_NvrhiDevice->resetTimerQuery(m_GPUQueries[readIndex]);
        }

        {
            ScopedCommandList cmd{ "GPU Frame Begin" };
            cmd->beginTimerQuery(m_GPUQueries[writeIndex]);
        }

        // define macro for render pass below
        #define ADD_RENDER_PASS(rendererName) \
        { \
            extern IRenderer* rendererName; \
            PROFILE_SCOPED(rendererName->GetName()) \
            if (m_RHI->m_NvrhiDevice->pollTimerQuery(rendererName->m_GPUQueries[readIndex])) \
            { \
                rendererName->m_GPUTime = SimpleTimer::SecondsToMilliseconds(m_RHI->m_NvrhiDevice->getTimerQueryTime(rendererName->m_GPUQueries[readIndex])); \
                m_RHI->m_NvrhiDevice->resetTimerQuery(rendererName->m_GPUQueries[readIndex]); \
            } \
            SimpleTimer cpuTimer; \
            ScopedCommandList cmd{ rendererName->GetName() }; \
            cmd->beginTimerQuery(rendererName->m_GPUQueries[writeIndex]); \
            rendererName->Render(cmd); \
            cmd->endTimerQuery(rendererName->m_GPUQueries[writeIndex]); \
            rendererName->m_CPUTime = static_cast<float>(cpuTimer.TotalMilliseconds()); \
        }

        ADD_RENDER_PASS(g_ClearRenderer);
        ADD_RENDER_PASS(g_BasePassRenderer);
        ADD_RENDER_PASS(g_ImGuiRenderer);

        #undef ADD_RENDER_PASS

        {
            ScopedCommandList cmd{ "GPU Frame End" };
            cmd->endTimerQuery(m_GPUQueries[writeIndex]);
        }

        // Wait for GPU to finish all work before presenting
        {
            PROFILE_SCOPED("WaitForIdle");
            m_RHI->m_NvrhiDevice->waitForIdle();
        }

        // Execute any queued GPU work in submission order
        ExecutePendingCommandLists();

        // Present swapchain
        if (!m_RHI->PresentSwapchain(m_CurrentSwapchainImageIdx))
        {
            SDL_LOG_ASSERT_FAIL("PresentSwapchain failed", "[Run ] PresentSwapchain failed");
            break;
        }

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

    SDL_assert(m_PendingCommandLists.empty() && "Pending command lists should be empty on device destruction");
    m_PendingCommandLists.clear();
    m_CommandListFreeList.clear();

    m_RHI->m_NvrhiDevice->waitForIdle();
    m_RHI->m_NvrhiDevice->runGarbageCollection();

    m_ImGuiLayer.Shutdown();
    CommonResources::GetInstance().Shutdown();

    // Shutdown global bindless texture system
    m_GlobalTextureDescriptorTable = nullptr;
    m_GlobalTextureBindingLayout = nullptr;
    m_NextTextureIndex = 0;

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
    DestroyDepthTextures();

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

nvrhi::BindingLayoutHandle Renderer::GetOrCreateBindingLayoutFromBindingSetDesc(const nvrhi::BindingSetDesc& setDesc, nvrhi::ShaderType visibility)
{
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = visibility;
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

bool Renderer::InitializeGlobalBindlessTextures()
{
    // Create bindless layout for global textures
    nvrhi::BindlessLayoutDesc bindlessDesc;
    bindlessDesc.visibility = nvrhi::ShaderType::All;
    bindlessDesc.maxCapacity = 1024; // Large capacity for many textures
    bindlessDesc.layoutType = nvrhi::BindlessLayoutDesc::LayoutType::MutableSrvUavCbv;

    m_GlobalTextureBindingLayout = GetOrCreateBindlessLayout(bindlessDesc);
    if (!m_GlobalTextureBindingLayout)
    {
        SDL_LOG_ASSERT_FAIL("Failed to create global bindless layout for textures", "[Renderer] Failed to create global bindless layout for textures");
        return false;
    }

    // Create descriptor table
    m_GlobalTextureDescriptorTable = m_RHI->m_NvrhiDevice->createDescriptorTable(m_GlobalTextureBindingLayout);
    if (!m_GlobalTextureDescriptorTable)
    {
        SDL_LOG_ASSERT_FAIL("Failed to create global texture descriptor table", "[Renderer] Failed to create global texture descriptor table");
        return false;
    }
    
    SDL_Log("[Renderer] Global bindless texture system initialized");
    return true;
}

uint32_t Renderer::RegisterTexture(nvrhi::TextureHandle texture)
{
    if (!texture || !m_GlobalTextureDescriptorTable)
    {
        SDL_LOG_ASSERT_FAIL("Invalid texture or descriptor table not initialized", "[Renderer] Invalid texture or descriptor table not initialized");
        return UINT32_MAX;
    }

    const uint32_t index = m_NextTextureIndex++;
    
    const nvrhi::BindingSetItem item = nvrhi::BindingSetItem::Texture_SRV(index, texture);
    if (!m_RHI->m_NvrhiDevice->writeDescriptorTable(m_GlobalTextureDescriptorTable, item))
    {
        SDL_LOG_ASSERT_FAIL("Failed to register texture in global descriptor table", "[Renderer] Failed to register texture at index %u", index);
        return UINT32_MAX;
    }

    SDL_Log("[Renderer] Registered texture (%s) at index %u", texture->getDesc().debugName.c_str(), index);
    return index;
}

void Renderer::HashPipelineCommonState(size_t& h, const nvrhi::RenderState& renderState, const nvrhi::FramebufferInfoEx& fbInfo, const nvrhi::BindingLayoutVector& bindingLayouts)
{
    // Raster State
    const auto& rs = renderState.rasterState;
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
    const auto& dss = renderState.depthStencilState;
    h = h * 1099511628211u + std::hash<bool>()(dss.depthTestEnable);
    h = h * 1099511628211u + std::hash<bool>()(dss.depthWriteEnable);
    h = h * 1099511628211u + std::hash<int>()((int)dss.depthFunc);
    h = h * 1099511628211u + std::hash<bool>()(dss.stencilEnable);
    h = h * 1099511628211u + std::hash<uint32_t>()(dss.stencilReadMask);
    h = h * 1099511628211u + std::hash<uint32_t>()(dss.stencilWriteMask);

    // Blend State
    const auto& bs = renderState.blendState;
    h = h * 1099511628211u + std::hash<bool>()(bs.alphaToCoverageEnable);
    for (const auto& target : bs.targets)
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
    for (auto format : fbInfo.colorFormats)
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

    auto it = m_GraphicsPipelineCache.find(h);
    if (it != m_GraphicsPipelineCache.end())
        return it->second;

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

    auto it = m_MeshletPipelineCache.find(h);
    if (it != m_MeshletPipelineCache.end())
        return it->second;

    // Create pipeline and cache it
    nvrhi::MeshletPipelineHandle pipeline = m_RHI->m_NvrhiDevice->createMeshletPipeline(pipelineDesc, fbInfo);
    SDL_assert(pipeline && "Failed to create meshlet pipeline");
    if (pipeline)
    {
        m_MeshletPipelineCache.emplace(h, pipeline);
    }

    return pipeline;
}

nvrhi::CommandListHandle Renderer::AcquireCommandList(std::string_view markerName)
{
    nvrhi::CommandListHandle handle;
    if (!m_CommandListFreeList.empty())
    {
        handle = m_CommandListFreeList.back();
        m_CommandListFreeList.pop_back();
    }
    else
    {
        const nvrhi::CommandListParameters params{.queueType = nvrhi::CommandQueue::Graphics};
        handle = m_RHI->m_NvrhiDevice->createCommandList(params);
    }

    SDL_assert(handle && "Failed to acquire command list");

    handle->open();
    handle->beginMarker(markerName.data());

    return handle;
}

void Renderer::SubmitCommandList(const nvrhi::CommandListHandle& commandList)
{
    SDL_assert(commandList && "Invalid command list submitted");

    commandList->endMarker();
    commandList->close();
    m_PendingCommandLists.push_back(commandList);
}

void Renderer::ExecutePendingCommandLists()
{
    PROFILE_FUNCTION();

    if (!m_PendingCommandLists.empty())
    {
        std::vector<nvrhi::ICommandList*> rawLists;
        rawLists.reserve(m_PendingCommandLists.size());
        for (const nvrhi::CommandListHandle& handle : m_PendingCommandLists)
        {
            rawLists.push_back(handle.Get());
        }

        m_RHI->m_NvrhiDevice->executeCommandLists(rawLists.data(), rawLists.size());
        m_PendingCommandLists.clear();
    }
}

bool Renderer::CreateDepthTextures()
{
    SDL_Log("[Init] Creating Depth textures");

    // Create a depth texture for the main framebuffer
    nvrhi::TextureDesc depthDesc;
    depthDesc.width = m_RHI->m_SwapchainExtent.x;
    depthDesc.height = m_RHI->m_SwapchainExtent.y;
    depthDesc.format = nvrhi::Format::D32;
    depthDesc.debugName = "DepthBuffer";
    depthDesc.isRenderTarget = true;
    depthDesc.isUAV = false;
    depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;

    m_DepthTexture = m_RHI->m_NvrhiDevice->createTexture(depthDesc);
    if (!m_DepthTexture)
    {
        SDL_LOG_ASSERT_FAIL("Failed to create depth texture", "[Init] Failed to create depth texture");
        return false;
    }

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
        return false;
    }

    // Create atomic counter buffer for SPD
    nvrhi::BufferDesc counterDesc;
    counterDesc.structStride = sizeof(uint32_t);
    counterDesc.byteSize = sizeof(uint32_t);
    counterDesc.canHaveUAVs = true;
    counterDesc.debugName = "SPD Atomic Counter";
    counterDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    m_SPDAtomicCounter = m_RHI->m_NvrhiDevice->createBuffer(counterDesc);

    ScopedCommandList cmd{ "HZB_Clear" };
    cmd->clearTextureFloat(m_HZBTexture, nvrhi::AllSubresources, DEPTH_FAR);

    SDL_Log("[Init] Created HZB texture (%ux%u, %u mips)", hzbWidth, hzbHeight, mipLevels);
    return true;
}

void Renderer::DestroyDepthTextures()
{
    SDL_Log("[Shutdown] Destroying Depth textures");

    m_DepthTexture = nullptr;
    m_HZBTexture = nullptr;
    m_SPDAtomicCounter = nullptr;
}

// Pipeline caching
nvrhi::ComputePipelineHandle Renderer::GetOrCreateComputePipeline(nvrhi::ShaderHandle shader, nvrhi::BindingLayoutHandle bindingLayout)
{
    // Hash relevant pipeline properties: CS shader handle, binding layout pointer
    size_t h = 1469598103934665603ull;
    h = h * 1099511628211u + std::hash<const void*>()(shader.Get());
    h = h * 1099511628211u + std::hash<const void*>()(bindingLayout.Get());

    auto it = m_ComputePipelineCache.find(h);
    if (it != m_ComputePipelineCache.end())
        return it->second;

    // Create pipeline and cache it
    nvrhi::ComputePipelineDesc desc;
    desc.CS = shader;
    desc.bindingLayouts = { bindingLayout };
    nvrhi::ComputePipelineHandle pipeline = m_RHI->m_NvrhiDevice->createComputePipeline(desc);
    SDL_assert(pipeline && "Failed to create compute pipeline");
    if (pipeline)
    {
        m_ComputePipelineCache.emplace(h, pipeline);
    }

    return pipeline;
}

int main(int argc, char* argv[])
{
    Config::ParseCommandLine(argc, argv);

    Renderer renderer{};
    Renderer::SetInstance(&renderer);
    if (!renderer.Initialize())
        return 1;

    renderer.Run();
    renderer.Shutdown();
    Renderer::SetInstance(nullptr);
    return 0;
}
