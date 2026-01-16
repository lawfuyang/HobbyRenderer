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

    float m_CPUTime = 0.0f;
    float m_GPUTime = 0.0f;
    nvrhi::TimerQueryHandle m_GPUQueries[2];
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

// Macro to register a renderer class
#define REGISTER_RENDERER(ClassName) \
IRenderer* g_##ClassName = nullptr; \
static bool s_##ClassName##Registered = []() { \
    RendererRegistry::RegisterRenderer([]() { \
        auto renderer = std::make_shared<ClassName>(); \
        SDL_assert(renderer && "Failed to initialize renderer: " #ClassName ); \
        g_##ClassName = renderer.get(); \
        SDL_assert(g_##ClassName && "Failed to assign global renderer pointer: " #ClassName ); \
        return renderer; \
    }); \
    return true; \
}();

struct Renderer
{
    // ============================================================================
    // Constants
    // ============================================================================

    // ShaderMake default SPIRV register shifts (from external/ShaderMake/ShaderMake/ShaderMake.cpp Options)
    static constexpr uint32_t SPIRV_SAMPLER_SHIFT   = 100;  // sRegShift (s#)
    static constexpr uint32_t SPIRV_TEXTURE_SHIFT   = 200;  // tRegShift (t#)
    static constexpr uint32_t SPIRV_CBUFFER_SHIFT   = 300;  // bRegShift (b#)
    static constexpr uint32_t SPIRV_UAV_SHIFT       = 400;  // uRegShift (u#)

    // ============================================================================
    // Instance Management
    // ============================================================================

    static void SetInstance(Renderer* instance);
    static Renderer* GetInstance();

    // ============================================================================
    // Lifecycle
    // ============================================================================

    bool Initialize();
    void Run();
    void Shutdown();

    // ============================================================================
    // Command List Management
    // ============================================================================

    nvrhi::CommandListHandle AcquireCommandList(std::string_view markerName);
    void SubmitCommandList(const nvrhi::CommandListHandle& commandList);
    void ExecutePendingCommandLists();

    // ============================================================================
    // Swapchain / Backbuffer
    // ============================================================================

    nvrhi::TextureHandle GetCurrentBackBufferTexture() const;

    // ============================================================================
    // Binding Layouts & Pipelines
    // ============================================================================

    // Create or retrieve a cached binding layout derived from a BindingSetDesc.
    nvrhi::BindingLayoutHandle GetOrCreateBindingLayoutFromBindingSetDesc(
        const nvrhi::BindingSetDesc& setDesc,
        nvrhi::ShaderType visibility = nvrhi::ShaderType::All);

    // Create or retrieve a cached bindless layout.
    nvrhi::BindingLayoutHandle GetOrCreateBindlessLayout(const nvrhi::BindlessLayoutDesc& desc);

    // Get or create a graphics pipeline given a full pipeline description and framebuffer info.
    // The pipeline will be cached internally by the renderer.
    nvrhi::GraphicsPipelineHandle GetOrCreateGraphicsPipeline(
        const nvrhi::GraphicsPipelineDesc& pipelineDesc,
        const nvrhi::FramebufferInfoEx& fbInfo);

    // ============================================================================
    // Shaders
    // ============================================================================

    // Retrieve a shader handle by name (output stem), e.g., "imgui_VSMain" or "imgui_PSMain"
    nvrhi::ShaderHandle GetShaderHandle(std::string_view name) const;

    // ============================================================================
    // Global Bindless Texture System
    // ============================================================================

    // Initialize global bindless texture system
    bool InitializeGlobalBindlessTextures();

    // Register a texture in the global bindless table and return its index
    uint32_t RegisterTexture(nvrhi::TextureHandle texture);

    // Get the global texture bindless descriptor table
    nvrhi::DescriptorTableHandle GetGlobalTextureDescriptorTable() const { return m_GlobalTextureDescriptorTable; }

    // Get the global texture bindless binding layout
    nvrhi::BindingLayoutHandle GetGlobalTextureBindingLayout() const { return m_GlobalTextureBindingLayout; }

    // ============================================================================
    // Public State & Resources
    // ============================================================================

    SDL_Window* m_Window = nullptr;
    GraphicRHI m_RHI{};
    nvrhi::DeviceHandle m_NvrhiDevice;

    // Swapchain textures
    nvrhi::TextureHandle m_SwapchainTextures[GraphicRHI::SwapchainImageCount] = {};
    uint32_t m_CurrentSwapchainImage = 0;

    // Depth buffer for main framebuffer
    nvrhi::TextureHandle m_DepthTexture;

    // Cached shader handles loaded from compiled SPIR-V binaries
    // (keyed by output stem, e.g., "imgui_VSMain")
    std::unordered_map<std::string, nvrhi::ShaderHandle> m_ShaderCache;

    // ImGui state
    ImGuiLayer m_ImGuiLayer;

    // Scene
    Scene m_Scene;

    // Camera
    Camera m_Camera;

    // Selected camera index for GLTF cameras
    int m_SelectedCameraIndex = -1;

    // Directional Light
    struct DirectionalLight
    {
        float yaw       = 0.0f;
        float pitch     = -0.5f;
        float intensity = 10000.0f;  // Default to 10,000 lux (bright daylight)
    } m_DirectionalLight;

    // Renderers
    std::vector<std::shared_ptr<IRenderer>> m_Renderers;

    // Performance metrics
    double m_FrameTime = 0.0;
    double m_FPS       = 0.0;

    // Main view pipeline statistics
    nvrhi::PipelineStatistics m_MainViewPipelineStatistics;

    // Frame counter for double buffering
    uint32_t m_FrameNumber = 0;

    // ============================================================================
    // Public Methods
    // ============================================================================

    // Return last frame time in milliseconds
    double GetFrameTimeMs() const { return m_FrameTime; }

    // Get directional light direction in world space
    Vector3 GetDirectionalLightDirection() const;

    // Set camera from a GLTF scene camera
    void SetCameraFromSceneCamera(const Scene::Camera& sceneCam);

    // ============================================================================
    // Internal State
    // ============================================================================

private:
    // Command list pools
    std::vector<nvrhi::CommandListHandle> m_CommandListFreeList;
    std::vector<nvrhi::CommandListHandle> m_PendingCommandLists;

    // Caches
    std::unordered_map<size_t, nvrhi::BindingLayoutHandle> m_BindingLayoutCache;
    std::unordered_map<size_t, nvrhi::GraphicsPipelineHandle> m_GraphicsPipelineCache;

    // Global bindless texture descriptor table
    nvrhi::DescriptorTableHandle m_GlobalTextureDescriptorTable;
    nvrhi::BindingLayoutHandle m_GlobalTextureBindingLayout;
    uint32_t m_NextTextureIndex = 0;

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
