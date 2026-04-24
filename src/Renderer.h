#pragma once

#include "AsyncMeshQueue.h"
#include "AsyncTextureQueue.h"
#include "Camera.h"
#include "GraphicRHI.h"
#include "RenderGraph.h"
#include "Scene.h"
#include "srrhi.h"
#include "TaskScheduler.h"

#include "shaders/ShaderIDs.h"

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

class ImGuiLayer
{
public:
    void Initialize();
    void Shutdown();
    void ProcessEvent(const SDL_Event& event);
    void UpdateFrame();
};

enum class RenderingMode : uint32_t
{
    Normal = srrhi::CommonConsts::RENDERING_MODE_NORMAL,
    IBL = srrhi::CommonConsts::RENDERING_MODE_IBL,
    ReferencePathTracer = srrhi::CommonConsts::RENDERING_MODE_PATH_TRACER
};

struct Renderer
{
    SingletonFunctionsSimple(Renderer);
    
    static constexpr float DEPTH_NEAR = 1.0f;
    static constexpr float DEPTH_FAR = 0.0f;
    static constexpr nvrhi::Format DEPTH_FORMAT = nvrhi::Format::D24S8;
    static constexpr nvrhi::Format HDR_COLOR_FORMAT = nvrhi::Format::R11G11B10_FLOAT;
    static constexpr nvrhi::Format PATH_TRACER_HDR_COLOR_FORMAT = nvrhi::Format::RGBA32_FLOAT;
    static constexpr nvrhi::Format GBUFFER_ALBEDO_FORMAT    = nvrhi::Format::RGBA8_UNORM;
    static constexpr nvrhi::Format GBUFFER_NORMALS_FORMAT   = nvrhi::Format::RG16_FLOAT;
    static constexpr nvrhi::Format GBUFFER_ORM_FORMAT       = nvrhi::Format::RG8_UNORM;
    static constexpr nvrhi::Format GBUFFER_EMISSIVE_FORMAT  = nvrhi::Format::RGBA16_FLOAT;
    static constexpr nvrhi::Format GBUFFER_MOTION_FORMAT    = nvrhi::Format::RGBA16_FLOAT;

    // Lifecycle
    void Initialize();
    void InitializeForTests(); // Headless init for --run-tests: RHI + CommonResources, no scene/renderers
    void Run();
    void Shutdown();
    void ScheduleAndRunAllRenderers();

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
    struct ComputeDispatchParams
    {
        uint32_t x = 0, y = 0, z = 0; // For direct dispatch
        nvrhi::BufferHandle indirectBuffer = nullptr; // For indirect dispatch (if not null, dispatch is indirect)
        uint32_t indirectOffsetBytes = 0; // For indirect dispatch
    };

    struct RenderPassParams
    {
        nvrhi::CommandListHandle commandList;
        uint32_t shaderID = UINT32_MAX;
        nvrhi::BindingSetDesc bindingSetDesc;
        uint32_t registerSpace = 0;

        struct BindingSetDescAndRegisterSpace
        {
            nvrhi::BindingSetDesc bindingSetDesc;
            uint32_t registerSpace = 0;
        };
        std::vector<BindingSetDescAndRegisterSpace> additionalBindingSets;

        const void* pushConstants = nullptr;
        size_t pushConstantsSize = 0;
        bool bIncludeBindlessResources = true;
        // Compute specific
        ComputeDispatchParams dispatchParams;
        // Graphics specific
        nvrhi::FramebufferHandle framebuffer;
        nvrhi::DepthStencilState* depthStencilState = nullptr;
        nvrhi::BlendState::RenderTarget* blendState = nullptr;
    };

    void AddComputePass(const RenderPassParams& params);
    void AddFullScreenPass(const RenderPassParams& params);
    void GenerateMipsUsingSPD(nvrhi::TextureHandle texture, nvrhi::BufferHandle spdAtomicCounter, nvrhi::CommandListHandle commandList, const char* markerName, uint32_t reductionType);
    nvrhi::ShaderHandle GetShaderHandle(uint32_t shaderID) const;

    // Shared GPU stack init used by both Initialize() and InitializeForTests().
    // Creates RHI device + swapchain against the given window, resolves asset
    // paths, initialises bindless heaps, loads shaders, and brings up
    // CommonResources.  Returns false on any fatal failure.
    bool InitializeGPUStack(SDL_Window* window);

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

    // Returns the swapchain extent (width, height) as a pair.
    std::pair<uint32_t, uint32_t> SwapchainSize();

    static MicroProfileThreadLogGpu*& GetGPULogForCurrentThread();

    static nvrhi::BindingSetDesc CreateBindingSetDesc(std::span<const srrhi::ResourceEntry> resources, uint32_t pushConstantBytes = 0);

    template<typename SrInput>
    static nvrhi::BindingSetDesc CreateBindingSetDesc(const SrInput& inputs) { return CreateBindingSetDesc(inputs.m_Resources, SrInput::PushConstantBytes); }

    // Public State & Resources
    SDL_Window* m_Window = nullptr;
    std::unique_ptr<GraphicRHI> m_RHI;

    uint32_t m_AcquiredSwapchainImageIdx = 0;
    uint32_t m_SwapChainImageIdx = 0;

    // Render Graph
    RenderGraph m_RenderGraph;

    // Shader handles — indexed by ShaderID:: constants, populated by LoadShaders()
    nvrhi::ShaderHandle m_ShaderHandles[ShaderID::COUNT]{};

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
    float m_BloomIntensity = 0.005f;

    // ReSTIR DI settings
    bool m_EnableReSTIRDI = true;
    bool m_EnableReSTIRDIRelaxDenoising = true;

    // bloom
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

    bool m_bTAAEnabled = true;
    bool m_bTAADebugView = false;
    float m_TAASharpness = 0.0f;
    float m_PrevFrameExposure = 1.0f; // Previous frame's exposure multiplier (readback from GPU)

    // Environment Lighting settings
    bool m_EnableSky = true;
    std::string m_IrradianceTexturePath = "irradiance.dds";
    std::string m_RadianceTexturePath = "radiance.dds";
    std::string m_BRDFLutTexture = "brdf_lut.dds";

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
    uint32_t m_NextTextureIndex = srrhi::CommonConsts::DEFAULT_TEXTURE_COUNT;

    // Global sampler descriptor heap
    nvrhi::DescriptorTableHandle m_StaticSamplerDescriptorTable;
    nvrhi::BindingLayoutHandle m_StaticSamplerBindingLayout;

    // GPU Timing
    nvrhi::TimerQueryHandle m_GPUQueries[2];

    // Background loading queues for async streaming of textures and meshes.
    AsyncTextureQueue m_AsyncTextureQueue;
    AsyncMeshQueue    m_AsyncMeshQueue;

    // Private methods
    void HashPipelineCommonState(size_t& h, const nvrhi::RenderState&, const nvrhi::FramebufferInfoEx&, const nvrhi::BindingLayoutVector&);
    void LoadShaders();
    void UnloadShaders();
    void ReloadShaders();

    bool m_Running = true;
    bool m_RequestedShaderReload = false;
};
#define g_Renderer Renderer::GetInstance()

class ScopedCommandList
{
public:
    ScopedCommandList(const nvrhi::CommandListHandle& commandList, std::string_view markerName = "");
    ~ScopedCommandList();

    const nvrhi::CommandListHandle& operator->() const { return m_CommandList; }
    operator nvrhi::ICommandList*() const { return m_CommandList.Get(); }
    operator const nvrhi::CommandListHandle& () const { return m_CommandList; }

private:
    const nvrhi::CommandListHandle& m_CommandList;
    std::string m_MarkerName;
    bool m_HasMarker;
};

class ScopedGpuProfile
{
public:
    ScopedGpuProfile(std::string_view name, const nvrhi::CommandListHandle& commandList);

private:
    static std::string_view BuildMicroProfileName(std::string_view name);

    nvrhi::utils::ScopedMarker m_Marker;
    MicroProfileToken m_Token = MICROPROFILE_INVALID_TOKEN;
    MicroProfileScopeGpuHandler m_Scope;
};

#define PROFILE_GPU_SCOPED(NAME, CMDLIST) ScopedGpuProfile GENERATE_UNIQUE_VARIABLE(scopedGpuProfile){ NAME, CMDLIST };