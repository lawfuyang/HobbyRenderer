#include "Renderer.h"
#include "Utilities.h"

namespace
{
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
}

void Renderer::Shutdown()
{
    ScopedTimerLog shutdownScope{"[Timing] Shutdown phase:"};

    m_RHI.Shutdown();

    if (m_Window)
    {
        SDL_DestroyWindow(m_Window);
        m_Window = nullptr;
    }

    SDL_Quit();
    SDL_Log("[Shutdown] Clean exit");
}

int main(int /*argc*/, char* /*argv*/[])
{
    Renderer renderer{};
    if (!renderer.Initialize())
        return 1;

    renderer.Run();
    renderer.Shutdown();
    return 0;
}
