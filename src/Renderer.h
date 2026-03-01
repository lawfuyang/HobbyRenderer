#pragma once

#include "pch.h"
#include "GraphicRHI.h"
#include "ImGuiLayer.h"
#include "Scene.h"
#include "Camera.h"
#include "RenderGraph.h"
#include "TaskScheduler.h"

class IRenderer
{
public:
    virtual ~IRenderer() = default;
    virtual void Initialize() {}
    virtual void PostSceneLoad() {}
    virtual bool Setup(RenderGraph& renderGraph) { return false; }
    virtual void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) {}
    virtual const char* GetName() const { return "Unnamed Renderer"; }

    virtual bool IsBasePassRenderer() const { return false; }

    float m_CPUTime = 0.0f;
    float m_GPUTime = 0.0f;
    nvrhi::TimerQueryHandle m_GPUQueries[2];
    bool m_bPassEnabled = false;
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

enum class RenderingMode : uint32_t
{
    Normal = RENDERING_MODE_NORMAL,
    IBL = RENDERING_MODE_IBL,
    ReferencePathTracer = RENDERING_MODE_PATH_TRACER
};

struct Renderer
{
    // Constants
    static constexpr uint32_t SPIRV_SAMPLER_SHIFT   = 100;  // sRegShift (s#)
    static constexpr uint32_t SPIRV_TEXTURE_SHIFT   = 200;  // tRegShift (t#)
    static constexpr uint32_t SPIRV_CBUFFER_SHIFT   = 300;  // bRegShift (b#)
    static constexpr uint32_t SPIRV_UAV_SHIFT       = 400;  // uRegShift (u#)

    static constexpr float DEPTH_NEAR = 1.0f;
    static constexpr float DEPTH_FAR = 0.0f;
    static constexpr nvrhi::Format DEPTH_FORMAT = nvrhi::Format::D24S8;
    static constexpr nvrhi::Format HDR_COLOR_FORMAT = nvrhi::Format::R11G11B10_FLOAT;
    static constexpr nvrhi::Format PATH_TRACER_HDR_COLOR_FORMAT = nvrhi::Format::RGBA32_FLOAT;
    static constexpr nvrhi::Format GBUFFER_ALBEDO_FORMAT    = nvrhi::Format::RGBA8_UNORM;
    static constexpr nvrhi::Format GBUFFER_NORMALS_FORMAT   = nvrhi::Format::RG16_FLOAT;
    static constexpr nvrhi::Format GBUFFER_ORM_FORMAT       = nvrhi::Format::RG8_UNORM;
    static constexpr nvrhi::Format GBUFFER_EMISSIVE_FORMAT  = nvrhi::Format::RGBA8_UNORM;
    static constexpr nvrhi::Format GBUFFER_MOTION_FORMAT    = nvrhi::Format::RGBA16_FLOAT;

    // Instance Management
    static void SetInstance(Renderer* instance);
    static Renderer* GetInstance();

    // Lifecycle
    void Initialize();
    void Run();
    void Shutdown();

    // Command List Management
    nvrhi::CommandListHandle AcquireCommandList(bool bImmediatelyQueue = true);
    void ExecutePendingCommandLists();

    // Swapchain / Backbuffer
    nvrhi::TextureHandle GetCurrentBackBufferTexture() const;
    void SaveBackBufferScreenshot();

    // Binding Layouts & Pipelines
    nvrhi::BindingLayoutHandle GetOrCreateBindingLayoutFromBindingSetDesc(const nvrhi::BindingSetDesc& setDesc, uint32_t registerSpace = 0);
    nvrhi::BindingLayoutHandle GetOrCreateBindlessLayout(const nvrhi::BindlessLayoutDesc& desc);
    nvrhi::GraphicsPipelineHandle GetOrCreateGraphicsPipeline(const nvrhi::GraphicsPipelineDesc& pipelineDesc, const nvrhi::FramebufferInfoEx& fbInfo);
    nvrhi::MeshletPipelineHandle GetOrCreateMeshletPipeline(const nvrhi::MeshletPipelineDesc& pipelineDesc, const nvrhi::FramebufferInfoEx& fbInfo);
    nvrhi::ComputePipelineHandle GetOrCreateComputePipeline(nvrhi::ShaderHandle shader, const nvrhi::BindingLayoutVector& bindingLayouts);

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
        nvrhi::DepthStencilState* depthStencilState = nullptr;
    };

    void AddComputePass(const RenderPassParams& params);
    void AddFullScreenPass(const RenderPassParams& params);
    void GenerateMipsUsingSPD(nvrhi::TextureHandle texture, nvrhi::BufferHandle spdAtomicCounter, nvrhi::CommandListHandle commandList, const char* markerName, SpdReductionType reductionType);
    nvrhi::ShaderHandle GetShaderHandle(std::string_view name) const;

    // Global Bindless Texture System
    void InitializeStaticBindlessTextures();
    uint32_t RegisterTexture(nvrhi::TextureHandle texture);
    bool RegisterTextureAtIndex(uint32_t index, nvrhi::TextureHandle texture);
    nvrhi::DescriptorTableHandle GetStaticTextureDescriptorTable() const { return m_StaticTextureDescriptorTable; }
    nvrhi::BindingLayoutHandle GetStaticTextureBindingLayout() const { return m_StaticTextureBindingLayout; }

    // Global Sampler Descriptor Heap
    void InitializeStaticBindlessSamplers();
    bool RegisterSamplerAtIndex(uint32_t index, nvrhi::SamplerHandle sampler);
    nvrhi::DescriptorTableHandle GetStaticSamplerDescriptorTable() const { return m_StaticSamplerDescriptorTable; }
    nvrhi::BindingLayoutHandle GetStaticSamplerBindingLayout() const { return m_StaticSamplerBindingLayout; }

    // Public Methods
    double GetFrameTimeMs() const { return m_FrameTime; }
    void SetCameraFromSceneCamera(const Scene::Camera& sceneCam);

    // Public State & Resources
    SDL_Window* m_Window = nullptr;
    std::unique_ptr<GraphicRHI> m_RHI;

    uint32_t m_AcquiredSwapchainImageIdx = 0;
    uint32_t m_SwapChainImageIdx = 0;

    // Render Graph
    RenderGraph m_RenderGraph;

    // Shader cache
    std::unordered_map<std::string, nvrhi::ShaderHandle> m_ShaderCache;

    // UI
    ImGuiLayer m_ImGuiLayer;

    // Parallel processing
    std::unique_ptr<TaskScheduler> m_TaskScheduler;

    // Scene
    Scene m_Scene;

    // Renderers
    std::vector<std::shared_ptr<IRenderer>> m_Renderers;

    // Performance metrics
    double m_FrameTime = 0.0;
    double m_FPS       = 0.0;
    uint32_t m_TargetFPS = 200;
    nvrhi::PipelineStatistics m_SelectedBasePassPipelineStatistics;
    int m_SelectedRendererIndexForPipelineStatistics = -1;
    uint32_t m_FrameNumber = 0;

    int m_SelectedNodeIndex = -1;

    // Culling options
    bool m_EnableFrustumCulling = true;
    bool m_EnableConeCulling = false;
    bool m_FreezeCullingCamera = false;
    bool m_EnableOcclusionCulling = true;

    // Rendering mode
    RenderingMode m_Mode = RenderingMode::Normal;

    // Rendering options
    bool m_UseMeshletRendering = true;
    int m_ForcedLOD = -1;
    bool m_EnableAnimations = true;
    bool m_EnableRTShadows = true;
    uint32_t m_PathTracerMaxBounces = 8;

    float m_AdaptationSpeed = 5.0f;
    bool m_EnableAutoExposure = true;

    bool m_EnableBloom = true;
    bool m_DebugBloom = false;
    float m_BloomIntensity = 0.1f;

    // ReSTIR DI settings
    bool m_EnableReSTIRDI = false;
    bool m_ReSTIRDI_EnableTemporal = true;
    bool m_ReSTIRDI_EnableSpatial = true;
    int  m_ReSTIRDI_SpatialSamples = 1;
    bool m_ReSTIRDI_EnableCheckerboard = true;
    float m_BloomKnee = 0.1f;
    float m_UpsampleRadius = 0.85f;

    int m_DebugMode = 0;
    int m_ActiveDebugMode = 0;
    struct {
        bool m_EnableBloom;
        bool m_EnableAutoExposure;
        float m_ExposureValue;
        float m_ExposureCompensation;
    } m_DebugBackup{};

    // Environment Lighting settings
    bool m_EnableSky = true;
    std::string m_IrradianceTexturePath = "irradiance.dds";
    std::string m_RadianceTexturePath = "radiance.dds";
    std::string m_BRDFLutTexture = "brdf_lut.dds";

    nvrhi::TextureHandle m_RadianceTexture;
    nvrhi::TextureHandle m_IrradianceTexture;

private:
    // Internal State
    std::vector<nvrhi::CommandListHandle> m_CommandListFreeList;
    std::vector<nvrhi::CommandListHandle> m_PendingCommandLists;
    std::vector<nvrhi::CommandListHandle> m_InFlightCommandLists;

    // Caches
    std::mutex m_CacheMutex;
    std::unordered_map<size_t, nvrhi::BindingLayoutHandle> m_BindingLayoutCache;
    std::unordered_map<size_t, nvrhi::GraphicsPipelineHandle> m_GraphicsPipelineCache;
    std::unordered_map<size_t, nvrhi::MeshletPipelineHandle> m_MeshletPipelineCache;
    std::unordered_map<size_t, nvrhi::ComputePipelineHandle> m_ComputePipelineCache;

    // Global bindless texture system
    nvrhi::DescriptorTableHandle m_StaticTextureDescriptorTable;
    nvrhi::BindingLayoutHandle m_StaticTextureBindingLayout;
    uint32_t m_NextTextureIndex = DEFAULT_TEXTURE_COUNT;

    // Global sampler descriptor heap
    nvrhi::DescriptorTableHandle m_StaticSamplerDescriptorTable;
    nvrhi::BindingLayoutHandle m_StaticSamplerBindingLayout;

    // GPU Timing
    nvrhi::TimerQueryHandle m_GPUQueries[2];

    // Private methods
    void HashPipelineCommonState(size_t& h, const nvrhi::RenderState&, const nvrhi::FramebufferInfoEx&, const nvrhi::BindingLayoutVector&);
    void LoadShaders();
    void UnloadShaders();
    void ReloadShaders();

    static Renderer* s_Instance;
    bool m_Running = true;
    bool m_RequestedShaderReload = false;
};

class ScopedCommandList
{
public:
    ScopedCommandList(const nvrhi::CommandListHandle& commandList, std::string_view markerName = "")
        : m_CommandList(commandList)
        , m_HasMarker(!markerName.empty())
    {
        m_CommandList->open();

        if (m_HasMarker)
        {
            Renderer::GetInstance()->m_RHI->SetCommandListDebugName(commandList, markerName);
        }

        if (m_HasMarker)
        {
            m_CommandList->beginMarker(markerName.data());
        }
    }

    ~ScopedCommandList()
    {
        if (m_HasMarker)
        {
            m_CommandList->endMarker();
        }
        m_CommandList->close();
    }

    const nvrhi::CommandListHandle& operator->() const { return m_CommandList; }
    operator nvrhi::ICommandList*() const { return m_CommandList.Get(); }
    operator const nvrhi::CommandListHandle& () const { return m_CommandList; }

private:
    const nvrhi::CommandListHandle& m_CommandList;
    bool m_HasMarker;
};
