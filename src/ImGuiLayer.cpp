
#include "Renderer.h"
#include "CommonResources.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>

void ImGuiLayer::Initialize()
{
    SDL_Window* window = g_Renderer.m_Window;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForD3D(window))
    {
        SDL_LOG_ASSERT_FAIL("ImGui_ImplSDL3_InitForD3D failed", "[Init] Failed to initialize ImGui SDL3 backend");
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
    
    Scene& scene = g_Renderer.m_Scene;
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    if (ImGui::BeginMainMenuBar())
    {
        ImGui::Text("FPS: %.1f", g_Renderer.m_FPS);
        ImGui::Separator();
        ImGui::Text("CPU Time: %.3f ms", g_Renderer.m_FrameTime);
        ImGui::Separator();
        ImGui::Text("VRAM: %.1f MB", g_Renderer.m_RHI->GetVRAMUsageMB());
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
        ImGui::DragInt("Target FPS", (int*)&g_Renderer.m_TargetFPS, 1.0f, 10, 200);

        // Rendering options
        if (ImGui::TreeNode("Rendering"))
        {
            static const char* kRenderingModes[] = { "Normal", "Image Based Lighting", "Reference Pathtracer" };
            int currentMode = static_cast<int>(g_Renderer.m_Mode);
            if (ImGui::Combo("Rendering Mode", &currentMode, kRenderingModes, IM_ARRAYSIZE(kRenderingModes)))
            {
                g_Renderer.m_Mode = static_cast<RenderingMode>(currentMode);
            }

            ImGui::Checkbox("Use Meshlet Rendering", &g_Renderer.m_UseMeshletRendering);
            ImGui::Checkbox("Enable RT Shadows", &g_Renderer.m_EnableRTShadows);
            ImGui::Checkbox("Enable Sky", &g_Renderer.m_EnableSky);

            static const char* kDebugModes[] = {
                "None", "Instances", "Meshlets", "World Normals", "Albedo", "Roughness", "Metallic", "Emissive", "LOD", "Motion Vectors", "ReGIR Cells"
            };
            ImGui::Combo("Debug Mode", &g_Renderer.m_DebugMode, kDebugModes, IM_ARRAYSIZE(kDebugModes));

            const char* lodNames[] = { "Auto", "LOD 0", "LOD 1", "LOD 2", "LOD 3", "LOD 4", "LOD 5", "LOD 6", "LOD 7" };
            int forcedLODIdx = g_Renderer.m_ForcedLOD + 1;
            if (ImGui::SliderInt("Forced LOD", &forcedLODIdx, 0, srrhi::CommonConsts::MAX_LOD_COUNT))
            {
                g_Renderer.m_ForcedLOD = forcedLODIdx - 1;
            }
            ImGui::SameLine();
            ImGui::Text("%s", lodNames[forcedLODIdx]);

            ImGui::Checkbox("ReSTIR DI", &g_Renderer.m_EnableReSTIRDI);
            if (g_Renderer.m_EnableReSTIRDI)
            {
                extern void RTXDIIMGUISettings();
                RTXDIIMGUISettings();
            }

            ImGui::Checkbox("Enable Animations", &g_Renderer.m_EnableAnimations);

            ImGui::TreePop();
        }

        // Path Tracer settings
        if (ImGui::TreeNode("Path Tracer"))
        {
            int maxBounces = (int)g_Renderer.m_PathTracerMaxBounces;
            if (ImGui::SliderInt("Max Bounces", &maxBounces, 1, 12))
            {
                g_Renderer.m_PathTracerMaxBounces = (uint32_t)maxBounces;
            }
            ImGui::TreePop();
        }

        // Post-processing settings
        if (ImGui::TreeNode("Post-Processing"))
        {
            ImGui::Checkbox("Enable Bloom", &g_Renderer.m_EnableBloom);
            ImGui::Checkbox("Debug Bloom Only", &g_Renderer.m_DebugBloom);
            ImGui::DragFloat("Bloom Intensity", &g_Renderer.m_BloomIntensity, 0.001f, 0.0f, 1.0f);
            ImGui::DragFloat("Bloom Knee", &g_Renderer.m_BloomKnee, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Bloom Upsample Radius", &g_Renderer.m_UpsampleRadius, 0.01f, 0.1f, 2.0f);

            ImGui::Separator();

            ImGui::Checkbox("Auto Exposure", &g_Renderer.m_EnableAutoExposure);
            if (g_Renderer.m_EnableAutoExposure)
            {
                ImGui::DragFloat("EV Min", &scene.m_Camera.m_ExposureValueMin, 0.1f, -20.0f, 20.0f);
                ImGui::DragFloat("EV Max", &scene.m_Camera.m_ExposureValueMax, 0.1f, -20.0f, 20.0f);
                ImGui::DragFloat("Adaptation Speed", &g_Renderer.m_AdaptationSpeed, 0.1f, 0.0f, 20.0f);
            }
            else
            {
                ImGui::DragFloat("Exposure Value (EV100)", &scene.m_Camera.m_ExposureValue, 0.1f, -20.0f, 20.0f);
            }
            ImGui::DragFloat("Exposure Compensation", &scene.m_Camera.m_ExposureCompensation, 0.1f, -10.0f, 10.0f);
            ImGui::Text("Current Multiplier: %.4f", scene.m_Camera.m_Exposure);

            ImGui::TreePop();
        }

        // TAA (FSR3) controls
        if (ImGui::TreeNode("TAA"))
        {
            ImGui::Checkbox("Enable TAA", &g_Renderer.m_bTAAEnabled);

            if (g_Renderer.m_bTAAEnabled)
            {
                ImGui::Checkbox("Debug View", &g_Renderer.m_bTAADebugView);
                ImGui::SliderFloat("Sharpness", &g_Renderer.m_TAASharpness, 0.0f, 1.0f);
            }

            ImGui::TreePop();
        }

        // Lights controls
        if (ImGui::TreeNode("Lights"))
        {
            // the last light is guaranteed to be the sun light
            Scene::Light& sun = scene.m_Lights.back();
            if (ImGui::TreeNode("Sun Orientation"))
            {
                bool changed = false;
                float sunYaw = scene.GetSunYaw();
                float sunPitch = scene.GetSunPitch();
                changed |= ImGui::SliderAngle("Yaw", &sunYaw, -180.0f, 180.0f);
                changed |= ImGui::SliderAngle("Pitch", &sunPitch, 0.0f, 90.0f);
                Vector3 sunDir = scene.GetSunDirection();
                ImGui::Text("Direction: %.2f, %.2f, %.2f", sunDir.x, sunDir.y, sunDir.z);

                if (changed)
                {
                    scene.m_LightsDirty = true;
                    scene.SetSunPitchYaw(sunPitch, sunYaw);
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
                    if (light.m_NodeIndex >= 0)
                    {
                        bool highlighted = (g_Renderer.m_SelectedNodeIndex == light.m_NodeIndex);
                        if (ImGui::Checkbox("Highlight", &highlighted))
                            g_Renderer.m_SelectedNodeIndex = highlighted ? light.m_NodeIndex : -1;
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
        // Camera controls
        if (ImGui::TreeNode("Camera"))
        {
            ImGui::DragFloat("Move Speed", &scene.m_Camera.m_MoveSpeed, 0.1f, 0.0f, 100.0f);
            ImGui::DragFloat("Mouse Sensitivity", &scene.m_Camera.m_MouseSensitivity, 0.0005f, 0.0f, 1.0f, "%.4f");

            // Display camera position
            Vector3 pos = scene.m_Camera.GetPosition();
            ImGui::Text("Position: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
            
            // Display forward vector (from view matrix, third column)
            Matrix view = scene.m_Camera.GetViewMatrix();
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
                if (ImGui::Combo("GLTF Camera", &scene.m_SelectedCameraIndex, cameraNames.data(), static_cast<int>(cameraNames.size())))
                {
                    if (scene.m_SelectedCameraIndex >= 0 && scene.m_SelectedCameraIndex < static_cast<int>(scene.m_Cameras.size()))
                    {
                        const Scene::Camera& selectedCam = scene.m_Cameras[scene.m_SelectedCameraIndex];
                        g_Renderer.SetCameraFromSceneCamera(selectedCam);
                    }
                }
            }

            if (ImGui::Button("Reset Camera"))
            {
                if (scene.m_SelectedCameraIndex >= 0 && scene.m_SelectedCameraIndex < static_cast<int>(scene.m_Cameras.size()))
                {
                    const Scene::Camera& selectedCam = scene.m_Cameras[scene.m_SelectedCameraIndex];
                    g_Renderer.SetCameraFromSceneCamera(selectedCam);
                }
                else
                {
                    // Reset to 0,0,0 position and default orientation
                    scene.m_Camera.Reset();
                }
            }

            ImGui::TreePop();
        }

        // Selected Node Highlight
        if (ImGui::TreeNode("Node Highlight"))
        {
            const char* currentLabel = g_Renderer.m_SelectedNodeIndex == -1 ? "None" : scene.m_Nodes[g_Renderer.m_SelectedNodeIndex].m_Name.c_str();

            if (ImGui::BeginCombo("Highlighted Node", currentLabel))
            {
                if (ImGui::Selectable("None", g_Renderer.m_SelectedNodeIndex == -1))
                    g_Renderer.m_SelectedNodeIndex = -1;

                for (int i = 0; i < (int)scene.m_Nodes.size(); ++i)
                {
                    const Scene::Node& node = scene.m_Nodes[i];

                    bool isSelected = (g_Renderer.m_SelectedNodeIndex == i);
                    const std::string& nodeName = node.m_Name;
                    std::string label = std::to_string(i) + ": " + (nodeName.empty() ? "Unnamed Node" : nodeName);

                    if (ImGui::Selectable(label.c_str(), isSelected))
                        g_Renderer.m_SelectedNodeIndex = i;

                    if (isSelected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::TreePop();
        }

        // Culling controls
        if (ImGui::TreeNode("Culling"))
        {
            ImGui::Checkbox("Enable Frustum Culling", &g_Renderer.m_EnableFrustumCulling);
            ImGui::Checkbox("Enable Cone Culling", &g_Renderer.m_EnableConeCulling);
            ImGui::Checkbox("Enable Occlusion Culling", &g_Renderer.m_EnableOcclusionCulling);

            bool prevFreeze = g_Renderer.m_FreezeCullingCamera;
            ImGui::Checkbox("Freeze Culling Camera", &g_Renderer.m_FreezeCullingCamera);
            if (!prevFreeze && g_Renderer.m_FreezeCullingCamera)
            {
                scene.m_FrozenCullingViewMatrix = scene.m_Camera.GetViewMatrix();
                scene.m_FrozenCullingCameraPos = scene.m_Camera.GetPosition();
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
                for (const std::shared_ptr<IRenderer>& r : g_Renderer.m_Renderers)
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
            if (g_Renderer.m_SelectedRendererIndexForPipelineStatistics != -1)
            {
                currentRendererName = g_Renderer.m_Renderers[g_Renderer.m_SelectedRendererIndexForPipelineStatistics]->GetName();
            }

            if (ImGui::BeginCombo("Select Base Pass Renderer", currentRendererName))
            {
                const bool bIsNoneSelected = (g_Renderer.m_SelectedRendererIndexForPipelineStatistics == -1);
                if (ImGui::Selectable("None", bIsNoneSelected))
                {
                    g_Renderer.m_SelectedRendererIndexForPipelineStatistics = -1;
                }
                if (bIsNoneSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }

                for (int i = 0; i < (int)g_Renderer.m_Renderers.size(); ++i)
                {
                    const char* name = g_Renderer.m_Renderers[i]->GetName();
                    const bool bIsSelected = (g_Renderer.m_SelectedRendererIndexForPipelineStatistics == i);
                    if (g_Renderer.m_Renderers[i]->IsBasePassRenderer())
                    {
                        if (ImGui::Selectable(name, bIsSelected))
                        {
                            g_Renderer.m_SelectedRendererIndexForPipelineStatistics = i;
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
                    memset(&g_Renderer.m_SelectedBasePassPipelineStatistics, 0, sizeof(g_Renderer.m_SelectedBasePassPipelineStatistics));
                }
            }

            const nvrhi::PipelineStatistics& stats = g_Renderer.m_SelectedBasePassPipelineStatistics;
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
        g_Renderer.m_RenderGraph.RenderDebugUI();
    }
    ImGui::End();

    if (g_Renderer.m_SelectedNodeIndex >= 0 && g_Renderer.m_SelectedNodeIndex < (int)scene.m_Nodes.size())
    {
        const Scene::Node& node = scene.m_Nodes[g_Renderer.m_SelectedNodeIndex];

        DirectX::XMMATRIX view = DirectX::XMLoadFloat4x4(&scene.m_View.m_MatWorldToView);
        DirectX::XMMATRIX invView = DirectX::XMLoadFloat4x4(&scene.m_View.m_MatViewToWorld);
        DirectX::XMMATRIX proj = DirectX::XMLoadFloat4x4(&scene.m_View.m_MatViewToClipNoOffset);
        
        ImVec2 viewportSize = ImGui::GetIO().DisplaySize;
        float viewportWidth = viewportSize.x;
        float viewportHeight = viewportSize.y;

        DirectX::XMVECTOR vCenter = DirectX::XMLoadFloat3(&node.m_Center);
        float radius = node.m_Radius;

        // Fallback for nodes that might not have bounding sphere pre-calculated (if any, like pure light nodes)
        if (radius <= 1e-4f)
        {
            vCenter = DirectX::XMVectorSet(node.m_WorldTransform._41, node.m_WorldTransform._42, node.m_WorldTransform._43, 1.0f);
            radius = 1.0f;
            if (node.m_LightIndex >= 0)
            {
                // Point/Spot light radius from range
                radius = scene.m_Lights[node.m_LightIndex].m_Radius == 0.0f ? 0.5f : scene.m_Lights[node.m_LightIndex].m_Radius;
                if (scene.m_Lights[node.m_LightIndex].m_Type == Scene::Light::Directional) radius = 100.0f;
            }
        }

        DirectX::XMVECTOR vScreenCenter = DirectX::XMVector3Project(vCenter, 0.0f, 0.0f, viewportWidth, viewportHeight, 0.0f, 1.0f, proj, view, DirectX::XMMatrixIdentity());
        Vector3 screenCenter;
        DirectX::XMStoreFloat3(&screenCenter, vScreenCenter);

        bool isOffScreen = (screenCenter.z < 0.0f || screenCenter.z > 1.0f ||
                            screenCenter.x < 10.0f || screenCenter.x > viewportWidth - 10.0f ||
                            screenCenter.y < 10.0f || screenCenter.y > viewportHeight - 10.0f);

        // Check if point is in front of camera (Z in [0, 1] for typical DX range)
        if (!isOffScreen)
        {
            float cx = screenCenter.x;
            float cy = screenCenter.y;

            // Project a point at correct distance horizontally from camera's perspective
            // using camera's world-space Right vector ensures stability during rotation.
            DirectX::XMVECTOR vRight = invView.r[0]; 
            DirectX::XMVECTOR vPoint = DirectX::XMVectorAdd(vCenter, DirectX::XMVectorScale(vRight, radius));
            DirectX::XMVECTOR vScreenPoint = DirectX::XMVector3Project(vPoint, 0.0f, 0.0f, viewportWidth, viewportHeight, 0.0f, 1.0f, proj, view, DirectX::XMMatrixIdentity());
            Vector3 screenPoint;
            DirectX::XMStoreFloat3(&screenPoint, vScreenPoint);

            float radiusPx = sqrtf((screenPoint.x - cx) * (screenPoint.x - cx) + (screenPoint.y - cy) * (screenPoint.y - cy));
            radiusPx = std::max(radiusPx, 2.0f); // Minimum visual size

            ImDrawList* drawList = ImGui::GetForegroundDrawList();
            drawList->AddCircle(ImVec2(cx, cy), radiusPx, IM_COL32(255, 255, 0, 255), 64, 2.5f);

            std::string text = (node.m_Name.empty() ? "Node" : node.m_Name) + " [" + std::to_string(g_Renderer.m_SelectedNodeIndex) + "]";
            drawList->AddText(ImVec2(cx, cy - radiusPx - 20), IM_COL32(255, 255, 0, 255), text.c_str());

            // --- Spotlight cone visualization ---
            if (node.m_LightIndex >= 0)
            {
                const Scene::Light& light = scene.m_Lights[node.m_LightIndex];
                if (light.m_Type == Scene::Light::Spot)
                {
                    // In glTF, spotlights point along the node's local -Z axis.
                    // m_WorldTransform row 2 (0-indexed) is the Z-axis; negate for forward.
                    const Matrix& wt = node.m_WorldTransform;
                    DirectX::XMVECTOR vForward = DirectX::XMVectorSet(-wt._31, -wt._32, -wt._33, 0.0f);
                    vForward = DirectX::XMVector3Normalize(vForward);

                    // Build two orthogonal vectors perpendicular to the forward direction
                    DirectX::XMVECTOR vUp = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
                    if (fabsf(DirectX::XMVectorGetY(vForward)) > 0.99f)
                        vUp = DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
                    DirectX::XMVECTOR vSide = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(vForward, vUp));
                    vUp = DirectX::XMVector3Cross(vSide, vForward);

                    // Cone length: use range if set, otherwise a reasonable world-space fallback
                    float coneLength = (light.m_Range > 1e-4f) ? light.m_Range : 3.0f;

                    // Light origin in world space (from node transform translation)
                    DirectX::XMVECTOR vOrigin = DirectX::XMVectorSet(wt._41, wt._42, wt._43, 1.0f);

                    // Helper: project a world point to screen, returns false if behind camera
                    auto ProjectToScreen = [&](DirectX::XMVECTOR vWorld, ImVec2& outScreen) -> bool
                    {
                        DirectX::XMVECTOR vS = DirectX::XMVector3Project(vWorld, 0.0f, 0.0f, viewportWidth, viewportHeight, 0.0f, 1.0f, proj, view, DirectX::XMMatrixIdentity());
                        Vector3 s;
                        DirectX::XMStoreFloat3(&s, vS);
                        outScreen = ImVec2(s.x, s.y);
                        return (s.z >= 0.0f && s.z <= 1.0f);
                    };

                    // Draw a cone ring: project N points around the rim circle at the given half-angle
                    auto DrawConeRing = [&](float halfAngle, ImU32 color, float thickness, int segments = 24)
                    {
                        float rimRadius = coneLength * tanf(halfAngle);
                        DirectX::XMVECTOR vTip = DirectX::XMVectorAdd(vOrigin, DirectX::XMVectorScale(vForward, coneLength));

                        ImVec2 prevScreen;
                        bool prevValid = false;
                        ImVec2 firstScreen;
                        bool firstValid = false;

                        for (int s = 0; s <= segments; ++s)
                        {
                            float theta = (float)s / (float)segments * DirectX::XM_2PI;
                            DirectX::XMVECTOR vRimPoint = DirectX::XMVectorAdd(
                                vTip,
                                DirectX::XMVectorAdd(
                                    DirectX::XMVectorScale(vSide, rimRadius * cosf(theta)),
                                    DirectX::XMVectorScale(vUp,   rimRadius * sinf(theta))
                                )
                            );

                            ImVec2 screenRim;
                            bool valid = ProjectToScreen(vRimPoint, screenRim);

                            if (s == 0) { firstScreen = screenRim; firstValid = valid; }

                            if (prevValid && valid)
                                drawList->AddLine(prevScreen, screenRim, color, thickness);

                            prevScreen = screenRim;
                            prevValid = valid;
                        }

                        // Draw 4 lines from origin to rim (at cardinal directions around the cone)
                        for (int s = 0; s < 4; ++s)
                        {
                            float theta = (float)s / 4.0f * DirectX::XM_2PI;
                            DirectX::XMVECTOR vRimPoint = DirectX::XMVectorAdd(
                                vTip,
                                DirectX::XMVectorAdd(
                                    DirectX::XMVectorScale(vSide, rimRadius * cosf(theta)),
                                    DirectX::XMVectorScale(vUp,   rimRadius * sinf(theta))
                                )
                            );
                            ImVec2 screenRim;
                            bool valid = ProjectToScreen(vRimPoint, screenRim);
                            ImVec2 screenOrigin;
                            bool originValid = ProjectToScreen(vOrigin, screenOrigin);
                            if (valid && originValid)
                                drawList->AddLine(screenOrigin, screenRim, color, thickness);
                        }
                    };

                    // Draw direction arrow from origin along forward axis
                    {
                        float arrowLen = coneLength * 0.25f;
                        DirectX::XMVECTOR vArrowTip = DirectX::XMVectorAdd(vOrigin, DirectX::XMVectorScale(vForward, arrowLen));
                        ImVec2 screenOrigin, screenArrowTip;
                        bool o = ProjectToScreen(vOrigin, screenOrigin);
                        bool t = ProjectToScreen(vArrowTip, screenArrowTip);
                        if (o && t)
                        {
                            drawList->AddLine(screenOrigin, screenArrowTip, IM_COL32(255, 200, 50, 255), 2.5f);
                            // Small arrowhead triangle
                            float adx = screenArrowTip.x - screenOrigin.x;
                            float ady = screenArrowTip.y - screenOrigin.y;
                            float alen = sqrtf(adx * adx + ady * ady);
                            if (alen > 1e-4f) { adx /= alen; ady /= alen; }
                            float headSize = 8.0f;
                            float perpX = -ady, perpY = adx;
                            ImVec2 ah1 = ImVec2(screenArrowTip.x + headSize * adx,  screenArrowTip.y + headSize * ady);
                            ImVec2 ah2 = ImVec2(screenArrowTip.x + headSize * 0.5f * perpX - headSize * 0.5f * adx,
                                                screenArrowTip.y + headSize * 0.5f * perpY - headSize * 0.5f * ady);
                            ImVec2 ah3 = ImVec2(screenArrowTip.x - headSize * 0.5f * perpX - headSize * 0.5f * adx,
                                                screenArrowTip.y - headSize * 0.5f * perpY - headSize * 0.5f * ady);
                            drawList->AddTriangleFilled(ah1, ah2, ah3, IM_COL32(255, 200, 50, 255));
                        }
                    }

                    // Outer cone (solid yellow)
                    DrawConeRing(light.m_SpotOuterConeAngle, IM_COL32(255, 255, 0, 200), 1.5f);

                    // Inner cone (brighter, thinner)
                    if (light.m_SpotInnerConeAngle > 1e-4f)
                        DrawConeRing(light.m_SpotInnerConeAngle, IM_COL32(255, 255, 180, 160), 1.0f);
                }
            }
        }
        else
        {
            // Calculate direction from camera to node
            DirectX::XMVECTOR vCamPos = invView.r[3];
            DirectX::XMVECTOR vDirWorld = DirectX::XMVector3Normalize(DirectX::XMVectorSubtract(vCenter, vCamPos));
            
            // Transform world direction to camera space
            DirectX::XMVECTOR vDirView = DirectX::XMVector3TransformNormal(vDirWorld, view);
            
            // Screen space direction (X = right, Y = down)
            // Note: dx is negated because the project's projection matrix mirrors the X-axis
            float dx = -DirectX::XMVectorGetX(vDirView); 
            float dy = -DirectX::XMVectorGetY(vDirView); // Flip Y for screen
            
            // If the object is behind, the projection is inverted.
            // We want the arrow to point towards where the object is in world space.
            if (DirectX::XMVectorGetZ(vDirView) < 0.0f)
            {
                dx = -dx;
                dy = -dy;
            }

            float len = sqrtf(dx * dx + dy * dy);
            if (len > 1e-4f)
            {
                dx /= len;
                dy /= len;
            }
            else
            {
                // Edge case: directly behind or aligned with look vector
                dx = 0.0f; dy = -1.0f;
            }

            // Find position on the edge of the screen
            float margin = 40.0f;
            float centerX = viewportWidth * 0.5f;
            float centerY = viewportHeight * 0.5f;
            
            // Ray-cast to viewport edges
            float scaleX = (dx > 0.0f) ? (viewportWidth * 0.5f - margin) / dx : (dx < 0.0f) ? (-viewportWidth * 0.5f + margin) / dx : 1e10f;
            float scaleY = (dy > 0.0f) ? (viewportHeight * 0.5f - margin) / dy : (dy < 0.0f) ? (-viewportHeight * 0.5f + margin) / dy : 1e10f;
            float scale = std::min(scaleX, scaleY);
            
            float ax = centerX + dx * scale;
            float ay = centerY + dy * scale;
            
            ImDrawList* drawList = ImGui::GetForegroundDrawList();
            ImU32 color = IM_COL32(255, 255, 0, 255);
            float arrowSize = 25.0f;
            float angle = atan2f(dy, dx);
            
            // Arrow Head (Triangle)
            ImVec2 p1 = ImVec2(ax + arrowSize * cosf(angle), ay + arrowSize * sinf(angle));
            ImVec2 p2 = ImVec2(ax + arrowSize * 0.6f * cosf(angle + 2.4f), ay + arrowSize * 0.6f * sinf(angle + 2.4f));
            ImVec2 p3 = ImVec2(ax + arrowSize * 0.6f * cosf(angle - 2.4f), ay + arrowSize * 0.6f * sinf(angle - 2.4f));
            drawList->AddTriangleFilled(p1, p2, p3, color);
            
            // Arrow Shaft (Quad)
            ImVec2 midBase = ImVec2((p2.x + p3.x) * 0.5f, (p2.y + p3.y) * 0.5f);
            ImVec2 shaftEnd = ImVec2(midBase.x - arrowSize * 0.8f * cosf(angle), midBase.y - arrowSize * 0.8f * sinf(angle));
            float sw = arrowSize * 0.2f;
            float sx = -sinf(angle) * sw;
            float sy = cosf(angle) * sw;
            
            drawList->AddQuadFilled(
                ImVec2(midBase.x + sx, midBase.y + sy),
                ImVec2(midBase.x - sx, midBase.y - sy),
                ImVec2(shaftEnd.x - sx, shaftEnd.y - sy),
                ImVec2(shaftEnd.x + sx, shaftEnd.y + sy),
                color);

            std::string text = "LOOK [" + std::to_string(g_Renderer.m_SelectedNodeIndex) + "]";
            drawList->AddText(ImVec2(ax - 20, ay + 20), color, text.c_str());
        }
    }

    ImGui::Render();
}


