#include "pch.h"
#include "ImGuiLayer.h"
#include "Renderer.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

bool ImGuiLayer::Initialize(SDL_Window* window)
{
    m_Window = window;

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

void ImGuiLayer::Shutdown()
{
    SDL_Log("[Shutdown] Shutting down ImGui");
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiLayer::ProcessEvent(const SDL_Event& event)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiLayer::RenderFrame()
{
    return; // TODO
    
    Renderer* renderer = Renderer::GetInstance();

    const double fps = renderer->m_FPS;
    const double frameTime = renderer->m_FrameTime;

    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    if (ImGui::BeginMainMenuBar())
    {
        ImGui::Text("FPS: %.1f", fps);
        ImGui::Text("Frame Time: %.3f ms", frameTime);
        ImGui::EndMainMenuBar();
    }

    if (ImGui::Begin("Property Grid"))
    {
        static bool s_ShowDemoWindow = false;
        if (ImGui::Checkbox("Show Demo Window", &s_ShowDemoWindow))
        {
            ImGui::ShowDemoWindow();
        }

        ImGui::End();
    }

    ImGui::Render();
}
