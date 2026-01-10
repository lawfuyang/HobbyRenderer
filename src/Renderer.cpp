#include "Renderer.h"
#include "Utilities.h"
#include "Config.h"

#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

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

bool Renderer::InitializeImGui()
{
    SDL_Log("[Init] Initializing ImGui");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForVulkan(m_Window))
    {
        SDL_Log("[Init] Failed to initialize ImGui SDL3 backend");
        SDL_assert(false && "ImGui_ImplSDL3_InitForVulkan failed");
        return false;
    }

    SDL_Log("[Init] ImGui initialized successfully");
    return true;
}

void Renderer::ShutdownImGui()
{
    SDL_Log("[Shutdown] Shutting down ImGui");
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void Renderer::RenderImGuiFrame()
{
    return; // TODO

    // Start ImGui frame
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // Top menu bar
    if (ImGui::BeginMainMenuBar())
    {
        ImGui::Text("FPS: %.1f", m_FPS);
        ImGui::Text("Frame Time: %.3f ms", m_FrameTime);
        ImGui::EndMainMenuBar();
    }

    // Demo window
    if (ImGui::Begin("Property Grid"))
    {
        static bool s_ShowDemoWindow = false;
        if (ImGui::Checkbox("Show Demo Window", &s_ShowDemoWindow))
        {
            ImGui::ShowDemoWindow();
        }
        
        ImGui::End();
    }

    // Rendering
    ImGui::Render();
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

    if (!InitializeImGui())
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
            ImGui_ImplSDL3_ProcessEvent(&event);

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
        RenderImGuiFrame();

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

    ShutdownImGui();
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
    if (!renderer.Initialize())
        return 1;

    renderer.Run();
    renderer.Shutdown();
    return 0;
}
