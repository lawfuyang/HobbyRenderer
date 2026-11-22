#include "Renderer.h"
#include "CommonResources.h"
#include "Config.h"
#include "PixCapture.h"
#include "RenderDocCapture.h"
#include "Streaming/FeedbackManager.h"

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

    // Invariant: the SDL3 backend must have populated DisplaySize from the window.
    // A zero DisplaySize means the window is not yet visible or the backend was not
    // properly initialized, which will cause divide-by-zero in ImGuiRenderer::Render.
    SDL_assert(ImGui::GetIO().DisplaySize.x > 0.0f &&
        "ImGuiIO::DisplaySize.x is 0 after ImGui_ImplSDL3_NewFrame — window may not be visible");
    SDL_assert(ImGui::GetIO().DisplaySize.y > 0.0f &&
        "ImGuiIO::DisplaySize.y is 0 after ImGui_ImplSDL3_NewFrame — window may not be visible");

    // A screenshot reads the backbuffer, so whatever the previous frame's UI drew is in the image. Comparison
    // runs skip the UI entirely; the frame still has to be submitted and rendered for the buffer to be complete.
    if (Config::Get().m_bHideUI)
    {
        ImGui::Render();
        return;
    }

    if (ImGui::BeginMainMenuBar())
    {
        // The readouts change every frame, so each value is padded to a fixed field: the default font is fixed
        // pitch, which keeps the numbers from shifting and the separators from sliding as digits come and go.
        ImGui::Text("FPS: %7.1f", g_Renderer.m_FPS);
        ImGui::Separator();
        ImGui::Text("CPU Time: %8.3f ms", g_Renderer.m_FrameTime);
        ImGui::Separator();
        // Summed from the frame's top-level GPU scopes, so it is the timed GPU work of a whole frame rather than
        // one pass, and it arrives one frame late like every other timer-query value.
        ImGui::Text("GPU Time: %8.3f ms", static_cast<double>(g_Renderer.GetTotalGpuTimeMs()));
        ImGui::Separator();
        ImGui::Text("VRAM: %8.1f MB", g_Renderer.m_RHI->GetVRAMUsageMB());
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

        // The global view switch stays outside the nodes: it is what an inspection session changes constantly, and
        // everything below it is a setting rather than a view.
        static const char* kDebugModes[] = {
            "None",
            "Instances",
            "Meshlets",
            "World Normals",
            "Albedo",
            "Roughness",
            "Metallic",
            "Emissive",
            "LOD",
            "Motion Vectors",
            "ReGIR Cells",
            "Streaming Mip",
            "Cluster IDs",
            "Cluster LOD Level",
            "Cluster Bounds",
            "Triangles"
        };
        ImGui::Combo("Debug Mode", &Config::Get().m_DebugMode, kDebugModes, IM_ARRAYSIZE(kDebugModes));

        // Rendering options
        if (ImGui::TreeNode("Rendering"))
        {
            static const char* kRenderingModes[] = { "Normal", "Reference Pathtracer" };
            int currentMode = static_cast<int>(g_Renderer.m_Mode);
            if (ImGui::Combo("Rendering Mode", &currentMode, kRenderingModes, IM_ARRAYSIZE(kRenderingModes)))
            {
                g_Renderer.m_Mode = static_cast<RenderingMode>(currentMode);
                g_Renderer.ApplyRenderingModeDefaults(g_Renderer.m_Mode);
            }

            ImGui::Checkbox("Enable Sky", &g_Renderer.m_EnableSky);
            ImGui::Checkbox("Enable Animations", &g_Renderer.m_EnableAnimations);
            ImGui::Checkbox("Enable RT Shadows", &g_Renderer.m_EnableRTShadows);

            if (ImGui::TreeNode("Path Tracer"))
            {
                int maxBounces = (int)g_Renderer.m_PathTracerMaxBounces;
                if (ImGui::SliderInt("Max Bounces", &maxBounces, 1, 12))
                {
                    g_Renderer.m_PathTracerMaxBounces = (uint32_t)maxBounces;
                }
                ImGui::TreePop();
            }

            // ── Indirect Lighting Technique ─────────────────────────────────
            if (ImGui::TreeNodeEx("Indirect Lighting", ImGuiTreeNodeFlags_None))
            {
                // Helper: find a renderer by name and set its clear flag.
                auto RequestRendererClear = [](const char* name)
                {
                    for (const std::shared_ptr<IRenderer>& r : g_Renderer.m_Renderers)
                    {
                        if (strcmp(r->GetName(), name) == 0)
                        {
                            r->m_bClearOnNextRender = true;
                            break;
                        }
                    }
                };

                static const char* kIndirectTechniques[] = { "None", "ReSTIR GI", "SHARC", "ReSTIR GI + SHARC" };
                int technique = static_cast<int>(g_Renderer.m_IndirectLightingTechnique);
                if (ImGui::Combo("Technique", &technique, kIndirectTechniques, IM_ARRAYSIZE(kIndirectTechniques)))
                {
                    uint32_t prev = g_Renderer.m_IndirectLightingTechnique;
                    g_Renderer.m_IndirectLightingTechnique = static_cast<uint32_t>(technique);

                    if (technique == srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_RESTIR_GI && prev != srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_RESTIR_GI)
                        RequestRendererClear("RTXDIRenderer");
                    if (technique == srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_SHARC && prev != srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_SHARC)
                        RequestRendererClear("SHARCRenderer");
                    if (technique == srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_RESTIR_GI_SHARC && prev != srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_RESTIR_GI_SHARC)
                    {
                        RequestRendererClear("RTXDIRenderer");
                        RequestRendererClear("SHARCRenderer");
                    }
                }

                // SHARC debug overlay (available in SHARC-only and combined modes)
                if (g_Renderer.m_IndirectLightingTechnique == srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_SHARC ||
                    g_Renderer.m_IndirectLightingTechnique == srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_RESTIR_GI_SHARC)
                {
                    const char* debugModes[] = { "Off", "Voxel Color", "Accumulated Radiance", "Cache Heatmap" };
                    int debugMode = static_cast<int>(g_Renderer.m_SHARCDebugMode);
                    if (ImGui::Combo("SHARC Debug", &debugMode, debugModes, IM_ARRAYSIZE(debugModes)))
                        g_Renderer.m_SHARCDebugMode = static_cast<uint32_t>(debugMode);
                }

                // ReSTIR DI is the direct-lighting half of the same technique choice, so it lives here rather than
                // with the shadowing switches.
                ImGui::Checkbox("ReSTIR DI", &g_Renderer.m_EnableReSTIRDI);
                if (g_Renderer.m_EnableReSTIRDI)
                {
                    extern void RTXDIIMGUISettings();
                    RTXDIIMGUISettings();
                }

                ImGui::TreePop();
            }

            ImGui::TreePop();
        }

        // Level of detail: which path draws geometry and how much of it. Both paths keep a forced-level control, so
        // the two carry the path they belong to in their labels instead of being two identically named sliders in
        // two different nodes.
        const ImGuiTreeNodeFlags lodPanelFlags =
            Config::Get().m_ClusterDebugMode != 0 ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
        const bool bLodPanelOpen = ImGui::TreeNodeEx("Level of Detail", lodPanelFlags);
        g_Renderer.SetClusterCountersPanelOpen(bLodPanelOpen);
        if (bLodPanelOpen)
        {
            ImGui::Checkbox("Use Meshlet Rendering", &g_Renderer.m_UseMeshletRendering);

            // The cluster switch is live: the DAG is cooked at load time (on by default), so this only chooses
            // which path draws. A scene cooked with --cluster 0 has no cluster data to draw from, and then there is
            // nothing to switch to. The worst-case job/group/node counts are clamped to at least one so their
            // buffers are never empty, so the depth (which has no such clamp) is what tells the two apart.
            const bool bHasClusterData = g_Renderer.m_Scene.m_MaxClusterTreeDepth > 0;
            ImGui::BeginDisabled(!bHasClusterData);
            ImGui::Checkbox("Enable Cluster LOD", &g_Renderer.m_EnableClusterRendering);
            ImGui::EndDisabled();
            if (!bHasClusterData)
                ImGui::TextDisabled("No cooked cluster data in this scene: it was cooked with --cluster 0.");

            ImGui::SeparatorText("Cluster traversal");
            ImGui::SliderFloat("LOD pixel error", &Config::Get().m_ClusterPixelError, 0.05f, 8.0f, "%.2f px");
            ImGui::SliderInt("Forced cluster LOD", &Config::Get().m_ClusterForcedLOD, -1, static_cast<int>(ClusterLod::kMaxLODLevels) - 1,
                Config::Get().m_ClusterForcedLOD < 0 ? "metric" : "%d");

            int maxClusters = Config::Get().m_ClusterMaxClusters;
            if (ImGui::DragInt("Max clusters (0 = no cap)", &maxClusters, 512.0f, 0, 1 << 20))
            {
                Config::Get().m_ClusterMaxClusters = std::max(maxClusters, 0);
            }

            // Needs both buckets' depth to be worth re-testing, so it reads as off when occlusion culling is off.
            ImGui::BeginDisabled(!g_Renderer.m_EnableOcclusionCulling);
            ImGui::Checkbox("Two-pass culling", &Config::Get().m_ClusterTwoPass);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("Re-tests what the first cluster pass rejected against this frame's depth and draws\n"
                    "what survives. If the sweep in panels above shows 0 revived clusters on a moving camera, the\n"
                    "first pass had nothing left to reject.");
            }

            // The three cluster views are Debug Modes, so this combo is only the traversal's counter readback.
            // The copy runs while the counters are on screen, so a closed panel costs nothing.
            static const char* const kClusterDebugViewNames[] = { "Off", "Traversal Counters" };
            static constexpr uint32_t kClusterDebugViewModes[] = { 0, 4 };

            int clusterDebugView = 0;
            for (uint32_t i = 0; i < uint32_t(IM_ARRAYSIZE(kClusterDebugViewNames)); ++i)
            {
                if (kClusterDebugViewModes[i] == Config::Get().m_ClusterDebugMode)
                    clusterDebugView = int(i);
            }

            if (ImGui::Combo("Counters", &clusterDebugView, kClusterDebugViewNames, IM_ARRAYSIZE(kClusterDebugViewNames)))
            {
                Config::Get().m_ClusterDebugMode = kClusterDebugViewModes[clusterDebugView];
            }

            if (Config::Get().m_ClusterDebugMode != 4)
            {
                ImGui::TextDisabled("Pick 'Traversal Counters' above to read the GPU's counters for the traversal.");
            }
            else
            {
                for (uint32_t bucketIndex = 0; bucketIndex < 2; ++bucketIndex)
                {
                    const char* bucketName = bucketIndex == 0 ? "Opaque" : "Masked";
                    const Scene::BucketInfo& bucket = bucketIndex == 0 ? scene.m_OpaqueBucket : scene.m_MaskedBucket;

                    // A bucket with no instances never runs a traversal, so its counters would stay on "waiting".
                    if (bucket.m_Count == 0)
                    {
                        ImGui::Text("%s: no instances", bucketName);
                        continue;
                    }

                    if (!g_Renderer.ClusterCountersValid(bucketIndex))
                    {
                        ImGui::Text("%s: (waiting for the first readback)", bucketName);
                        continue;
                    }

                    const srrhi::ClusterCounters& counters = g_Renderer.GetClusterCounters(bucketIndex);
                    ImGui::Text("%s: %u instances, %u nodes, %u groups", bucketName,
                        counters.m_InstanceCount, counters.m_NodeCount, counters.m_GroupCount);
                    ImGui::Text("    %u clusters, %u capped", counters.m_ClusterCount, counters.m_ClusterCapped);
                    ImGui::Text("    rejected %u nodes, %u groups -> %u revived", counters.m_RejectNodeCount,
                        counters.m_RejectGroupCount, counters.m_SecondPassClusterCount);
                }

                // The traversal's own scopes, as GpuPassTimers recorded them for the frame the counters came from.
                float traversalMilliseconds = 0.0f;
                for (const GpuPassTimers::Entry& entry : g_Renderer.m_GpuPassTimers.GetEntries())
                {
                    if (entry.m_Name.rfind("Cluster Traversal", 0) == 0)
                        traversalMilliseconds += entry.m_Milliseconds;
                }
                ImGui::Text("GPU: %.3f ms", static_cast<double>(traversalMilliseconds));
            }

            ImGui::SeparatorText("Meshlet path");
            const char* lodNames[] = { "Auto", "LOD 0", "LOD 1", "LOD 2", "LOD 3", "LOD 4", "LOD 5", "LOD 6", "LOD 7" };
            int forcedLODIdx = Config::Get().m_ForcedLOD + 1;
            if (ImGui::SliderInt("Forced meshlet LOD", &forcedLODIdx, 0, srrhi::CommonConsts::MAX_LOD_COUNT))
            {
                Config::Get().m_ForcedLOD = forcedLODIdx - 1;
            }
            ImGui::SameLine();
            ImGui::Text("%s", lodNames[forcedLODIdx]);

            ImGui::TreePop();
        }

        // Post-processing settings
        if (ImGui::TreeNode("Post-Processing"))
        {
            if (ImGui::TreeNode("Bloom"))
            {
                ImGui::Checkbox("Enable Bloom", &g_Renderer.m_EnableBloom);
                ImGui::Checkbox("Debug Bloom Only", &g_Renderer.m_DebugBloom);
                ImGui::DragFloat("Bloom Intensity", &g_Renderer.m_BloomIntensity, 0.001f, 0.0f, 1.0f);
                ImGui::DragFloat("Bloom Knee", &g_Renderer.m_BloomKnee, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Bloom Threshold", &g_Renderer.m_BloomThreshold, 0.01f, 0.0f, 10.0f);
                ImGui::DragFloat("Bloom Upsample Radius", &g_Renderer.m_UpsampleRadius, 0.01f, 0.1f, 2.0f);
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Exposure"))
            {
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

            ImGui::TreePop();
        }

        // Lighting: the sun's orientation and every light's own settings
        if (ImGui::TreeNode("Lighting"))
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
        // Anti-aliasing (FSR3): the temporal resolve, kept with the other image-quality settings.
        if (ImGui::TreeNode("Temporal Anti-Aliasing"))
        {
            ImGui::Checkbox("Enable TAA", &g_Renderer.m_bTAAEnabled);

            if (g_Renderer.m_bTAAEnabled)
            {
                ImGui::Checkbox("TAA debug view", &g_Renderer.m_bTAADebugView);
                ImGui::SliderFloat("Sharpness", &g_Renderer.m_TAASharpness, 0.0f, 1.0f);
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

            // Highlighting is a viewer aid for the node the camera is looking at, so it belongs with the camera.
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

            ImGui::TreePop();
        }

        // Culling controls: both paths' switches, so a validation run can find all of them in one place.
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

            // The cluster traversal culls from its own bounds, so it keeps its own pair of switches.
            ImGui::SeparatorText("Cluster traversal");
            ImGui::Checkbox("Disable Cluster Culling", &Config::Get().m_ClusterDisableCulling);

            const bool bPrevClusterFreeze = Config::Get().m_ClusterFreezeCulling;
            ImGui::Checkbox("Freeze Cluster Culling Camera", &Config::Get().m_ClusterFreezeCulling);
            if (!bPrevClusterFreeze && Config::Get().m_ClusterFreezeCulling)
            {
                scene.m_FrozenCullingViewMatrix = scene.m_Camera.GetViewMatrix();
                scene.m_FrozenCullingCameraPos = scene.m_Camera.GetPosition();
            }

            ImGui::TreePop();
        }

        // Opacity micromaps: the toggle relinks every BLAS, so it is a load-time cost rather than a frame knob.
        if (ImGui::TreeNode("Opacity Micromaps"))
        {
            ImGui::BeginDisabled(!g_Renderer.m_bOmmSupported);
            bool bEnableOmm = g_Renderer.m_bEnableOmm;
            if (ImGui::Checkbox("Enable OMM", &bEnableOmm))
            {
                g_Renderer.m_bEnableOmm = bEnableOmm;
                g_Renderer.m_bOmmRebuildRequested = true;
                g_Renderer.m_bOmmDebugDirty = true;
            }
            ImGui::EndDisabled();

            if (!g_Renderer.m_bOmmSupported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("The device does not report Feature::RayTracingOpacityMicromap");
            }

            // Scene totals straight from the bake results the BLASes were built from.
            uint64_t maskCount = 0;
            uint64_t dataBytes = 0;
            uint64_t maskedTriangles = 0;
            int primitiveCount = 0;
            for (const Scene::Mesh& mesh : scene.m_Meshes)
            {
                for (const Scene::Primitive& primitive : mesh.m_Primitives)
                {
                    ++primitiveCount;
                    for (const OmmLodData& lod : primitive.m_Omm)
                    {
                        maskCount += lod.m_Stats.m_MaskCount;
                        dataBytes += lod.m_Stats.m_DataBytes;
                        maskedTriangles += lod.m_Stats.m_MaskedTriangles;
                    }
                }
            }
            ImGui::Text("%llu masks, %.2f MiB, %llu masked triangles", static_cast<unsigned long long>(maskCount),
                        double(dataBytes) / (1024.0 * 1024.0), static_cast<unsigned long long>(maskedTriangles));

            // The classification view is mode 1; the micromap grid and the candidate heatmap are not implemented.
            static const char* kDebugViews[] = { "Off", "Texel classification" };
            int debugMode = std::min(g_Renderer.m_OmmDebugMode, 1);
            if (ImGui::Combo("OMM debug view", &debugMode, kDebugViews, IM_ARRAYSIZE(kDebugViews)))
            {
                g_Renderer.m_OmmDebugMode = debugMode;
                g_Renderer.m_bOmmDebugDirty = true;
            }

            if (g_Renderer.m_OmmDebugMode == 1)
            {
                if (ImGui::SliderInt("Primitive", &Config::Get().m_OmmDebugPrimitive, 0, std::max(0, primitiveCount - 1)))
                    g_Renderer.m_bOmmDebugDirty = true;
                if (ImGui::SliderInt("Alpha mip", &Config::Get().m_OmmDebugMip, 0, 6))
                    g_Renderer.m_bOmmDebugDirty = true;
                ImGui::Checkbox("Mixed only", &g_Renderer.m_bOmmShowOnlyMixed);
                if (ImGui::Button("Refresh"))
                    g_Renderer.m_bOmmDebugDirty = true;

                const OmmDebugTexelStats& stats = g_Renderer.m_OmmDebugStats;
                if (stats.m_Width > 0)
                {
                    const double texels = double(stats.m_Width) * double(stats.m_Height);
                    ImGui::Text("%ux%u: %.1f%% opaque, %.1f%% transparent, %.1f%% mixed", stats.m_Width, stats.m_Height,
                                100.0 * stats.m_OpaqueCount / texels, 100.0 * stats.m_TransparentCount / texels,
                                100.0 * stats.m_MixedCount / texels);
                }
                else
                {
                    ImGui::TextDisabled("No readable alpha texture for this primitive");
                }
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

            // The capture itself needs a frame boundary, so the button only arms it for the next frame.
            ImGui::SameLine();
            ImGui::BeginDisabled(!g_RenderDoc.IsAvailable());
            if (ImGui::Button("RenderDoc Capture"))
            {
                g_Renderer.m_bRenderDocCaptureRequested = true;
            }
            ImGui::EndDisabled();

            if (!g_RenderDoc.IsAvailable() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip(RenderDocCapture::IsCompiledIn()
                    ? "RenderDoc is unavailable: no renderdoc.dll was loaded (pass --renderdoc=<path>)"
                    : "RenderDoc is unavailable: this build has no RenderDoc support (renderdoc_app.h was not found when CMake ran)");
            }

            if (!g_Renderer.m_RenderDocLastCapturePath.empty())
            {
                ImGui::TextWrapped("Last capture: %s", g_Renderer.m_RenderDocLastCapturePath.c_str());
            }

            // PIX arms the same way: the capture has to bracket a whole frame.
            ImGui::SameLine();
            ImGui::BeginDisabled(!g_Pix.IsAvailable());
            if (ImGui::Button("PIX Capture"))
            {
                g_Renderer.m_bPixCaptureRequested = true;
            }
            ImGui::EndDisabled();

            if (!g_Pix.IsAvailable() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip(PixCapture::IsCompiledIn()
                    ? "PIX is unavailable: no WinPixGpuCapturer.dll was found (pass --pix=<path>)"
                    : "PIX is unavailable: this build has no PIX support (WinPixEventRuntime was not found when CMake ran)");
            }

            if (!g_Renderer.m_PixLastCapturePath.empty())
            {
                ImGui::TextWrapped("Last PIX capture: %s", g_Renderer.m_PixLastCapturePath.c_str());
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

                // Timings come from the timer queries each GPU scope opens, resolved one frame late.
                const GpuPassTimers& gpuTimers = g_Renderer.m_GpuPassTimers;
                const std::vector<GpuPassTimers::Entry>& gpuEntries = gpuTimers.GetEntries();
                const uint32_t gpuFrame = gpuTimers.GetLastResolvedFrame();

                // "-" rather than 0.000: a scope whose query had not completed is not 0 ms.
                auto drawGpuTime = [&](const GpuPassTimers::Entry& entry)
                {
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.3f", entry.m_Milliseconds);
                };

                auto findEntry = [&](std::string_view group, std::string_view name) -> const GpuPassTimers::Entry*
                {
                    for (const GpuPassTimers::Entry& entry : gpuEntries)
                    {
                        if (entry.m_Frame == gpuFrame && entry.m_Name == name && entry.m_Group == group)
                        {
                            return &entry;
                        }
                    }
                    return nullptr;
                };

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

                    // The renderer's own scope, then every dispatch it recorded underneath it.
                    if (const GpuPassTimers::Entry* entry = findEntry({}, r->GetName()))
                    {
                        drawGpuTime(*entry);
                    }
                    else
                    {
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted("-");
                    }

                    for (const GpuPassTimers::Entry& entry : gpuEntries)
                    {
                        if (entry.m_Frame != gpuFrame || entry.m_Group != r->GetName())
                        {
                            continue;
                        }

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("    %s", entry.m_Name.c_str());
                        drawGpuTime(entry);
                    }
                }

                // Scopes recorded outside a renderer: the load's uploads and mip chains, the tile work, and
                // anything a renderer does not own.
                for (const GpuPassTimers::Entry& entry : gpuEntries)
                {
                    if (entry.m_Frame != gpuFrame || !entry.m_Group.empty())
                    {
                        continue;
                    }

                    bool bIsRendererScope = false;
                    for (const std::shared_ptr<IRenderer>& r : g_Renderer.m_Renderers)
                    {
                        bIsRendererScope |= (entry.m_Name == r->GetName());
                    }
                    if (bIsRendererScope)
                    {
                        continue;
                    }

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%s", entry.m_Name.c_str());
                    drawGpuTime(entry);
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

        // ─── Texture Streaming Stats ──────────────────────────────────────────
        if (ImGui::TreeNode("Texture Streaming"))
        {
            const nvfeedback::FeedbackManagerStats& stats = g_Renderer.m_FeedbackManager->GetStats();

            // The sample override and the packed-mips switch are the two controls that make the stats below move on
            // demand, so they sit above the numbers rather than with the rendering features.
            static const char* kMipNames[] = { "Auto", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15" };
            int forcedMipIdx = g_Renderer.m_ForcedTextureMip + 1;
            if (ImGui::SliderInt("Forced texture mip", &forcedMipIdx, 0, 16))
            {
                g_Renderer.m_ForcedTextureMip = forcedMipIdx - 1;
            }
            ImGui::SameLine();
            ImGui::Text("%s", kMipNames[forcedMipIdx]);

            // Test switch: stream every standard tile out so only the packed (mip-tail) levels stay
            // resident; "Heap Allocated" and the VRAM readout below should drop within a second.
            bool bPackedMipsOnly = g_Renderer.m_FeedbackManager->IsPackedMipsOnly();
            if (ImGui::Checkbox("Packed mips only", &bPackedMipsOnly))
                g_Renderer.m_FeedbackManager->SetPackedMipsOnly(bPackedMipsOnly);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Stop streaming: keep only the packed mip levels and unload every streamed tile");

            ImGui::SeparatorText("Memory");
            ImGui::Text("Heaps:           %u (TTM: %u, Packed-Mip: %u)",
                        stats.m_NumHeaps,
                        stats.m_NumTTMHeaps,
                        stats.m_NumHeaps - stats.m_NumTTMHeaps);
            ImGui::Text("Heap Allocated:  %.1f MB", BYTES_TO_MB(stats.m_HeapAllocationInBytes));
            ImGui::Text("Heap Free Tiles: %u", stats.m_HeapTilesFree);
            ImGui::Text("Tiles Total:     %u", stats.m_TilesTotal);

            ImGui::SeparatorText("Activity");
            uint32_t pct = stats.m_TilesTotal > 0
                ? (stats.m_TilesAllocated * 100u / stats.m_TilesTotal) : 0u;
            ImGui::Text("Tiles Allocated: %u (%u%%)", stats.m_TilesAllocated, pct);
            ImGui::Text("Tiles Standby:   %u", stats.m_TilesStandby);

            // ── Bandwidth moving average graph ──
            ImGui::SeparatorText("Bandwidth (MB/s)");
            {
                // Lazily initialize ring buffer
                static std::vector<float> s_BandwidthHistory;
                static uint32_t s_BandwidthHistoryIndex = 0;
                if (s_BandwidthHistory.empty())
                {
                    s_BandwidthHistory.resize(128, 0.0f);
                    s_BandwidthHistoryIndex = 0;
                }

                // Count tiles submitted this frame (set by UpdateStreamingPreRender)
                uint32_t tilesSubmitted = g_Renderer.m_TilesSubmittedThisFrame;

                // Accumulate time and tiles, push a sample at 30 Hz regardless of I/O activity
                static float s_TimeAccum = 0.0f;
                static uint32_t s_TileAccum = 0;
                constexpr float kSampleInterval = 1.0f / 30.0f;
                float dt = ImGui::GetIO().DeltaTime;
                s_TimeAccum += dt;
                s_TileAccum += tilesSubmitted;

                if (s_TimeAccum >= kSampleInterval)
                {
                    float mbps = (float)(s_TileAccum * 64u * 1024u)
                               / (s_TimeAccum * 1000.0f * 1000.0f);
                    uint32_t idx = s_BandwidthHistoryIndex;
                    s_BandwidthHistory[idx] = mbps;
                    s_BandwidthHistoryIndex = (idx + 1) % (uint32_t)s_BandwidthHistory.size();
                    s_TimeAccum = 0.0f;
                    s_TileAccum = 0;
                }

                // Build draw buffer in order (oldest first)
                auto& hist = s_BandwidthHistory;
                uint32_t head = s_BandwidthHistoryIndex;
                std::vector<float> drawBuffer;
                drawBuffer.reserve(hist.size());
                drawBuffer.insert(drawBuffer.end(), hist.begin() + head, hist.end());
                drawBuffer.insert(drawBuffer.end(), hist.begin(), hist.begin() + head);

                // Compute avg bandwidth from last values
                float avgBw = 0.0f;
                for (float v : drawBuffer) avgBw += v;
                avgBw /= (float)drawBuffer.size();

                char overlay[64];
                snprintf(overlay, sizeof(overlay), "Bandwidth (MB/s) avg = %.2f", avgBw);

                float graphMax = 0.0f;
                for (float v : drawBuffer) graphMax = std::max(graphMax, v);
                float graphMaxScale = 12.5f;
                while (graphMaxScale < graphMax) graphMaxScale *= 2.0f;

                ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                ImGui::PlotLines("##bwgraph", drawBuffer.data(), (int)drawBuffer.size(),
                    0, overlay, 0.0f, graphMaxScale, ImVec2(0, 50.0f));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Bandwidth (MB/s) max: %.2f, scale: %.2f", graphMax, graphMaxScale);
                ImGui::PopStyleColor();

                ImGui::Text("Tiles submitted this frame: %u", tilesSubmitted);
            }

            ImGui::SeparatorText("CPU Timing");
            ImGui::Text("BeginFrame:       %.3f ms", stats.m_CpuTimeBeginFrame * 1000.0);
            ImGui::Text("UpdateMappings:   %.3f ms", stats.m_CpuTimeUpdateTileMappings * 1000.0);
            ImGui::Text("ResolveFeedback:  %.3f ms", stats.m_CpuTimeResolve * 1000.0);

            ImGui::SeparatorText("Tile Residency Debug");
            {
                const uint32_t numTex = g_Renderer.m_FeedbackManager->GetNumTextures();

                static std::vector<std::string> s_TexNames;
                static std::vector<const char*> s_TexNamePtrs;
                s_TexNames.clear();
                s_TexNamePtrs.clear();

                s_TexNames.push_back("None");
                s_TexNamePtrs.push_back(s_TexNames.back().c_str());

                for (uint32_t i = 0; i < numTex; ++i)
                {
                    nvfeedback::FeedbackTexture* ft = g_Renderer.m_FeedbackManager->GetTextureByIndex(i);
                    int userIdx = ft->GetUserIndex();
                    std::string name;
                    if (userIdx >= 0 && userIdx < (int)g_Renderer.m_Scene.m_Textures.size())
                    {
                        const std::string& uri = g_Renderer.m_Scene.m_Textures[userIdx].m_Uri;
                        if (!uri.empty())
                            name = std::filesystem::path(uri).filename().string();
                    }
                    if (name.empty())
                        s_TexNames.push_back("Texture " + std::to_string(i));
                    else
                        s_TexNames.push_back(name);
                    s_TexNamePtrs.push_back(s_TexNames.back().c_str());
                }

                int selectedIdx = g_Renderer.m_TileResidencyDebugTextureIdx + 1;
                if (selectedIdx >= (int)s_TexNamePtrs.size())
                    selectedIdx = 0; // clamp if previously-selected texture was unregistered

                if (ImGui::Combo("Texture", &selectedIdx, s_TexNamePtrs.data(), (int)s_TexNamePtrs.size()))
                {
                    g_Renderer.m_TileResidencyDebugTextureIdx = selectedIdx - 1;
                }

                if (g_Renderer.m_TileResidencyDebugTextureIdx >= 0)
                {
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Visualizing: %s", s_TexNamePtrs[selectedIdx]);
                    ImGui::Text("Black tiles = not streamed in yet");
                }
            }

            ImGui::TreePop();
        }

        // ─── Streaming VRAM budget ────────────────────────────────────────────
        // One cap for both streaming systems - the texture tile heaps and the cluster group arena - because they
        // spend the same VRAM. The arena takes its group data out of it first, so the tile heaps budget against
        // what is left. Lowering it is the stress test: both start evicting within a frame or two, and raising it
        // back lets them grow again.
        ImGui::SeparatorText("Streaming VRAM");
        {
            const uint64_t totalMB = g_Renderer.m_RHI->GetVRAMTotalBytes() / (1024ull * 1024ull);
            const int maxBudgetMB = int(totalMB != 0 ? totalMB : 16384);
            int budgetMB = int(std::min<uint64_t>(g_Renderer.m_StreamingBudgetMB, uint64_t(maxBudgetMB)));

            const double texMB = BYTES_TO_MB(g_Renderer.m_FeedbackManager->GetStats().m_HeapAllocationInBytes);
            const double geoMB = g_Renderer.m_GeometryStreaming
                ? BYTES_TO_MB(g_Renderer.m_GeometryStreaming->GetStats().m_ArenaBytes) : 0.0;

            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderInt("VRAM budget (MB)", &budgetMB, 1, maxBudgetMB, "%d MB"))
                g_Renderer.m_StreamingBudgetMB = uint64_t(std::max(budgetMB, 1));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Cap on the texture tile heaps and the cluster group arena together; geometry\n"
                                  "reserves its group data first and the tiles get the rest.\n"
                                  "Lower it to stress both streaming systems; the GPU has %llu MB.\n"
                                  "The arena only streams below the scene's own group data (%.1f MB here):\n"
                                  "above that every group simply stays resident.",
                                  static_cast<unsigned long long>(totalMB), geoMB);

            const double tileCapMB = BYTES_TO_MB(g_Renderer.GetTileStreamingBudgetBytes());
            const bool bTextureOverBudget = g_Renderer.m_FeedbackManager->GetStats().m_bOverBudget;
            ImGui::Text("Tiles: %.1f / %.1f MB, geometry: %.1f MB, of %llu MB%s", texMB, tileCapMB, geoMB,
                static_cast<unsigned long long>(g_Renderer.m_StreamingBudgetMB),
                bTextureOverBudget ? "  (tiles over budget)" : "");
        }

        // ─── Cluster Geometry Streaming ───────────────────────────────────────
        // The cluster counterpart of the texture panel above: the same "device asks, host serves" exchange, so the
        // same questions apply - how much is resident, how hard is it working, and what is it spending it on.
        if (g_Renderer.m_GeometryStreaming && ImGui::TreeNode("Cluster Streaming"))
        {
            const GeometryStreaming& streaming = *g_Renderer.m_GeometryStreaming;
            const GeometryStreaming::Stats& stats = streaming.GetStats();

            int ageThreshold = int(Config::Get().m_ClusterStreamAgeThreshold);
            if (ImGui::SliderInt("Unload age", &ageThreshold, 2, 240, "%d frames"))
                Config::Get().m_ClusterStreamAgeThreshold = uint32_t(ageThreshold);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Frames a resident group may go untouched before the age filter asks for it back");

            ImGui::SeparatorText("Memory");
            ImGui::Text("Groups: %u / %u resident", stats.m_ResidentGroups, stats.m_GroupCount);
            ImGui::Text("Arena:  %.2f / %.2f MB", BYTES_TO_MB(stats.m_ResidentBytes), BYTES_TO_MB(stats.m_ArenaBytes));
            ImGui::Text("Not in a blob: %.2f MB", BYTES_TO_MB(stats.m_WastedBytes));

            ImGui::SeparatorText("Activity");
            ImGui::Text("Loaded %u / unloaded %u this frame", stats.m_LoadedThisFrame, stats.m_UnloadedThisFrame);
            ImGui::Text("Requests %u, %u active groups", stats.m_RequestsLastFrame, stats.m_ActiveGroups);
            ImGui::Text("Dropped loads: %u, evicted by budget: %u", stats.m_DroppedLoads, stats.m_EvictedByBudget);

            // ── Resident geometry over time ──
            ImGui::SeparatorText("Resident geometry (MB)");
            {
                static std::vector<float> s_ResidentHistory;
                static uint32_t s_HistoryIndex = 0;
                static float s_TimeAccum = 0.0f;
                if (s_ResidentHistory.empty())
                {
                    s_ResidentHistory.resize(128, 0.0f);
                    s_HistoryIndex = 0;
                }

                // Sampled at 30 Hz like the texture bandwidth graph, so a burst of loads reads as one step rather
                // than as a frame of noise.
                s_TimeAccum += ImGui::GetIO().DeltaTime;
                constexpr float kSampleInterval = 1.0f / 30.0f;
                if (s_TimeAccum >= kSampleInterval)
                {
                    s_ResidentHistory[s_HistoryIndex] = float(BYTES_TO_MB(stats.m_ResidentBytes));
                    s_HistoryIndex = (s_HistoryIndex + 1) % uint32_t(s_ResidentHistory.size());
                    s_TimeAccum = 0.0f;
                }

                std::vector<float> drawBuffer;
                drawBuffer.reserve(s_ResidentHistory.size());
                drawBuffer.insert(drawBuffer.end(), s_ResidentHistory.begin() + s_HistoryIndex, s_ResidentHistory.end());
                drawBuffer.insert(drawBuffer.end(), s_ResidentHistory.begin(), s_ResidentHistory.begin() + s_HistoryIndex);

                float peak = 0.0f;
                for (float v : drawBuffer)
                    peak = std::max(peak, v);
                const float graphMax = std::max(1.0f, peak * 1.1f);

                char overlay[96];
                snprintf(overlay, sizeof(overlay), "Resident MB (arena %.1f MB)", BYTES_TO_MB(stats.m_ArenaBytes));
                ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.4f, 0.9f, 1.0f, 1.0f));
                ImGui::PlotLines("##clusterresidentgraph", drawBuffer.data(), (int)drawBuffer.size(), 0, overlay,
                    0.0f, graphMax, ImVec2(0, 60.0f));
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Peak in the window: %.2f MB, scale: %.2f MB", peak, graphMax);
            }

            // ── Per-level residency histogram ──
            ImGui::SeparatorText("Resident groups per LOD level");
            if (stats.m_LevelCount > 0)
            {
                // A share rather than a count, because levels differ by orders of magnitude in group count: a full
                // bar means every group of that level is in the arena. The scale follows the tallest bar so the
                // shape stays readable when nothing is fully resident, and the peak is printed in the overlay.
                float share[ClusterLod::kMaxLODLevels] = {};
                float peakShare = 0.0f;
                for (uint32_t level = 0; level < stats.m_LevelCount; ++level)
                {
                    const uint32_t total = stats.m_GroupsPerLevel[level];
                    share[level] = total > 0 ? float(stats.m_ResidentPerLevel[level]) / float(total) : 0.0f;
                    peakShare = std::max(peakShare, share[level]);
                }

                char overlay[64];
                snprintf(overlay, sizeof(overlay), "Share per level, peak %.0f%%", peakShare * 100.0f);

                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.4f, 0.9f, 1.0f, 1.0f));
                ImGui::PlotHistogram("##clusterlevelhist", share, (int)stats.m_LevelCount, 0, overlay,
                    0.0f, std::max(0.1f, peakShare * 1.15f), ImVec2(0, 70.0f));
                ImGui::PopStyleColor();

                if (ImGui::IsItemHovered())
                {
                    std::string tip = "Level: resident / groups\n";
                    for (uint32_t level = 0; level < stats.m_LevelCount; ++level)
                    {
                        tip += "L" + std::to_string(level) + ": " +
                            std::to_string(stats.m_ResidentPerLevel[level]) + " / " +
                            std::to_string(stats.m_GroupsPerLevel[level]) + "\n";
                    }
                    ImGui::SetTooltip("%s", tip.c_str());
                }
            }
            else
            {
                ImGui::TextDisabled("No cluster groups");
            }

            ImGui::TreePop();
        }
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


