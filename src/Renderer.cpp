#include "Renderer.h"
#include "Utilities.h"
#include "Config.h"

#include <SDL3/SDL_main.h>

namespace
{
    class NvrhiErrorCallback : public nvrhi::IMessageCallback
    {
    public:
        void message(nvrhi::MessageSeverity severity, const char* messageText) override
        {
            const char* severityStr = "Unknown";
            switch (severity)
            {
                case nvrhi::MessageSeverity::Info:
                    severityStr = "Info";
                    break;
                case nvrhi::MessageSeverity::Warning:
                    severityStr = "Warning";
                    break;
                case nvrhi::MessageSeverity::Error:
                    severityStr = "Error";
                    break;
                case nvrhi::MessageSeverity::Fatal:
                    severityStr = "Fatal";
                    break;
            }
            SDL_Log("[NVRHI %s] %s", severityStr, messageText);

            // Assert on warning and above
            if (severity >= nvrhi::MessageSeverity::Warning)
            {
                SDL_assert(false && messageText);
            }
        }
    };

    // Read a binary file into memory
    std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
    {
        std::ifstream file{path, std::ios::binary | std::ios::ate};
        if (!file.is_open())
        {
            SDL_Log("[Shader] Failed to open file: %s", path.string().c_str());
            return {};
        }

        std::streamsize size = file.tellg();
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
        nvrhi::ShaderType shaderType = nvrhi::ShaderType::None;
    };

    // ShaderMake naming: <sourcefile_without_ext>_<EntryPoint>.spirv
    std::filesystem::path GetShaderOutputPath(const std::filesystem::path& sourcePath, std::string_view entryPoint)
    {
        std::filesystem::path filename = sourcePath.stem();
        std::string outName = filename.string() + "_" + std::string(entryPoint) + ".spirv";
        return std::filesystem::path{"bin"} / "shaders" / outName;
    }

    // Parse shaders.cfg to extract shader entries
    std::vector<ShaderMetadata> ParseShaderConfig(std::string_view configPath)
    {
        std::vector<ShaderMetadata> shaders;
        std::ifstream configFile{std::filesystem::path{configPath}};

        if (!configFile.is_open())
        {
            SDL_Log("[Shader] Failed to open config: %.*s", static_cast<int>(configPath.size()), configPath.data());
            return shaders;
        }

        std::string line;
        while (std::getline(configFile, line))
        {
            // Skip empty lines and comments
            if (line.empty() || line[0] == '/' || line[0] == '#')
                continue;

            // Trim leading/trailing whitespace
            size_t start = line.find_first_not_of(" \t\r\n");
            size_t end = line.find_last_not_of(" \t\r\n");
            if (start == std::string::npos)
                continue;

            line = line.substr(start, end - start + 1);

            // Parse: <shader_path> -T <profile> -E <entry> [other options]
            std::istringstream iss{line};
            std::string token;
            ShaderMetadata metadata;
            bool hasType = false;
            bool hasEntry = false;

            iss >> token;
            metadata.sourcePath = std::filesystem::path{token};

            while (iss >> token)
            {
                if (token == "-T" || token == "--profile")
                {
                    iss >> token;
                    if (token == "vs")
                        metadata.shaderType = nvrhi::ShaderType::Vertex;
                    else if (token == "ps")
                        metadata.shaderType = nvrhi::ShaderType::Pixel;
                    else if (token == "gs")
                        metadata.shaderType = nvrhi::ShaderType::Geometry;
                    else if (token == "cs")
                        metadata.shaderType = nvrhi::ShaderType::Compute;
                    else if (token == "hs")
                        metadata.shaderType = nvrhi::ShaderType::Hull;
                    else if (token == "ds")
                        metadata.shaderType = nvrhi::ShaderType::Domain;
                    hasType = true;
                }
                else if (token == "-E" || token == "--entryPoint")
                {
                    iss >> metadata.entryPoint;
                    hasEntry = true;
                }
            }

            if (hasType && hasEntry)
            {
                shaders.push_back(metadata);
                SDL_Log("[Shader] Parsed: %s (%s) -> entry: %s", metadata.sourcePath.generic_string().c_str(), 
                        metadata.shaderType == nvrhi::ShaderType::Vertex ? "VS" : 
                        metadata.shaderType == nvrhi::ShaderType::Pixel ? "PS" : "?",
                        metadata.entryPoint.c_str());
            }
        }

        SDL_Log("[Shader] Parsed %zu shader entries from config", shaders.size());
        return shaders;
    }

    void HandleInput(const SDL_Event& event)
    {
        (void)event;
    }

    void InitSDL()
    {
        SDL_Log("[Init] Starting SDL initialization");
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            SDL_Log("SDL_Init failed: %s", SDL_GetError());
            SDL_assert(false && "SDL_Init failed");
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
            SDL_Log("SDL_GetDisplayUsableBounds failed: %s", SDL_GetError());
            SDL_assert(false && "SDL_GetDisplayUsableBounds failed");
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
        SDL_Window* window = SDL_CreateWindow("Agentic Renderer", windowW, windowH, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!window)
        {
            SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
            SDL_assert(false && "SDL_CreateWindow failed");
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

    // Parse shaders.cfg to get list of shaders to load
    const std::filesystem::path configPath = std::filesystem::path{"src"} / "shaders" / "shaders.cfg";
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
        std::filesystem::path outputPath = GetShaderOutputPath(metadata.sourcePath, std::string_view{metadata.entryPoint});

        // Read the compiled binary
        std::vector<uint8_t> binary = ReadBinaryFile(outputPath);
        if (binary.empty())
        {
            SDL_Log("[Init] Failed to load compiled shader: %s", outputPath.generic_string().c_str());
            return false;
        }

        // Create shader descriptor
        nvrhi::ShaderDesc desc;
        desc.shaderType = metadata.shaderType;
        desc.debugName = outputPath.generic_string();

        // Create shader handle
        nvrhi::ShaderHandle handle = m_NvrhiDevice->createShader(desc, binary.data(), binary.size());
        if (!handle)
        {
            SDL_Log("[Init] Failed to create shader handle: %s", outputPath.generic_string().c_str());
            return false;
        }

        // Keyed by output stem (e.g., "imgui_VSMain") for easy retrieval
        std::string key = outputPath.stem().string();
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
    return {};
}

bool Renderer::Initialize()
{
    ScopedTimerLog initScope{"[Timing] Init phase:"};

    InitSDL();

    m_Window = CreateWindowScaled();
    if (!m_Window)
    {
        SDL_Quit();
        return false;
    }

    if (!m_RHI.Initialize(m_Window))
    {
        Shutdown();
        return false;
    }

    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);

    if (!m_RHI.CreateSwapchain(static_cast<uint32_t>(windowWidth), static_cast<uint32_t>(windowHeight)))
    {
        Shutdown();
        return false;
    }

    if (!CreateNvrhiDevice())
    {
        Shutdown();
        return false;
    }

    if (!CreateSwapchainTextures())
    {
        Shutdown();
        return false;
    }

    if (!LoadShaders())
    {
        Shutdown();
        return false;
    }

    if (!m_ImGuiLayer.Initialize(m_Window))
    {
        Shutdown();
        return false;
    }

    return true;
}

void Renderer::Run()
{
    ScopedTimerLog runScope{"[Timing] Run phase:"};

    SDL_Log("[Run ] Entering main loop");

    constexpr uint32_t kTargetFPS = 200;
    constexpr uint32_t kFrameDurationMs = SDL_MS_PER_SECOND / kTargetFPS;

    bool running = true;
    while (running)
    {
        const uint64_t frameStart = SDL_GetTicks();

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            m_ImGuiLayer.ProcessEvent(event);

            if (event.type == SDL_EVENT_QUIT)
            {
                SDL_Log("[Run ] Received quit event");
                running = false;
                break;
            }

            switch (event.type)
            {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_WHEEL:
                HandleInput(event);
                break;
            default:
                break;
            }
        }

        // Render ImGui frame
        m_ImGuiLayer.RenderFrame();

        const uint64_t frameTime = SDL_GetTicks() - frameStart;
        
        // Calculate frame time and FPS
        m_FrameTime = static_cast<double>(frameTime);
        if (m_FrameTime > 0.0)
            m_FPS = 1000.0 / m_FrameTime;

        if (frameTime < kFrameDurationMs)
        {
            SDL_Delay(static_cast<uint32_t>(kFrameDurationMs - frameTime));
        }
    }
}

void Renderer::Shutdown()
{
    ScopedTimerLog shutdownScope{"[Timing] Shutdown phase:"};

    m_ImGuiLayer.Shutdown();
    UnloadShaders();
    DestroySwapchainTextures();
    DestroyNvrhiDevice();
    m_RHI.Shutdown();

    if (m_Window)
    {
        SDL_DestroyWindow(m_Window);
        m_Window = nullptr;
    }

    SDL_Quit();
    SDL_Log("[Shutdown] Clean exit");
}

bool Renderer::CreateNvrhiDevice()
{
    SDL_Log("[Init] Creating NVRHI Vulkan device");

    static NvrhiErrorCallback errorCallback;

    nvrhi::vulkan::DeviceDesc deviceDesc;
    deviceDesc.errorCB = &errorCallback;
    deviceDesc.instance = static_cast<VkInstance>(m_RHI.m_Instance);
    deviceDesc.physicalDevice = m_RHI.m_PhysicalDevice;
    deviceDesc.device = m_RHI.m_Device;
    deviceDesc.graphicsQueue = m_RHI.m_GraphicsQueue;
    deviceDesc.graphicsQueueIndex = m_RHI.m_GraphicsQueueFamily;
    
    // Provide instance and device extensions for NVRHI to query support
    deviceDesc.instanceExtensions = const_cast<const char**>(m_RHI.GetInstanceExtensions().data());
    deviceDesc.numInstanceExtensions = m_RHI.GetInstanceExtensions().size();
    deviceDesc.deviceExtensions = const_cast<const char**>(m_RHI.GetDeviceExtensions().data());
    deviceDesc.numDeviceExtensions = m_RHI.GetDeviceExtensions().size();

    m_NvrhiDevice = nvrhi::vulkan::createDevice(deviceDesc);
    if (!m_NvrhiDevice)
    {
        SDL_Log("[Init] Failed to create NVRHI device");
        SDL_assert(false && "Failed to create NVRHI device");
        return false;
    }

    SDL_Log("[Init] NVRHI Vulkan device created successfully");

    // Wrap with validation layer if enabled
    if (Config::Get().m_EnableGPUValidation)
    {
        SDL_Log("[Init] Wrapping device with NVRHI validation layer");
        m_NvrhiDevice = nvrhi::validation::createValidationLayer(m_NvrhiDevice);
        SDL_Log("[Init] NVRHI validation layer enabled");
    }

    return true;
}

void Renderer::DestroyNvrhiDevice()
{
    if (m_NvrhiDevice)
    {
        SDL_Log("[Shutdown] Destroying NVRHI device");
        m_NvrhiDevice = nullptr;
    }
}

nvrhi::CommandListHandle Renderer::AcquireCommandList()
{
    if (!m_CommandListFreeList.empty())
    {
        const nvrhi::CommandListHandle handle = m_CommandListFreeList.back();
        m_CommandListFreeList.pop_back();
        return handle;
    }

    SDL_assert(m_NvrhiDevice && "NVRHI device is not initialized");

    nvrhi::CommandListParameters params{};
    params.queueType = nvrhi::CommandQueue::Graphics;
    return m_NvrhiDevice->createCommandList(params);
}

void Renderer::ReleaseCommandList(const nvrhi::CommandListHandle& commandList)
{
    if (!commandList)
    {
        return;
    }

    m_CommandListFreeList.push_back(commandList);
}

bool Renderer::CreateSwapchainTextures()
{
    SDL_Log("[Init] Creating NVRHI swap chain texture handles");

    // Convert VkFormat to nvrhi::Format
    const nvrhi::Format nvrhiFormat = GraphicRHI::VkFormatToNvrhiFormat(m_RHI.m_SwapchainFormat);
    if (nvrhiFormat == nvrhi::Format::UNKNOWN)
    {
        SDL_Log("[Init] Unsupported swapchain format: %d", m_RHI.m_SwapchainFormat);
        SDL_assert(false && "Unsupported swapchain format");
        return false;
    }

    for (size_t i = 0; i < GraphicRHI::SwapchainImageCount; ++i)
    {
        nvrhi::TextureDesc textureDesc;
        textureDesc.width = m_RHI.m_SwapchainExtent.width;
        textureDesc.height = m_RHI.m_SwapchainExtent.height;
        textureDesc.format = nvrhiFormat;
        textureDesc.debugName = "SwapchainImage_" + std::to_string(i);
        textureDesc.isRenderTarget = true;
        textureDesc.isUAV = false;
        textureDesc.initialState = nvrhi::ResourceStates::Present;
        textureDesc.keepInitialState = true;

        nvrhi::TextureHandle texture = m_NvrhiDevice->createHandleForNativeTexture(
            nvrhi::ObjectTypes::VK_Image,
            nvrhi::Object(m_RHI.m_SwapchainImages[i]),
            textureDesc
        );

        if (!texture)
        {
            SDL_Log("[Init] Failed to create NVRHI texture handle for swap chain image %zu", i);
            SDL_assert(false && "Failed to create NVRHI texture handle");
            return false;
        }

        m_SwapchainTextures[i] = texture;
    }

    SDL_Log("[Init] Created %u NVRHI swap chain texture handles", GraphicRHI::SwapchainImageCount);
    return true;
}

void Renderer::DestroySwapchainTextures()
{
    SDL_Log("[Shutdown] Destroying swap chain texture handles");
    for (size_t i = 0; i < GraphicRHI::SwapchainImageCount; ++i)
    {
        m_SwapchainTextures[i] = nullptr;
    }
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
