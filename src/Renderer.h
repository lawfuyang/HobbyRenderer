#pragma once

#include "Camera.h"
#include "CameraStateManager.h"
#include "CommonResources.h"
#include "GpuPassTimers.h"
#include "GraphicRHI.h"
#include "RenderGraph.h"
#include "Scene.h"
#include "srrhi.h"
#include "TaskScheduler.h"

#include "shaders/ShaderIDs.h"
#include "shaders/srrhi/cpp/Cluster.h"

// Streaming
#include "Streaming/FeedbackManager.h"
#include "Streaming/AsyncTileIO.h"

// ============================================================================
// Renderer interface & registry
// ============================================================================

// Base interface for a render-graph pass.
//
// Renderers are discovered through RendererRegistry (see REGISTER_RENDERER),
// owned by Renderer::m_Renderers, and driven by
// Renderer::ScheduleAndRunAllRenderers().
class IRenderer
{
public:
    virtual ~IRenderer() = default;

    // ─── Lifecycle ───
    virtual void Initialize() {}
    virtual void PostSceneLoad() {}

    // ─── Render graph integration ───
    virtual bool Setup(RenderGraph& renderGraph) { return false; }
    virtual void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) {}
    virtual const char* GetName() const { return "Unnamed Renderer"; }

    virtual bool IsBasePassRenderer() const { return false; }

    // ─── Per-frame statistics (filled in by the render loop) ───
    // GPU time of this renderer's own scope comes from GpuPassTimers, which the Profiler panel reads.
    float m_CPUTime = 0.0f;
    bool m_bPassEnabled = false;

    // Set to true by ImGuiLayer when the indirect lighting technique switches TO this
    // renderer.  Each renderer is responsible for clearing its stale persistent
    // buffers/textures on the next Render() call and then resetting this flag.
    bool m_bClearOnNextRender = false;
};

// Global registry of renderer factories and live instances.
// Factories are added at static-init time by REGISTER_RENDERER; instances are
// created during Renderer::InitializeGPUStack() and looked up by name when
// the render graph is scheduled.
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

// ============================================================================
// Renderer registration
// ============================================================================

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

// ============================================================================
// Editor / debug UI
// ============================================================================

class ImGuiLayer
{
public:
    void Initialize();
    void Shutdown();
    void ProcessEvent(const SDL_Event& event);
    void UpdateFrame();
};

// ============================================================================
// Rendering mode
// ============================================================================

// Top-level rendering mode. The values mirror srrhi::CommonConsts::
// RENDERING_MODE_* so they can be forwarded to shaders verbatim.
enum class RenderingMode : uint32_t
{
    Normal = srrhi::CommonConsts::RENDERING_MODE_NORMAL,
    ReferencePathTracer = srrhi::CommonConsts::RENDERING_MODE_PATH_TRACER
};

// ============================================================================
// Renderer
// ============================================================================

// Owns the RHI device, swapchain, scene, render graph and every render pass.
//
// State is deliberately public: the main loop, the individual renderers and the
// ImGui layer read and write it directly. Reach the singleton via `g_Renderer`.
struct Renderer
{
public:
    SingletonFunctionsSimple(Renderer);

    // ─── Nested types ───

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
        // Restricts the pass to a sub-rectangle of the framebuffer. Null covers the whole attachment, which is
        // what every pass but the debug overlays wants.
        const nvrhi::Viewport* viewport = nullptr;
    };

    // ─── Constants ───

    static constexpr float DEPTH_FAR = 0.0f;
    static constexpr nvrhi::Format DEPTH_FORMAT = nvrhi::Format::D24S8;
    static constexpr nvrhi::Format HDR_COLOR_FORMAT = nvrhi::Format::R11G11B10_FLOAT;
    static constexpr nvrhi::Format PATH_TRACER_HDR_COLOR_FORMAT = nvrhi::Format::RGBA32_FLOAT;
    static constexpr nvrhi::Format GBUFFER_ALBEDO_FORMAT    = nvrhi::Format::RGBA8_UNORM;
    static constexpr nvrhi::Format GBUFFER_NORMALS_FORMAT   = nvrhi::Format::RG16_FLOAT;
    static constexpr nvrhi::Format GBUFFER_ORM_FORMAT       = nvrhi::Format::RG8_UNORM;
    static constexpr nvrhi::Format GBUFFER_EMISSIVE_FORMAT  = nvrhi::Format::RGBA16_FLOAT;
    static constexpr nvrhi::Format GBUFFER_MOTION_FORMAT    = nvrhi::Format::RGBA16_FLOAT;

    // ─── Lifecycle ───
    void Initialize();
    void Run();
    void Shutdown();
    void ScheduleAndRunAllRenderers();

    // ─── Per-frame uploads (called by the main loop) ───

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

    // ─── Mode & debug settings ───

    void HandleDebugModeSettings();

    // Apply per-mode defaults for features that vary by rendering mode
    // (RT shadows, ReSTIR DI, indirect lighting technique).
    void ApplyRenderingModeDefaults(RenderingMode mode);

    // ─── Command list management ───
    nvrhi::CommandListHandle AcquireCommandList(bool bImmediatelyQueue = true);
    void ExecutePendingCommandLists();

    // Signal this event query once the command lists submitted by the current frame have executed, so
    // a consumer can poll it instead of calling waitForIdle() and stalling the CPU.
    void SignalEventAfterFrame(nvrhi::EventQueryHandle event) { m_EventsToSignal.push_back(std::move(event)); }

    // ─── Cluster traversal counters ───
    // --cluster-stats-frame hands one frame's counters to LogClusterCounters(), which stalls on purpose so the
    // values can be compared with the CPU cut. The panel path (the Cluster LOD section, or --cluster-debug 4)
    // instead copies every frame into a double-buffered staging buffer and reads it one frame later, so it never
    // stalls the frame it measures. One slot per bucket: 0 = opaque, 1 = masked, because both traverse.
    void RequestClusterCountersReadback(nvrhi::BufferHandle counters, nvrhi::CommandListHandle commandList,
                                        uint32_t bucketIndex, const char* bucketName, float focalPixels);
    void LogClusterCounters();

    void CopyClusterCountersForPanel(nvrhi::BufferHandle counters, nvrhi::CommandListHandle commandList, uint32_t bucketIndex);
    void PollClusterCounters();
    const srrhi::ClusterCounters& GetClusterCounters(uint32_t bucketIndex) const;
    bool ClusterCountersValid(uint32_t bucketIndex) const;

    // True only while the counters are actually on screen: the Cluster LOD section open and its debug view set
    // to "Traversal counters" (mode 4). Nothing is copied otherwise, so an idle panel costs nothing. Modes 1-3 of
    // that switch visualise clusters being drawn, which needs phase 2's mesh shader.
    bool IsClusterCountersLive() const { return m_bClusterCountersPanelOpen && m_ClusterDebugMode == 4; }
    void SetClusterCountersPanelOpen(bool bOpen) { m_bClusterCountersPanelOpen = bOpen; }

    // ─── Swapchain / backbuffer ───
    nvrhi::TextureHandle GetCurrentBackBufferTexture() const;
    void SaveBackBufferScreenshot();

    // ─── Binding sets & pipelines (cached) ───
    nvrhi::BindingLayoutHandle GetOrCreateBindingLayoutFromBindingSetDesc(const nvrhi::BindingSetDesc& setDesc, uint32_t registerSpace = 0);
    nvrhi::BindingLayoutHandle GetOrCreateBindlessLayout(const nvrhi::BindlessLayoutDesc& desc);
    nvrhi::GraphicsPipelineHandle GetOrCreateGraphicsPipeline(const nvrhi::GraphicsPipelineDesc& pipelineDesc, const nvrhi::FramebufferInfoEx& fbInfo);
    nvrhi::MeshletPipelineHandle GetOrCreateMeshletPipeline(const nvrhi::MeshletPipelineDesc& pipelineDesc, const nvrhi::FramebufferInfoEx& fbInfo);
    nvrhi::ComputePipelineHandle GetOrCreateComputePipeline(nvrhi::ShaderHandle shader, const nvrhi::BindingLayoutVector& bindingLayouts);

    // ─── Pass recording helpers ───

    void AddComputePass(const RenderPassParams& params);
    void AddFullScreenPass(const RenderPassParams& params);
    void GenerateMipsUsingSPD(nvrhi::TextureHandle texture, nvrhi::BufferHandle spdAtomicCounter, nvrhi::CommandListHandle commandList, const char* markerName, uint32_t reductionType);
    nvrhi::ShaderHandle GetShaderHandle(uint32_t shaderID) const;

    // ─── GPU stack initialisation ───

    // Used by Initialize(). Creates the RHI device + swapchain against the given
    // window, initialises the bindless heaps, loads shaders, and brings up
    // CommonResources. Returns false on any fatal failure.
    bool InitializeGPUStack(SDL_Window* window);

    // ─── Global bindless texture heap ───

    void InitializeStaticBindlessTextures();

    uint32_t RegisterTexture(nvrhi::TextureHandle texture) { return WriteBindlessItem(nvrhi::BindingSetItem::Texture_SRV(0, texture), "Texture_SRV"); }
    uint32_t RegisterTextureUAV(nvrhi::TextureHandle texture) { return WriteBindlessItem(nvrhi::BindingSetItem::Texture_UAV(0, texture), "Texture_UAV"); }
    uint32_t RegisterStructuredBufferSRV(nvrhi::BufferHandle buffer) { return WriteBindlessItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, buffer), "StructuredBuffer_SRV"); }
    uint32_t RegisterSamplerFeedbackTexture(nvrhi::SamplerFeedbackTextureHandle texture) { return WriteBindlessItem(nvrhi::BindingSetItem::SamplerFeedbackTexture_UAV(0, texture), "SamplerFeedbackTexture_UAV"); }
    bool RegisterTextureAtIndex(uint32_t index, nvrhi::TextureHandle texture) { return WriteBindlessItemAtIndex(index, nvrhi::BindingSetItem::Texture_SRV(index, texture), "Texture_SRV"); }
    bool RegisterSamplerFeedbackTextureAtIndex(uint32_t index, nvrhi::SamplerFeedbackTextureHandle texture) { return WriteBindlessItemAtIndex(index, nvrhi::BindingSetItem::SamplerFeedbackTexture_UAV(index, texture), "SamplerFeedbackTexture_UAV"); }

    nvrhi::DescriptorTableHandle GetStaticTextureDescriptorTable() const { return m_StaticTextureDescriptorTable; }
    nvrhi::BindingLayoutHandle GetStaticTextureBindingLayout() const { return m_StaticTextureBindingLayout; }

    // ─── Global bindless sampler heap ───

    void InitializeStaticBindlessSamplers();
    bool RegisterSamplerAtIndex(uint32_t index, nvrhi::SamplerHandle sampler);
    nvrhi::DescriptorTableHandle GetStaticSamplerDescriptorTable() const { return m_StaticSamplerDescriptorTable; }
    nvrhi::BindingLayoutHandle GetStaticSamplerBindingLayout() const { return m_StaticSamplerBindingLayout; }

    // ─── Queries ───

    double GetFrameTimeMs() const { return m_FrameTime; }
    void SetCameraFromSceneCamera(const Scene::Camera& sceneCam);

    // Returns the swapchain extent (width, height) as a pair.
    std::pair<uint32_t, uint32_t> SwapchainSize();

    static MicroProfileThreadLogGpu*& GetGPULogForCurrentThread();

    static nvrhi::BindingSetDesc CreateBindingSetDesc(std::span<const srrhi::ResourceEntry> resources, uint32_t pushConstantBytes = 0);

    template<typename SrInput>
    static nvrhi::BindingSetDesc CreateBindingSetDesc(const SrInput& inputs) { return CreateBindingSetDesc(inputs.m_Resources, SrInput::PushConstantBytes); }

    // ─── Texture streaming ───

    // True when sampler feedback can be used: the device supports it AND it was not
    // disabled with --disable-sampler-feedback. When false no sampler feedback texture
    // is created or resolved, and streaming synthesises an all-zero feedback map
    // ("finest mip required everywhere") instead of reading one back from the GPU.
    bool IsSamplerFeedbackEnabled() const { return m_bSamplerFeedbackEnabled; }

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

    // ─── Shader loading & pipeline hashing (internal) ───

    void HashPipelineCommonState(size_t& h, const nvrhi::RenderState&, const nvrhi::FramebufferInfoEx&, const nvrhi::BindingLayoutVector&);
    void LoadShaders();
    void UnloadShaders();
    void ReloadShaders();

    // ========================================================================
    // State
    // ========================================================================

    // ─── Window, device & swapchain ───

    SDL_Window* m_Window = nullptr;
    // Owns the nvrhi device, and therefore the Context every GPU resource holds a raw reference to.
    // Declared before every other member so it is destroyed last: members are destroyed in reverse
    // declaration order, so all GPU resources in this object die while the device is still alive.
    // Nothing below may be declared before m_RHI.
    std::unique_ptr<GraphicRHI> m_RHI;

    // Shared GPU resources (samplers, default textures, the unit-sphere mesh and its BLAS). Owned here
    // rather than in a function-local static: a static is destroyed at process exit, i.e. after
    // m_RHI has taken the device down, and its handles would then release against a dead Context.
    CommonResources m_CommonResources;

    uint32_t m_AcquiredSwapchainImageIdx = 0;
    uint32_t m_SwapChainImageIdx = 0;

    // ─── Render graph ───
    RenderGraph m_RenderGraph;

    // Shader handles — indexed by ShaderID:: constants, populated by LoadShaders()
    nvrhi::ShaderHandle m_ShaderHandles[ShaderID::COUNT]{};

    // ─── Editor UI ───
    ImGuiLayer m_ImGuiLayer;

    // ─── Task scheduling ───
    std::unique_ptr<TaskScheduler> m_TaskScheduler;

    // ─── Scene ───
    Scene m_Scene;

    // Camera state persistence (periodic save + restore on load)
    CameraStateManager m_CameraStateManager;

    // ─── Render passes ───
    std::vector<std::shared_ptr<IRenderer>> m_Renderers;

    // ─── Frame statistics & profiling ───
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
    // Cluster LOD: cooks the per-primitive cluster DAG and caches it in the cooked mesh file. Nothing draws it
    // yet, so this only decides whether the offline build runs.
    bool m_EnableClusterRendering = true;

    // Cluster traversal (phase 1): the threshold is a screen-space error in pixels, everything else exists so a
    // run can be scripted or A/B'd the way the panel does it.
    float m_ClusterPixelError = 1.0f;
    int m_ClusterForcedLOD = -1;     // -1 = pick per group from the metric, >= 0 = force one level
    uint32_t m_ClusterMaxClusters = 0; // 0 = the scene-derived worst case
    bool m_ClusterDisableCulling = false;
    bool m_ClusterFreezeCullingCamera = false;
    uint32_t m_ClusterDebugMode = 0;

    // Set by RequestClusterCountersReadback() on the stats frame, consumed by LogClusterCounters().
    static constexpr uint32_t kClusterCounterReadbackBuffers = 2;
    struct ClusterCountersReadback
    {
        // --cluster-stats-frame: one copy, read back after a wait-for-idle.
        nvrhi::BufferHandle m_Staging;
        const char* m_BucketName = nullptr;
        float m_FocalPixels = 0.0f;

        // Panel path: copied every frame while live, read one frame behind once its event has fired.
        nvrhi::BufferHandle m_PanelStaging[kClusterCounterReadbackBuffers];
        nvrhi::EventQueryHandle m_PanelEvent[kClusterCounterReadbackBuffers];
        srrhi::ClusterCounters m_Latest = {};
        bool m_bHasLatest = false;
    };
    ClusterCountersReadback m_ClusterCountersReadback[2];
    bool m_bClusterCountersPanelOpen = false;
    int m_ForcedTextureMip = -1; // -1 = auto, 0-15 = forced mip level
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
    bool m_EnableReSTIRDenoising = true;

    // Indirect lighting technique (mutually exclusive: 0=None, 1=RestirGI, 2=SHARC, 3=RestirGI+SHARC)
    uint32_t m_IndirectLightingTechnique = 3;

    // SHARC debug overlay (SHARCDebugMode enum value; 0 = off)
    uint32_t m_SHARCDebugMode = 0;

    // Bloom shaping parameters
    float m_BloomKnee = 0.1f;
    // Scene luminance below this blooms not at all. The emitters in Bistro are hundreds of times brighter than
    // the room around them, so with no threshold one lamp smears over the whole frame.
    float m_BloomThreshold = 1.0f;
    float m_UpsampleRadius = 0.85f;

    int      m_DebugMode = 0;
    int      m_ActiveDebugMode = 0;
    bool     m_bAnyDebugWasActive = false;
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

    // Opacity micromaps (OMM): baked per primitive per LOD, attached to the BLAS.
    bool m_bOmmSupported = false;        // Device capability, set during Initialize
    bool m_bEnableOmm = false;           // User toggle; off rebuilds the BLASes without the micromap linkage
    bool m_bOmmRebuildRequested = false; // Set by the panel, serviced by the frame loop
    int  m_OmmDebugMode = 0;             // 0 = off, 1 = texel classification, 2 = micromap grid, 3 = candidate heatmap
    int  m_OmmDebugPrimitive = 0;        // flat index over Scene::Mesh::m_Primitives
    int  m_OmmDebugMip = 0;              // alpha mip the classification is taken from
    bool m_bOmmShowOnlyMixed = false;    // classification view: draw mixed texels only
    bool m_bOmmDebugDirty = true;        // set by the panel, serviced by UpdateOmmDebugTexture
    OmmDebugTexelStats m_OmmDebugStats;  // classification of the selected primitive, read by the panel
    nvrhi::TextureHandle m_OmmDebugTexture; // RGBA8 view of m_OmmDebugStats, sampled by the debug pass

    // RenderDoc in-app capture: the panel requests one, the frame loop brackets the next frame with it, and
    // the newest written path is reported back in the panel.
    bool m_bRenderDocCaptureRequested = false;
    std::string m_RenderDocLastCapturePath;

    // PIX takes a capture through its own capturer module rather than an injected tool, so the bracket is
    // PIXBeginCapture2/PIXEndCapture around one frame (see PixCapture.h).
    bool m_bPixCaptureRequested = false;
    std::string m_PixLastCapturePath;

    // Environment Lighting settings
    bool m_EnableSky = true;

    // Effective sampler feedback state — see IsSamplerFeedbackEnabled().
    bool m_bSamplerFeedbackEnabled = false;

    std::unique_ptr<nvfeedback::FeedbackManager> m_FeedbackManager;

    std::unique_ptr<nvfeedback::AsyncTileIO> m_AsyncTileIO; // Async tile I/O thread pool

    // Tile requests that exceeded the per-frame budget (kMaxTilesPerFrame) and were
    // deferred to the next frame.  Drained before new requests each frame.
    std::vector<nvfeedback::FeedbackTextureUpdate> m_PendingTileRequests;

    // Count of tile indices actually submitted to AsyncTileIO this frame (for UI/debug).
    uint32_t m_TilesSubmittedThisFrame = 0;

    int m_TileResidencyDebugTextureIdx = -1; // -1 = disabled, 0..N = selected feedback texture index

    // ─── Command list pool ───

    std::vector<nvrhi::CommandListHandle> m_CommandListFreeList;
    std::vector<nvrhi::CommandListHandle> m_PendingCommandLists;
    std::vector<nvrhi::CommandListHandle> m_InFlightCommandLists;

    // Event queries to signal once this frame's command lists have executed (see SignalEventAfterFrame)
    std::vector<nvrhi::EventQueryHandle> m_EventsToSignal;

    // ─── Pipeline & binding-layout caches ───

    std::mutex m_CacheMutex;
    std::unordered_map<size_t, nvrhi::BindingLayoutHandle> m_BindingLayoutCache;
    std::unordered_map<size_t, nvrhi::GraphicsPipelineHandle> m_GraphicsPipelineCache;
    std::unordered_map<size_t, nvrhi::MeshletPipelineHandle> m_MeshletPipelineCache;
    std::unordered_map<size_t, nvrhi::ComputePipelineHandle> m_ComputePipelineCache;

    // ─── Bindless heap state ───

    // Texture heap: STATIC_* slots first, dynamically allocated indices after them.
    nvrhi::DescriptorTableHandle m_StaticTextureDescriptorTable;
    nvrhi::BindingLayoutHandle m_StaticTextureBindingLayout;
    uint32_t m_NextTextureIndex = srrhi::CommonConsts::DEFAULT_TEXTURE_COUNT;

    // Sampler heap
    nvrhi::DescriptorTableHandle m_StaticSamplerDescriptorTable;
    nvrhi::BindingLayoutHandle m_StaticSamplerBindingLayout;
    // Slots RegisterSamplerAtIndex has written; everything else is filled with the default sampler so the
    // shader-visible heap holds no uninitialised descriptors.
    std::unordered_set<uint32_t> m_RegisteredSamplerIndices;

    // ─── GPU timing ───

    nvrhi::TimerQueryHandle m_GPUQueries[2];

    // One timer query per GPU scope the frame records; the Profiler panel's GPU column reads it.
    GpuPassTimers m_GpuPassTimers;

    // ─── Runtime flags ───

    bool m_Running = true;
    bool m_RequestedShaderReload = false;

private:
    // ─── Bindless heap internals ───

    // Allocate an index, write a BindingSetItem to the global descriptor table, return index (UINT32_MAX on failure).
    uint32_t WriteBindlessItem(const nvrhi::BindingSetItem& item, const char* label);
    // Fills the slots of the two bindless tables that no registration wrote with a valid default descriptor.
    void FillUnusedBindlessSlots();
    // Rebuilds the OMM debug classification texture when the panel changed the selected primitive or mip.
    void UpdateOmmDebugTexture();
    // Write a BindingSetItem at a known index, return success.
    bool WriteBindlessItemAtIndex(uint32_t index, const nvrhi::BindingSetItem& item, const char* label);
};

// Global accessor for the Renderer singleton.
#define g_Renderer Renderer::GetInstance()

// ============================================================================
// RAII helpers
// ============================================================================

// Opens a debug marker on construction and holds the command list for the scope;
// releases the list back to the renderer on destruction.
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

// Opens a MicroProfile GPU timer scope on construction, closes it on destruction, and times the same range
// with a nvrhi timer query for the Profiler panel's GPU column.
class ScopedGpuProfile
{
public:
    ScopedGpuProfile(std::string_view name, const nvrhi::CommandListHandle& commandList);
    ~ScopedGpuProfile();

    ScopedGpuProfile(const ScopedGpuProfile&) = delete;
    ScopedGpuProfile& operator=(const ScopedGpuProfile&) = delete;

private:
    static std::string_view BuildMicroProfileName(std::string_view name);

    nvrhi::utils::ScopedMarker m_Marker;
    MicroProfileToken m_Token = MICROPROFILE_INVALID_TOKEN;
    MicroProfileScopeGpuHandler m_Scope;
    nvrhi::ICommandList* m_CommandList = nullptr;
    nvrhi::ITimerQuery* m_TimerQuery = nullptr;   // null when this scope got no query
};

// Usage: PROFILE_GPU_SCOPED("Pass name", commandList);
#define PROFILE_GPU_SCOPED(NAME, CMDLIST) ScopedGpuProfile GENERATE_UNIQUE_VARIABLE(scopedGpuProfile){ NAME, CMDLIST };