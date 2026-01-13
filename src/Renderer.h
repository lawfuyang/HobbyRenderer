#pragma once

#include "pch.h"
#include "GraphicRHI.h"
#include "ImGuiLayer.h"
#include "Scene.h"
#include "Camera.h"

class IRenderer
{
public:
    virtual ~IRenderer() = default;
    virtual bool Initialize() = 0;
    virtual void Render(nvrhi::CommandListHandle commandList) = 0;
    virtual const char* GetName() const = 0;
};

class RendererRegistry
{
public:
    using Creator = std::function<std::shared_ptr<IRenderer>()>;

    static void RegisterRenderer(Creator creator)
    {
        s_Creators.push_back(creator);
    }

    static const std::vector<Creator>& GetCreators()
    {
        return s_Creators;
    }

private:
    inline static std::vector<Creator> s_Creators;
};

struct Renderer
{
    // ShaderMake default SPIRV register shifts (from external/ShaderMake/ShaderMake/ShaderMake.cpp Options)
    static constexpr uint32_t SPIRV_SAMPLER_SHIFT = 100;  // sRegShift (s#)
    static constexpr uint32_t SPIRV_TEXTURE_SHIFT = 200;  // tRegShift (t#)
    static constexpr uint32_t SPIRV_CBUFFER_SHIFT = 300;  // bRegShift (b#)
    static constexpr uint32_t SPIRV_UAV_SHIFT = 400;      // uRegShift (u#)
    // Instance management
    static void SetInstance(Renderer* instance);
    static Renderer* GetInstance();

    // Lifecycle
    bool Initialize();
    void Run();
    void Shutdown();

    // Command list helpers
    nvrhi::CommandListHandle AcquireCommandList(std::string_view markerName);
    void SubmitCommandList(const nvrhi::CommandListHandle& commandList);
    void ExecutePendingCommandLists();

    // Swapchain / backbuffer
    nvrhi::TextureHandle GetCurrentBackBufferTexture() const;

    // ImGui helpers
    // Prepares ImGui frame (calls NewFrame(), builds UI, and calls ImGui::Render()).
    void UpdateImGuiFrame();

    // Create or retrieve a cached binding layout derived from a BindingSetDesc.
    nvrhi::BindingLayoutHandle GetOrCreateBindingLayoutFromBindingSetDesc(const nvrhi::BindingSetDesc& setDesc, nvrhi::ShaderType visibility = nvrhi::ShaderType::All);

    // Get or create a graphics pipeline given a full pipeline description and framebuffer info. The pipeline will be cached internally by the renderer.
    nvrhi::GraphicsPipelineHandle GetOrCreateGraphicsPipeline(const nvrhi::GraphicsPipelineDesc& pipelineDesc, const nvrhi::FramebufferInfoEx& fbInfo);

    // Retrieve a shader handle by name (output stem), e.g., "imgui_VSMain" or "imgui_PSMain"
    nvrhi::ShaderHandle GetShaderHandle(std::string_view name) const;

    // Public state & resources
    SDL_Window* m_Window = nullptr;
    GraphicRHI m_RHI{};
    nvrhi::DeviceHandle m_NvrhiDevice;

    // Swapchain textures
    nvrhi::TextureHandle m_SwapchainTextures[GraphicRHI::SwapchainImageCount] = {};
    uint32_t m_CurrentSwapchainImage = 0;
    // Depth buffer for main framebuffer
    nvrhi::TextureHandle m_DepthTexture;

    // Cached shader handles loaded from compiled SPIR-V binaries (keyed by output stem, e.g., "imgui_VSMain")
    std::unordered_map<std::string, nvrhi::ShaderHandle> m_ShaderCache;

    // ImGui state
    ImGuiLayer m_ImGuiLayer;
    // Scene
    Scene m_Scene;
    // Camera
    Camera m_Camera;
    // Renderers
    std::vector<std::shared_ptr<IRenderer>> m_Renderers;
    double m_FrameTime = 0.0;
    double m_FPS = 0.0;

    // Return last frame time in milliseconds
    double GetFrameTimeMs() const { return m_FrameTime; }

    // Command list pools
    std::vector<nvrhi::CommandListHandle> m_CommandListFreeList;
    std::vector<nvrhi::CommandListHandle> m_PendingCommandLists;

    // Caches
    std::unordered_map<size_t, nvrhi::BindingLayoutHandle> m_BindingLayoutCache;
    std::unordered_map<size_t, nvrhi::GraphicsPipelineHandle> m_GraphicsPipelineCache;

private:
    // Device / swapchain helpers
    bool CreateNvrhiDevice();
    void DestroyNvrhiDevice();
    bool CreateSwapchainTextures();
    void DestroySwapchainTextures();

    // Shader loading
    bool LoadShaders();
    void UnloadShaders();

    static Renderer* s_Instance;
};
