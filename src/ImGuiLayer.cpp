#include "pch.h"
#include "ImGuiLayer.h"
#include "Renderer.h"

// Include shared structs
#include "shaders/ShaderShared.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include "CommonResources.h"

void ImGuiLayer::Initialize()
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
        return;
    }

    SDL_Log("[Init] ImGui initialized successfully");
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
    PROFILE_FUNCTION();
    
    // Build ImGui UI and end with ImGui::Render(); rendering happens in ImGuiRenderer::Render
    Renderer* renderer = Renderer::GetInstance();
    Scene& scene = renderer->m_Scene;
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    if (ImGui::BeginMainMenuBar())
    {
        ImGui::Text("FPS: %.1f", renderer->m_FPS);
        ImGui::Separator();
        ImGui::Text("CPU Time: %.3f ms", renderer->m_FrameTime);
        ImGui::Separator();
        ImGui::Text("VRAM: %.1f MB", renderer->m_RHI->GetVRAMUsageMB());
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

        // Rendering options
        if (ImGui::TreeNode("Rendering"))
        {
            static const char* kRenderingModes[] = { "Normal", "Image Based Lighting", "Reference Pathtracer" };
            int currentMode = static_cast<int>(renderer->m_Mode);
            if (ImGui::Combo("Rendering Mode", &currentMode, kRenderingModes, IM_ARRAYSIZE(kRenderingModes)))
            {
                renderer->m_Mode = static_cast<RenderingMode>(currentMode);
            }

            ImGui::Checkbox("Use Meshlet Rendering", &renderer->m_UseMeshletRendering);
            ImGui::Checkbox("Enable RT Shadows", &renderer->m_EnableRTShadows);
            ImGui::Checkbox("Enable Sky", &renderer->m_EnableSky);

            static const char* kDebugModes[] = {
                "None", "Instances", "Meshlets", "World Normals", "Albedo", "Roughness", "Metallic", "Emissive", "LOD", "Motion Vectors"
            };
            ImGui::Combo("Debug Mode", &renderer->m_DebugMode, kDebugModes, IM_ARRAYSIZE(kDebugModes));

            const char* lodNames[] = { "Auto", "LOD 0", "LOD 1", "LOD 2", "LOD 3", "LOD 4", "LOD 5", "LOD 6", "LOD 7" };
            int forcedLODIdx = renderer->m_ForcedLOD + 1;
            if (ImGui::SliderInt("Forced LOD", &forcedLODIdx, 0, MAX_LOD_COUNT))
            {
                renderer->m_ForcedLOD = forcedLODIdx - 1;
            }
            ImGui::SameLine();
            ImGui::Text("%s", lodNames[forcedLODIdx]);

            ImGui::Checkbox("Enable Animations", &renderer->m_EnableAnimations);

            ImGui::TreePop();
        }

        // Post-processing settings
        if (ImGui::TreeNode("Post-Processing"))
        {
            ImGui::Checkbox("Enable Bloom", &renderer->m_EnableBloom);
            ImGui::Checkbox("Debug Bloom Only", &renderer->m_DebugBloom);
            ImGui::DragFloat("Bloom Intensity", &renderer->m_BloomIntensity, 0.001f, 0.0f, 1.0f);
            ImGui::DragFloat("Bloom Knee", &renderer->m_BloomKnee, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Bloom Upsample Radius", &renderer->m_UpsampleRadius, 0.01f, 0.1f, 2.0f);

            ImGui::Separator();

            ImGui::Checkbox("Auto Exposure", &renderer->m_EnableAutoExposure);
            if (renderer->m_EnableAutoExposure)
            {
                ImGui::DragFloat("EV Min", &renderer->m_Camera.m_ExposureValueMin, 0.1f, -20.0f, 20.0f);
                ImGui::DragFloat("EV Max", &renderer->m_Camera.m_ExposureValueMax, 0.1f, -20.0f, 20.0f);
                ImGui::DragFloat("Adaptation Speed", &renderer->m_AdaptationSpeed, 0.1f, 0.0f, 20.0f);
            }
            else
            {
                ImGui::DragFloat("Exposure Value (EV100)", &renderer->m_Camera.m_ExposureValue, 0.1f, -20.0f, 20.0f);
            }
            ImGui::DragFloat("Exposure Compensation", &renderer->m_Camera.m_ExposureCompensation, 0.1f, -10.0f, 10.0f);
            ImGui::Text("Current Multiplier: %.4f", renderer->m_Camera.m_Exposure);

            ImGui::TreePop();
        }

        // Lights controls
        if (ImGui::TreeNode("Lights"))
        {
            // index 0 is guaranteed to be the sun light
            Scene::Light& sun = scene.m_Lights[0];
            if (ImGui::TreeNode("Sun Orientation"))
            {
                bool changed = false;
                changed |= ImGui::SliderAngle("Yaw", &scene.m_SunYaw, -180.0f, 180.0f);
                changed |= ImGui::SliderAngle("Pitch", &scene.m_SunPitch, 0.0f, 90.0f);
                ImGui::Text("Direction: %.2f, %.2f, %.2f", scene.m_SunDirection.x, scene.m_SunDirection.y, scene.m_SunDirection.z);

                if (changed)
                {
                    scene.m_LightsDirty = true;

                    // Final sun direction for atmosphere (points TOWARDS the sun)
                    const float pitchRad = scene.m_SunPitch;
                    const float yawRad = scene.m_SunYaw;
                    scene.m_SunDirection.x = cosf(pitchRad) * sinf(yawRad);
                    scene.m_SunDirection.y = sinf(pitchRad);
                    scene.m_SunDirection.z = cosf(pitchRad) * cosf(yawRad);
                }

                ImGui::TreePop();
            }

            for (size_t i = 0; i < scene.m_Lights.size(); ++i)
            {
                Scene::Light& light = scene.m_Lights[i];
                std::string label = light.m_Name.empty() ? ("Light " + std::to_string(i)) : light.m_Name;
                if (ImGui::TreeNode(label.c_str()))
                {
                    const char* types[] = { "Directional", "Point", "Spot" };
                    ImGui::Text("Type: %s", types[light.m_Type]);
                    scene.m_LightsDirty |= ImGui::ColorEdit3("Color", (float*)&light.m_Color);
                    scene.m_LightsDirty |= ImGui::DragFloat("Intensity", &light.m_Intensity, 0.1f, 0.1f, 10.0f);
                    scene.m_LightsDirty |= ImGui::DragFloat("Range", &light.m_Range, 0.1f, 0.0f, 1000.0f);
                    if (light.m_Type == Scene::Light::Spot)
                    {
                        scene.m_LightsDirty |= ImGui::DragFloat("Inner Angle", &light.m_SpotInnerConeAngle, 0.01f, 0.0f, light.m_SpotOuterConeAngle);
                        scene.m_LightsDirty |= ImGui::DragFloat("Outer Angle", &light.m_SpotOuterConeAngle, 0.01f, light.m_SpotInnerConeAngle, DirectX::XM_PI);
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }

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
            if (!scene.m_Cameras.empty())
            {
                std::vector<const char*> cameraNames;
                for (const Scene::Camera& cam : scene.m_Cameras)
                {
                    cameraNames.push_back(cam.m_Name.empty() ? "Unnamed Camera" : cam.m_Name.c_str());
                }
                if (ImGui::Combo("GLTF Camera", &renderer->m_SelectedCameraIndex, cameraNames.data(), static_cast<int>(cameraNames.size())))
                {
                    if (renderer->m_SelectedCameraIndex >= 0 && renderer->m_SelectedCameraIndex < static_cast<int>(scene.m_Cameras.size()))
                    {
                        const Scene::Camera& selectedCam = scene.m_Cameras[renderer->m_SelectedCameraIndex];
                        renderer->SetCameraFromSceneCamera(selectedCam);
                    }
                }
            }

            if (ImGui::Button("Reset Camera"))
            {
                if (renderer->m_SelectedCameraIndex >= 0 && renderer->m_SelectedCameraIndex < static_cast<int>(scene.m_Cameras.size()))
                {
                    const Scene::Camera& selectedCam = scene.m_Cameras[renderer->m_SelectedCameraIndex];
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

        // Culling controls
        if (ImGui::TreeNode("Culling"))
        {
            ImGui::Checkbox("Enable Frustum Culling", &renderer->m_EnableFrustumCulling);
            ImGui::Checkbox("Enable Cone Culling", &renderer->m_EnableConeCulling);
            ImGui::Checkbox("Enable Occlusion Culling", &renderer->m_EnableOcclusionCulling);

            bool prevFreeze = renderer->m_FreezeCullingCamera;
            ImGui::Checkbox("Freeze Culling Camera", &renderer->m_FreezeCullingCamera);
            if (!prevFreeze && renderer->m_FreezeCullingCamera)
            {
                renderer->m_FrozenCullingViewMatrix = renderer->m_Camera.GetViewMatrix();
                renderer->m_FrozenCullingCameraPos = renderer->m_Camera.GetPosition();
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

            ImGui::Text("Total Instances: %zu", scene.m_InstanceData.size());
            ImGui::Text("Opaque:      %u", scene.m_OpaqueBucket.m_Count);
            ImGui::Text("Masked:      %u", scene.m_MaskedBucket.m_Count);
            ImGui::Text("Transparent: %u", scene.m_TransparentBucket.m_Count);
            ImGui::NewLine();

            if (ImGui::BeginTable("TimingsTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Pass");
                ImGui::TableSetupColumn("CPU (ms)");
                ImGui::TableSetupColumn("GPU (ms)");
                ImGui::TableHeadersRow();
                for (const std::shared_ptr<IRenderer>& r : renderer->m_Renderers)
                {
                    if (!r->m_bPassEnabled)
                    {
                        continue;
                    }
                    
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
        if (ImGui::TreeNode("Base Pass Pipeline Statistics"))
        {
            const char* currentRendererName = "None";
            if (renderer->m_SelectedRendererIndexForPipelineStatistics != -1)
            {
                currentRendererName = renderer->m_Renderers[renderer->m_SelectedRendererIndexForPipelineStatistics]->GetName();
            }

            if (ImGui::BeginCombo("Select Base Pass Renderer", currentRendererName))
            {
                const bool bIsNoneSelected = (renderer->m_SelectedRendererIndexForPipelineStatistics == -1);
                if (ImGui::Selectable("None", bIsNoneSelected))
                {
                    renderer->m_SelectedRendererIndexForPipelineStatistics = -1;
                }
                if (bIsNoneSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }

                for (int i = 0; i < (int)renderer->m_Renderers.size(); ++i)
                {
                    const char* name = renderer->m_Renderers[i]->GetName();
                    const bool bIsSelected = (renderer->m_SelectedRendererIndexForPipelineStatistics == i);
                    if (renderer->m_Renderers[i]->IsBasePassRenderer())
                    {
                        if (ImGui::Selectable(name, bIsSelected))
                        {
                            renderer->m_SelectedRendererIndexForPipelineStatistics = i;
                        }
                    }
                    if (bIsSelected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();

                if (bIsNoneSelected)
                {
                    memset(&renderer->m_SelectedBasePassPipelineStatistics, 0, sizeof(renderer->m_SelectedBasePassPipelineStatistics));
                }
            }

            const nvrhi::PipelineStatistics& stats = renderer->m_SelectedBasePassPipelineStatistics;
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

        // Render Graph debug UI
        renderer->m_RenderGraph.RenderDebugUI();
    }
    ImGui::End();

    ImGui::Render();
}


