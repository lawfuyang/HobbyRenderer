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
                "None", "Instances", "Meshlets", "World Normals", "Albedo", "Roughness", "Metallic", "Emissive", "LOD", "Motion Vectors", "Sky Visibility"
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

            ImGui::Checkbox("ReSTIR DI", &renderer->m_EnableReSTIRDI);
            if (renderer->m_EnableReSTIRDI)
            {
                ImGui::Indent();
                ImGui::Checkbox("Temporal Resampling", &renderer->m_ReSTIRDI_EnableTemporal);
                ImGui::Checkbox("Spatial Resampling", &renderer->m_ReSTIRDI_EnableSpatial);
                if (renderer->m_ReSTIRDI_EnableSpatial)
                {
                    ImGui::SliderInt("Spatial Samples", &renderer->m_ReSTIRDI_SpatialSamples, 1, 8);
                }
                ImGui::Checkbox("Checkerboard Sampling", &renderer->m_ReSTIRDI_EnableCheckerboard);
                ImGui::Unindent();
            }

            ImGui::Checkbox("Enable Animations", &renderer->m_EnableAnimations);

            ImGui::TreePop();
        }

        // Path Tracer settings
        if (ImGui::TreeNode("Path Tracer"))
        {
            int maxBounces = (int)renderer->m_PathTracerMaxBounces;
            if (ImGui::SliderInt("Max Bounces", &maxBounces, 1, 12))
            {
                renderer->m_PathTracerMaxBounces = (uint32_t)maxBounces;
            }
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
                ImGui::DragFloat("EV Min", &scene.m_Camera.m_ExposureValueMin, 0.1f, -20.0f, 20.0f);
                ImGui::DragFloat("EV Max", &scene.m_Camera.m_ExposureValueMax, 0.1f, -20.0f, 20.0f);
                ImGui::DragFloat("Adaptation Speed", &renderer->m_AdaptationSpeed, 0.1f, 0.0f, 20.0f);
            }
            else
            {
                ImGui::DragFloat("Exposure Value (EV100)", &scene.m_Camera.m_ExposureValue, 0.1f, -20.0f, 20.0f);
            }
            ImGui::DragFloat("Exposure Compensation", &scene.m_Camera.m_ExposureCompensation, 0.1f, -10.0f, 10.0f);
            ImGui::Text("Current Multiplier: %.4f", scene.m_Camera.m_Exposure);

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
                        renderer->SetCameraFromSceneCamera(selectedCam);
                    }
                }
            }

            if (ImGui::Button("Reset Camera"))
            {
                if (scene.m_SelectedCameraIndex >= 0 && scene.m_SelectedCameraIndex < static_cast<int>(scene.m_Cameras.size()))
                {
                    const Scene::Camera& selectedCam = scene.m_Cameras[scene.m_SelectedCameraIndex];
                    renderer->SetCameraFromSceneCamera(selectedCam);
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
            const char* currentLabel = renderer->m_SelectedNodeIndex == -1 ? "None" : scene.m_Nodes[renderer->m_SelectedNodeIndex].m_Name.c_str();

            if (ImGui::BeginCombo("Highlighted Node", currentLabel))
            {
                if (ImGui::Selectable("None", renderer->m_SelectedNodeIndex == -1))
                    renderer->m_SelectedNodeIndex = -1;

                for (int i = 0; i < (int)scene.m_Nodes.size(); ++i)
                {
                    const Scene::Node& node = scene.m_Nodes[i];

                    bool isSelected = (renderer->m_SelectedNodeIndex == i);
                    const std::string& nodeName = node.m_Name;
                    std::string label = std::to_string(i) + ": " + (nodeName.empty() ? "Unnamed Node" : nodeName);

                    if (ImGui::Selectable(label.c_str(), isSelected))
                        renderer->m_SelectedNodeIndex = i;

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
            ImGui::Checkbox("Enable Frustum Culling", &renderer->m_EnableFrustumCulling);
            ImGui::Checkbox("Enable Cone Culling", &renderer->m_EnableConeCulling);
            ImGui::Checkbox("Enable Occlusion Culling", &renderer->m_EnableOcclusionCulling);

            bool prevFreeze = renderer->m_FreezeCullingCamera;
            ImGui::Checkbox("Freeze Culling Camera", &renderer->m_FreezeCullingCamera);
            if (!prevFreeze && renderer->m_FreezeCullingCamera)
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

    if (renderer->m_SelectedNodeIndex >= 0 && renderer->m_SelectedNodeIndex < (int)scene.m_Nodes.size())
    {
        const Scene::Node& node = scene.m_Nodes[renderer->m_SelectedNodeIndex];

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
        DirectX::XMFLOAT3 screenCenter;
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
            DirectX::XMFLOAT3 screenPoint;
            DirectX::XMStoreFloat3(&screenPoint, vScreenPoint);

            float radiusPx = sqrtf((screenPoint.x - cx) * (screenPoint.x - cx) + (screenPoint.y - cy) * (screenPoint.y - cy));
            radiusPx = std::max(radiusPx, 2.0f); // Minimum visual size

            ImDrawList* drawList = ImGui::GetForegroundDrawList();
            drawList->AddCircle(ImVec2(cx, cy), radiusPx, IM_COL32(255, 255, 0, 255), 64, 2.5f);

            std::string text = (node.m_Name.empty() ? "Node" : node.m_Name) + " [" + std::to_string(renderer->m_SelectedNodeIndex) + "]";
            drawList->AddText(ImVec2(cx, cy - radiusPx - 20), IM_COL32(255, 255, 0, 255), text.c_str());
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

            std::string text = "LOOK [" + std::to_string(renderer->m_SelectedNodeIndex) + "]";
            drawList->AddText(ImVec2(ax - 20, ay + 20), color, text.c_str());
        }
    }

    ImGui::Render();
}


