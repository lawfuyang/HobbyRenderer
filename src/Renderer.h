#pragma once

#include "Camera.h"
#include "CameraStateManager.h"
#include "GraphicRHI.h"
#include "RenderGraph.h"
#include "Scene.h"
#include "srrhi.h"
#include "TaskScheduler.h"

#include "shaders/ShaderIDs.h"

// Streaming
#include "Streaming/FeedbackManager.h"
#include "Streaming/AsyncTileIO.h"

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

    // Set to true by ImGuiLayer when the indirect lighting technique switches TO this
    // renderer.  Each renderer is responsible for clearing its stale persistent
    // buffers/textures on the next Render() call and then resetting this flag.
    bool m_bClearOnNextRender = false;
};

class RendererRegistry
{
public:
    using Creator = std::function<std::shared_ptr<IRenderer>()>;

    static void RegisterRenderer(const char* name, Creator creator)
    {
        s_Creators.push_back({ name, creator });
    }

    static const std::vector<std::pair<const char*, Creator>>& GetCreators()
    {
        return s_Creators;
    }

    // Thread-safe after static init: renderers are created during InitializeGPUStack
    // and looked up during ScheduleAndRunAllRenderers (both on main thread).
    static IRenderer* GetRenderer(const char* name)
    {
        return s_Renderers.at(name); // Throws if not found
    }

    static void SetRenderer(const char* name, IRenderer* renderer)
    {
        SDL_assert(s_Renderers.contains(name) == false && "Renderer already registered");
        s_Renderers[name] = renderer;
    }

private:
    inline static std::vector<std::pair<const char*, Creator>> s_Creators;
    inline static std::unordered_map<std::string, IRenderer*>  s_Renderers;
};

// Macro to register a renderer class.
// No longer creates cross-TU global pointers — renderers are stored in
// RendererRegistry and looked up by name.  This eliminates incremental-linker
// crashes where extern pointer relocations would go stale after a partial rebuild.
#define REGISTER_RENDERER(ClassName) \
static bool s_##ClassName##Registered = []() { \
    RendererRegistry::RegisterRenderer(#ClassName, []() { \
        auto renderer = std::make_shared<ClassName>(); \
        SDL_assert(renderer && "Failed to initialize renderer: " #ClassName ); \
        RendererRegistry::SetRenderer(#ClassName, renderer.get()); \
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
    ReferencePathTracer = srrhi::CommonConsts::RENDERING_MODE_PATH_TRACER,
    NormalBasic = srrhi::CommonConsts::RENDERING_MODE_NORMAL_BASIC
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
    void Run();
    void Shutdown();
    void ScheduleAndRunAllRenderers();

    // Upload any dirty instance transforms to the GPU and reset the dirty range.
    // Must be called once per frame before ScheduleAndRunAllRenderers() so that
    // the TLAS rebuild sees up-to-date RT instance descriptors.  Called explicitly
    // by RenderFrame() (main loop).
    void UploadDirtyInstanceTransforms();

    // Upload material constants for any materials whose dirty range is set
    // (m_MaterialDirtyRange.first <= second) and reset the range to clean.
    // Handles the case where m_Materials is empty or m_MaterialConstantsBuffer
    // is null (no-op).  Called explicitly by RenderFrame() (main loop).
    void UploadDirtyMaterialConstants();

    // Computes m_CSMCascadeSplits and m_CSMCascades[i].m_SplitNear/Far for the current frame.
    // Called once per frame before ScheduleAndRunAllRenderers() so all renderers see up-to-date splits.
    void ComputeCSMCascadeSplits();
    void ComputeCascadeViewProj();

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

    // Shared GPU stack init used by Initialize().
    // Creates RHI device + swapchain against the given window, resolves asset
    // paths, initialises bindless heaps, loads shaders, and brings up
    // CommonResources.  Returns false on any fatal failure.
    bool InitializeGPUStack(SDL_Window* window);

    // Global Bindless Texture System
    void InitializeStaticBindlessTextures();
    uint32_t RegisterTexture(nvrhi::TextureHandle texture);
    bool RegisterTextureAtIndex(uint32_t index, nvrhi::TextureHandle texture);
    uint32_t RegisterSamplerFeedbackTexture(nvrhi::SamplerFeedbackTextureHandle texture);
    bool RegisterSamplerFeedbackTextureAtIndex(uint32_t index, nvrhi::SamplerFeedbackTextureHandle texture);
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

    // Camera state persistence (periodic save + restore on load)
    CameraStateManager m_CameraStateManager;

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
    RenderingMode m_Mode = RenderingMode::NormalBasic;

    // Rendering options
    bool m_UseMeshletRendering = true;
    int m_ForcedLOD = -1;
    int m_ForcedTextureMip = -1; // -1 = auto, 0-15 = forced mip level
    bool m_EnableAnimations = true;
    bool m_EnableRTShadows = true;
    uint32_t m_PathTracerMaxBounces = 8;

    float m_AdaptationSpeed = 5.0f;
    bool m_EnableAutoExposure = true;

    bool m_EnableBloom = false;
    bool m_DebugBloom = false;
    float m_BloomIntensity = 0.005f;

    // ReSTIR DI settings
    bool m_EnableReSTIRDI = true;
    bool m_EnableReSTIRDenoising = true;

    // Indirect lighting technique (mutually exclusive: 0=None, 1=RestirGI, 2=SHARC, 3=RestirGI+SHARC)
    uint32_t m_IndirectLightingTechnique = 3;

    // SHARC debug overlay (SHARCDebugMode enum value; 0 = off)
    uint32_t m_SHARCDebugMode = 0;

    // ── CSM cascade data — written by ShadowRenderer, read by ShadowMaskRenderer / CSMDebugRenderer ──
    struct CSMCascadeData
    {
        Matrix  m_ViewProj;   // Light-space view-proj (texel-snapped, reversed-Z ortho)
        Matrix  m_View;       // Light-space view matrix (for GPU culling)
        float   m_SplitNear;  // View-space near depth for this cascade
        float   m_SplitFar;   // View-space far depth for this cascade
        Vector3 m_LightAABBMin; // Light-view-space AABB min (for frustum planes)
        Vector3 m_LightAABBMax; // Light-view-space AABB max (for frustum planes)
    };
    CSMCascadeData m_CSMCascades[4];
    float          m_CSMCascadeSplits[5]; // [0..4] view-space split depths

    // ── CSM settings (NormalBasic mode) ──────────────────────────────────────
    bool     m_EnableCSMShadows    = true;    // Master toggle — disables all CSM passes when off
    uint32_t m_CSMDebugMode        = 0;       // CSMDebugMode enum value; 0 = off
    uint32_t m_NumCSMCascades      = 4;       // Fixed at 4 for now
    float    m_CSMCascadeLambda    = 0.75f;   // λ blend factor (log vs uniform splits)

    // Shadow bias — normal-offset only
    float    m_CSMNormalBias       = 3.0f;    // Normal-offset bias in shadow-map texels
    float    m_CSMCascadeBiasScale = 1.0f;    // Per-cascade bias scale (0=uniform, 1=proportional)

    bool     m_EnableCascadeBlend  = false;   // Blend adjacent cascades at boundaries
    bool     m_EnablePCSS          = false;   // Enable Percentage-Closer Soft Shadows (cascades 0-2)
    bool     m_EnablePCSSShadowTemporal = false;    // Enable temporal shadow history resolve (PCSS only)
    bool     m_EnablePCSSShadowDepthMips = false;   // Enable shadow map min-reduction mip chain (PCSS early-out)

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

    std::unique_ptr<nvfeedback::FeedbackManager> m_FeedbackManager;
    
    std::unique_ptr<nvfeedback::AsyncTileIO> m_AsyncTileIO; // Async tile I/O thread pool

    // Tiles submitted to AsyncTileIO this frame — their UpdateTileMappings and MinMip
    // update is deferred to the NEXT frame, after Flush() confirms the tile data has
    // been written to the GPU.
    std::vector<nvfeedback::FeedbackTextureUpdate> m_SubmittedTilesPendingMapping;

    // Tile requests that exceeded the per-frame budget (kMaxTilesPerFrame) and were
    // deferred to the next frame.  Drained before new requests each frame.
    std::vector<nvfeedback::FeedbackTextureUpdate> m_PendingTileRequests;

    // Count of tile indices actually submitted to AsyncTileIO this frame (for UI/debug).
    uint32_t m_TilesSubmittedThisFrame = 0;

    int m_TileResidencyDebugTextureIdx = -1; // -1 = disabled, 0..N = selected feedback texture index

    // Initialise the FeedbackManager after scene load.
    void InitStreaming();
    // Shutdown streaming resources.
    void ShutdownStreaming();
    // Pre-render streaming update: flush async uploads, BeginFrame, tile submit, UpdateTileMappings.
    // Call BEFORE ScheduleAndRunAllRenderers().
    void UpdateStreamingPreRender(nvrhi::CommandListHandle cmd);
    // Post-render streaming update: ResolveFeedback + EndFrame.
    // Call AFTER ScheduleAndRunAllRenderers() so the GBuffer pass has written sampler feedback.
    void UpdateStreamingPostRender();

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