#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <nvrhi/vulkan.h>
#include <nvrhi/validation.h>

#ifndef VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
    #define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#endif
#include <vulkan/vulkan.hpp>

// Define the Vulkan dynamic dispatcher - this needs to occur in exactly one cpp file in the program.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace
{
    constexpr uint32_t kTargetFPS = 200;
    constexpr uint32_t kFrameDurationMs = SDL_MS_PER_SECOND / kTargetFPS;

    VkInstance CreateVulkanInstance()
    {
        SDL_Log("[Init] Creating Vulkan instance");

        // Load Vulkan library through SDL3
        if (!SDL_Vulkan_LoadLibrary(nullptr))
        {
            SDL_Log("SDL_Vulkan_LoadLibrary failed: %s", SDL_GetError());
            SDL_assert(false && "SDL_Vulkan_LoadLibrary failed");
            return VK_NULL_HANDLE;
        }

        // Initialize the dynamic loader
        vk::detail::DynamicLoader dl;
        PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = 
            dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
        VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

        // Get required instance extensions from SDL3
        uint32_t sdlExtensionCount = 0;
        const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
        if (!sdlExtensions)
        {
            SDL_Log("SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
            SDL_assert(false && "SDL_Vulkan_GetInstanceExtensions failed");
            return VK_NULL_HANDLE;
        }

        SDL_Log("[Init] SDL3 requires %u Vulkan instance extensions:", sdlExtensionCount);
        std::vector<const char*> extensions;
        for (uint32_t i = 0; i < sdlExtensionCount; ++i)
        {
            SDL_Log("[Init]   - %s", sdlExtensions[i]);
            extensions.push_back(sdlExtensions[i]);
        }

        // Application info
        vk::ApplicationInfo appInfo{};
        appInfo.pApplicationName = "Agentic Renderer";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "No Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_3;

        // Instance create info
        vk::InstanceCreateInfo createInfo{};
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.enabledLayerCount = 0; // No validation layers for now

        // Create instance
        vk::Instance instance = vk::createInstance(createInfo);
        if (!instance)
        {
            SDL_Log("vkCreateInstance failed");
            SDL_assert(false && "vkCreateInstance failed");
            return VK_NULL_HANDLE;
        }

        // Initialize the dispatcher with the instance
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);

        SDL_Log("[Init] Vulkan instance created successfully");
        return static_cast<VkInstance>(instance);
    }

    void DestroyVulkanInstance(VkInstance instance)
    {
        if (instance != VK_NULL_HANDLE)
        {
            SDL_Log("[Shutdown] Destroying Vulkan instance");
            vk::Instance vkInstance = static_cast<vk::Instance>(instance);
            vkInstance.destroy();
            SDL_Vulkan_UnloadLibrary();
        }
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

    std::pair<int, int> ChooseWindowSize()
    {
        const SDL_DisplayID primaryDisplay = SDL_GetPrimaryDisplay();
        SDL_Rect usableBounds{};
        if (!SDL_GetDisplayUsableBounds(primaryDisplay, &usableBounds))
        {
            SDL_Log("SDL_GetDisplayUsableBounds failed: %s", SDL_GetError());
            SDL_assert(false && "SDL_GetDisplayUsableBounds failed");
            return {1280, 720};
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

        const std::array<std::pair<int, int>, 5> kStandard16x9 = {
            std::pair<int, int>{3840, 2160},
            std::pair<int, int>{2560, 1440},
            std::pair<int, int>{1920, 1080},
            std::pair<int, int>{1600, 900},
            std::pair<int, int>{1280, 720},
        };

        int windowW = maxFitW;
        int windowH = maxFitH;
        int firstFitIndex = -1;
        for (int i = 0; i < static_cast<int>(kStandard16x9.size()); ++i)
        {
            if (kStandard16x9[i].first <= maxFitW && kStandard16x9[i].second <= maxFitH)
            {
                firstFitIndex = i;
                break;
            }
        }

        if (firstFitIndex >= 0)
        {
            int chosenIndex = firstFitIndex;
            const bool fillsUsableWidth  = kStandard16x9[firstFitIndex].first  == maxFitW;
            const bool fillsUsableHeight = kStandard16x9[firstFitIndex].second == maxFitH;
            if (fillsUsableWidth && fillsUsableHeight && firstFitIndex + 1 < static_cast<int>(kStandard16x9.size()))
            {
                chosenIndex = firstFitIndex + 1; // step down only when we exactly fill usable space
            }

            windowW = kStandard16x9[chosenIndex].first;
            windowH = kStandard16x9[chosenIndex].second;
        }

        SDL_Log("[Init] Usable bounds: %dx%d, max 16:9 fit: %dx%d, chosen: %dx%d", usableBounds.w, usableBounds.h, maxFitW, maxFitH, windowW, windowH);
        return {windowW, windowH};
    }

    SDL_Window* CreateWindowScaled()
    {
        const auto [windowW, windowH] = ChooseWindowSize();

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

    void Shutdown(SDL_Window* window, VkInstance instance)
    {
        SDL_Log("[Shutdown] Destroying window and quitting SDL");
        
        DestroyVulkanInstance(instance);
        
        if (window)
        {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
        SDL_Log("[Shutdown] Clean exit");
    }
}

int main(int /*argc*/, char* /*argv*/[])
{
    InitSDL();

    SDL_Window* window = CreateWindowScaled();
    VkInstance vkInstance = CreateVulkanInstance();

    SDL_Log("[Run ] Entering main loop");

    bool running = true;
    while (running)
    {
        const uint64_t frameStart = SDL_GetTicks();

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
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

        const uint64_t frameTime = SDL_GetTicks() - frameStart;
        if (frameTime < kFrameDurationMs)
        {
            SDL_Delay(static_cast<uint32_t>(kFrameDurationMs - frameTime));
        }
    }

    Shutdown(window, vkInstance);
    return 0;
}