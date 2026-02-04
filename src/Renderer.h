#pragma once

#include "pch.h"
#include "GraphicRHI.h"
#include "ImGuiLayer.h"
#include "Scene.h"
#include "Camera.h"

#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <memory>
#include "TaskScheduler.h"

class IRenderer
{
public:
    virtual ~IRenderer() = default;
    virtual void Initialize() = 0;
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
    // Constants
    static constexpr uint32_t SPIRV_SAMPLER_SHIFT   = 100;  // sRegShift (s#)
    static constexpr uint32_t SPIRV_TEXTURE_SHIFT   = 200;  // tRegShift (t#)
    static constexpr uint32_t SPIRV_CBUFFER_SHIFT   = 300;  // bRegShift (b#)
    static constexpr uint32_t SPIRV_UAV_SHIFT       = 400;  // uRegShift (u#)

    static constexpr float DEPTH_NEAR = 1.0f;
    static constexpr float DEPTH_FAR = 0.0f;
    static constexpr nvrhi::Format HDR_COLOR_FORMAT = nvrhi::Format::R11G11B10_FLOAT;
    inline static const nvrhi::Color kHDROutputClearColor = nvrhi::Color{ 1.0f };

    // Instance Management
    static void SetInstance(Renderer* instance);
    static Renderer* GetInstance();

    // Lifecycle
    void Initialize();
    void Run();
    void Shutdown();

    // Command List Management
    nvrhi::CommandListHandle AcquireCommandList(std::string_view markerName);
    void SubmitCommandList(const nvrhi::CommandListHandle& commandList);
    void ExecutePendingCommandLists();

    // Swapchain / Backbuffer
    nvrhi::TextureHandle GetCurrentBackBufferTexture() const;

    // Binding Layouts & Pipelines
    nvrhi::BindingLayoutHandle GetOrCreateBindingLayoutFromBindingSetDesc(const nvrhi::BindingSetDesc& setDesc, uint32_t registerSpace = 0);
    nvrhi::BindingLayoutHandle GetOrCreateBindlessLayout(const nvrhi::BindlessLayoutDesc& desc);
    nvrhi::GraphicsPipelineHandle GetOrCreateGraphicsPipeline(const nvrhi::GraphicsPipelineDesc& pipelineDesc, const nvrhi::FramebufferInfoEx& fbInfo);
    nvrhi::MeshletPipelineHandle GetOrCreateMeshletPipeline(const nvrhi::MeshletPipelineDesc& pipelineDesc, const nvrhi::FramebufferInfoEx& fbInfo);
    nvrhi::ComputePipelineHandle GetOrCreateComputePipeline(nvrhi::ShaderHandle shader, nvrhi::BindingLayoutHandle bindingLayout);

    // Rendering Helpers
    void DrawFullScreenPass(
        nvrhi::CommandListHandle commandList,
        const nvrhi::FramebufferHandle& framebuffer,
        std::string_view shaderName,
        const nvrhi::BindingSetDesc& bindingSetDesc,
        const void* pushConstants = nullptr,
        size_t pushConstantsSize = 0);

    struct ComputeDispatchParams
    {
        uint32_t x = 0, y = 0, z = 0; // For direct dispatch
        nvrhi::BufferHandle indirectBuffer = nullptr; // For indirect dispatch (if not null, dispatch is indirect)
        uint32_t indirectOffsetBytes = 0; // For indirect dispatch
    };

    struct RenderPassParams
    {
        nvrhi::CommandListHandle commandList;
        std::string_view shaderName;
        nvrhi::BindingSetDesc bindingSetDesc;
        const void* pushConstants = nullptr;
        size_t pushConstantsSize = 0;
        // Compute specific
        ComputeDispatchParams dispatchParams;
        // Graphics specific
        nvrhi::FramebufferHandle framebuffer;
    };

    void AddComputePass(const RenderPassParams& params);
    void AddFullScreenPass(const RenderPassParams& params);
    nvrhi::ShaderHandle GetShaderHandle(std::string_view name) const;

    // Global Bindless Texture System
    void InitializeGlobalBindlessTextures();
    uint32_t RegisterTexture(nvrhi::TextureHandle texture);
    nvrhi::DescriptorTableHandle GetGlobalTextureDescriptorTable() const { return m_GlobalTextureDescriptorTable; }
    nvrhi::BindingLayoutHandle GetGlobalTextureBindingLayout() const { return m_GlobalTextureBindingLayout; }

    // Public Methods
    double GetFrameTimeMs() const { return m_FrameTime; }
    void SetCameraFromSceneCamera(const Scene::Camera& sceneCam);

    // Public State & Resources
    SDL_Window* m_Window = nullptr;
    std::unique_ptr<GraphicRHI> m_RHI;

    uint32_t m_CurrentSwapchainImageIdx = 0;

    // Depth buffer
    nvrhi::TextureHandle m_DepthTexture;

    // HDR resources
    nvrhi::TextureHandle m_HDRColorTexture;
    nvrhi::BufferHandle m_LuminanceHistogram;
    nvrhi::BufferHandle m_ExposureBuffer;

    // Shader cache
    std::unordered_map<std::string, nvrhi::ShaderHandle> m_ShaderCache;

    // UI
    ImGuiLayer m_ImGuiLayer;

    // Parallel processing
    std::unique_ptr<TaskScheduler> m_TaskScheduler;

    // Scene and Camera
    Scene m_Scene;
    Camera m_Camera;
    int m_SelectedCameraIndex = -1;

    // Renderers
    std::vector<std::shared_ptr<IRenderer>> m_Renderers;

    // Performance metrics
    double m_FrameTime = 0.0;
    double m_GPUTime   = 0.0;
    double m_FPS       = 0.0;
    uint32_t m_TargetFPS = 200;
    nvrhi::PipelineStatistics m_MainViewPipelineStatistics;
    uint32_t m_FrameNumber = 0;

    // Culling options
    bool m_EnableFrustumCulling = true;
    bool m_EnableConeCulling = true;
    bool m_FreezeCullingCamera = false;
    Matrix m_FrozenCullingViewMatrix;
    Vector3 m_FrozenCullingCameraPos;
    bool m_EnableOcclusionCulling = true;
    nvrhi::TextureHandle m_HZBTexture;
    nvrhi::BufferHandle m_SPDAtomicCounter;

    // Rendering options
    bool m_UseMeshletRendering = true;
    int m_ForcedLOD = -1;
    bool m_EnableAnimations = true;
    bool m_EnableRTShadows = true;
    int m_DebugMode = 0;

private:
    // Internal State
    std::vector<nvrhi::CommandListHandle> m_CommandListFreeList;
    std::vector<nvrhi::CommandListHandle> m_PendingCommandLists;
    std::vector<nvrhi::CommandListHandle> m_InFlightCommandLists;

    // Caches
    std::unordered_map<size_t, nvrhi::BindingLayoutHandle> m_BindingLayoutCache;
    std::unordered_map<size_t, nvrhi::GraphicsPipelineHandle> m_GraphicsPipelineCache;
    std::unordered_map<size_t, nvrhi::MeshletPipelineHandle> m_MeshletPipelineCache;
    std::unordered_map<size_t, nvrhi::ComputePipelineHandle> m_ComputePipelineCache;

    // Global bindless texture system
    nvrhi::DescriptorTableHandle m_GlobalTextureDescriptorTable;
    nvrhi::BindingLayoutHandle m_GlobalTextureBindingLayout;
    uint32_t m_NextTextureIndex = 0;

    // GPU Timing
    nvrhi::TimerQueryHandle m_GPUQueries[2];

    // Private methods
    void CreateDepthTextures();
    void DestroyDepthTextures();
    void CreateHDRResources();
    void DestroyHDRResources();
    void HashPipelineCommonState(size_t& h, 
                                 const nvrhi::RenderState& renderState, 
                                 const nvrhi::FramebufferInfoEx& fbInfo, 
                                 const nvrhi::BindingLayoutVector& bindingLayouts);
    void LoadShaders();
    void UnloadShaders();

    static Renderer* s_Instance;
    bool m_Running = true;
};

class ScopedCommandList
{
public:
    ScopedCommandList(std::string_view markerName = "ScopedCommandList")
        : m_CommandList(Renderer::GetInstance()->AcquireCommandList(markerName))
    {
    }

    ~ScopedCommandList()
    {
        Renderer::GetInstance()->SubmitCommandList(m_CommandList);
    }

    nvrhi::CommandListHandle& operator->() { return m_CommandList; }
    operator nvrhi::ICommandList*() { return m_CommandList.Get(); }
    operator nvrhi::CommandListHandle& () { return m_CommandList; }

private:
    nvrhi::CommandListHandle m_CommandList;
};
