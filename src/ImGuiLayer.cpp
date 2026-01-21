#include "pch.h"
#include "ImGuiLayer.h"
#include "Renderer.h"

// Include shared structs
#include "shaders/ShaderShared.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include "CommonResources.h"

bool ImGuiLayer::Initialize()
{
    SDL_Window* window = Renderer::GetInstance()->m_Window;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForVulkan(window))
    {
        SDL_LOG_ASSERT_FAIL("ImGui_ImplSDL3_InitForVulkan failed", "[Init] Failed to initialize ImGui SDL3 backend");
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

void ImGuiLayer::UpdateFrame()
{
    // Build ImGui UI and end with ImGui::Render(); rendering happens in ImGuiRenderer::Render
    Renderer* renderer = Renderer::GetInstance();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    if (ImGui::BeginMainMenuBar())
    {
        ImGui::Text("FPS: %.1f", renderer->m_FPS);
        ImGui::Text("Frame Time: %.3f ms", renderer->m_FrameTime);
        ImGui::EndMainMenuBar();
    }

    static bool s_ShowDemoWindow = false;
    if (s_ShowDemoWindow)
    {
        ImGui::ShowDemoWindow(&s_ShowDemoWindow);
    }

    if (ImGui::Begin("Property Grid", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        //ImGui::Checkbox("Show Demo Window", &s_ShowDemoWindow);

        // Target FPS control
        ImGui::DragInt("Target FPS", (int*)&renderer->m_TargetFPS, 1.0f, 10, 200);

        // Camera controls
        if (ImGui::TreeNode("Camera"))
        {
            ImGui::DragFloat("Move Speed", &renderer->m_Camera.m_MoveSpeed, 0.1f, 0.0f, 100.0f);
            ImGui::DragFloat("Mouse Sensitivity", &renderer->m_Camera.m_MouseSensitivity, 0.0005f, 0.0f, 1.0f, "%.4f");

            // Display camera position
            Vector3 pos = renderer->m_Camera.GetPosition();
            ImGui::Text("Position: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
            
            // Display forward vector (from view matrix, third column)
            Matrix view = renderer->m_Camera.GetViewMatrix();
            Vector3 fwd = { view._13, view._23, view._33 };
            ImGui::Text("Forward: %.2f, %.2f, %.2f", fwd.x, fwd.y, fwd.z);
            
            // GLTF Camera selection
            if (!renderer->m_Scene.m_Cameras.empty())
            {
                std::vector<const char*> cameraNames;
                for (const auto& cam : renderer->m_Scene.m_Cameras)
                {
                    cameraNames.push_back(cam.m_Name.empty() ? "Unnamed Camera" : cam.m_Name.c_str());
                }
                if (ImGui::Combo("GLTF Camera", &renderer->m_SelectedCameraIndex, cameraNames.data(), static_cast<int>(cameraNames.size())))
                {
                    if (renderer->m_SelectedCameraIndex >= 0 && renderer->m_SelectedCameraIndex < static_cast<int>(renderer->m_Scene.m_Cameras.size()))
                    {
                        const Scene::Camera& selectedCam = renderer->m_Scene.m_Cameras[renderer->m_SelectedCameraIndex];
                        renderer->SetCameraFromSceneCamera(selectedCam);
                    }
                }
            }

            if (ImGui::Button("Reset Camera"))
            {
                if (renderer->m_SelectedCameraIndex >= 0 && renderer->m_SelectedCameraIndex < static_cast<int>(renderer->m_Scene.m_Cameras.size()))
                {
                    const Scene::Camera& selectedCam = renderer->m_Scene.m_Cameras[renderer->m_SelectedCameraIndex];
                    renderer->SetCameraFromSceneCamera(selectedCam);
                }
                else
                {
                    // Reset to 0,0,0 position and default orientation
                    renderer->m_Camera.Reset();
                }
            }

            ImGui::TreePop();
        }

        // Directional Light controls
        if (ImGui::TreeNode("Directional Light"))
        {
            ImGui::DragFloat("Yaw", &renderer->m_Scene.m_DirectionalLight.yaw, 0.01f, -PI, PI);
            ImGui::DragFloat("Pitch", &renderer->m_Scene.m_DirectionalLight.pitch, 0.01f, -PI * 0.5f, PI * 0.5f);
            ImGui::DragFloat("Lux", &renderer->m_Scene.m_DirectionalLight.intensity, 100.0f, 0.0f, 200000.0f);

            ImGui::TreePop();
        }

        // Culling controls
        if (ImGui::TreeNode("Culling"))
        {
            ImGui::Checkbox("Enable Frustum Culling", &renderer->m_EnableFrustumCulling);
            ImGui::Checkbox("Enable Occlusion Culling", &renderer->m_EnableOcclusionCulling);

            bool prevFreeze = renderer->m_FreezeCullingCamera;
            ImGui::Checkbox("Freeze Culling Camera", &renderer->m_FreezeCullingCamera);
            if (!prevFreeze && renderer->m_FreezeCullingCamera)
            {
                renderer->m_FrozenCullingViewMatrix = renderer->m_Camera.GetViewMatrix();
            }

            ImGui::TreePop();
        }

        // Timings
        if (ImGui::TreeNode("Profiler"))
        {
            if (ImGui::Button("Profiler Dump"))
            {
                const std::string dumpPath = (std::filesystem::path{ SDL_GetBasePath() } / "profiler_dump.html").string();
                MicroProfileDumpFileImmediately(dumpPath.c_str(), nullptr, nullptr);
            }

            if (ImGui::BeginTable("TimingsTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Pass");
                ImGui::TableSetupColumn("CPU (ms)");
                ImGui::TableSetupColumn("GPU (ms)");
                ImGui::TableHeadersRow();
                for (auto& r : renderer->m_Renderers)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%s", r->GetName());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.3f", r->m_CPUTime);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.3f", r->m_GPUTime);
                }
                ImGui::EndTable();
            }

            ImGui::TreePop();
        }

        // Pipeline statistics
        if (ImGui::TreeNode("Main View Pipeline Statistics"))
        {
            const auto& stats = renderer->m_MainViewPipelineStatistics;
            ImGui::Text("Input Assembly Vertices: %llu", stats.IAVertices);
            ImGui::Text("Input Assembly Primitives: %llu", stats.IAPrimitives);
            ImGui::Text("Vertex Shader Invocations: %llu", stats.VSInvocations);
            ImGui::Text("Geometry Shader Invocations: %llu", stats.GSInvocations);
            ImGui::Text("Geometry Shader Primitives: %llu", stats.GSPrimitives);
            ImGui::Text("Clipping Invocations: %llu", stats.CInvocations);
            ImGui::Text("Clipping Primitives: %llu", stats.CPrimitives);
            ImGui::Text("Pixel Shader Invocations: %llu", stats.PSInvocations);
            ImGui::Text("Hull Shader Invocations: %llu", stats.HSInvocations);
            ImGui::Text("Domain Shader Invocations: %llu", stats.DSInvocations);
            ImGui::Text("Compute Shader Invocations: %llu", stats.CSInvocations);
            ImGui::Text("Amplification Shader Invocations: %llu", stats.ASInvocations);
            ImGui::Text("Mesh Shader Invocations: %llu", stats.MSInvocations);
            ImGui::Text("Mesh Shader Primitives: %llu", stats.MSPrimitives);
            ImGui::TreePop();
        }
    }
    ImGui::End();

    ImGui::Render();
}


