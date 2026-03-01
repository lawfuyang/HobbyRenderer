/*
 * RTXDIRenderer.cpp
 *
 * Implements the ReSTIR DI pipeline:
 *   1. GenerateInitialSamples  — one thread per pixel, picks candidate light
 *   2. TemporalResampling      — combines with previous-frame reservoir
 *   3. ShadeSamples            — evaluates BRDF and writes radiance to g_RG_RTXDIDIOutput
 *
 * Controlled by Renderer::m_EnableReSTIRDI.  When disabled, Setup() bails out early
 * and the DeferredRenderer uses its normal AccumulateDirectLighting() path.
 */

#include "Renderer.h"
#include "CommonResources.h"
#include "Utilities.h"

#include "shaders/ShaderShared.h"

#include <Rtxdi/DI/ReSTIRDI.h>
#include <Rtxdi/RtxdiUtils.h>
#include <Rtxdi/LightSampling/RISBufferSegmentParameters.h>

RGTextureHandle g_RG_RTXDIDIOutput;
extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_GBufferAlbedo;
extern RGTextureHandle g_RG_GBufferNormals;
extern RGTextureHandle g_RG_GBufferORM;
extern RGTextureHandle g_RG_GBufferMotionVectors;

// ============================================================================

// ---- Tile parameters for the presampling pass --------------------------------
// These are fixed at startup. Increasing tileCount improves spatial diversity;
// increasing tileSize reduces variance within a tile. Values match FullSample defaults.
static constexpr uint32_t k_RISTileSize  = 1024u;  // samples per tile
static constexpr uint32_t k_RISTileCount = 128u;   // number of tiles
// Compact buffer stride: 3 × uint4 per RIS entry (see RTXDIApplicationBridge.hlsli).
static constexpr uint32_t k_CompactSlotsPerEntry = 3u;

class RTXDIRenderer : public IRenderer
{
public:
    // ------------------------------------------------------------------
    // Persistent GPU buffers (survive across frames)
    // ------------------------------------------------------------------
    RGBufferHandle m_RG_NeighborOffsetsBuffer;
    RGBufferHandle m_RG_RISBuffer;
    RGBufferHandle m_RG_LightReservoirBuffer;

    // Track if neighbor offsets buffer was newly allocated (needs initial fill)
    bool m_NeighborOffsetsBufferIsNew = false;

    // Compact light info buffer written by the presample pass and read by
    // the initial sampling pass (via RAB_LoadCompactLightInfo).
    // Layout: k_CompactSlotsPerEntry × uint4 per RIS tile entry.
    RGBufferHandle m_RG_RISLightDataBuffer;

    // Local-light PDF texture for power-importance presampling.
    // Square, power-of-2, R32_FLOAT with a full mip chain.
    // Mip 0 has one texel per local light (Z-curve mapped), written every frame
    // by RTXDIBuildLocalLightPDF; the mip chain is then regenerated.
    // RTXDI_PresampleLocalLights consumes the full chain for power-weighted
    // tile filling — this is the actual mechanism behind Power-RIS.
    RGTextureHandle      m_RG_LocalLightPDFTexture;
    uint32_t             m_PDFTexSize  = 0; // side length of the square texture
    uint32_t             m_PDFMipCount = 0; // number of mip levels

    // SPD atomic counter for mip generation
    RGBufferHandle       m_RG_SPDAtomicCounter;

    // ------------------------------------------------------------------
    // Persistent textures (G-buffer history for previous frame)
    // ------------------------------------------------------------------
    RGTextureHandle m_DepthHistory;
    RGTextureHandle m_GBufferAlbedoHistory;
    RGTextureHandle m_GbufferNormalsHistory;
    RGTextureHandle m_GBufferORMHistory;

    // Track if history textures are newly created in current frame
    bool m_AlbedoHistoryIsNew = false;
    bool m_ORMHistoryIsNew    = false;
    bool m_DepthHistoryIsNew  = false;
    bool m_NormalsHistoryIsNew = false;

    // ------------------------------------------------------------------
    // Persistent ray tracing acceleration structures
    // ------------------------------------------------------------------
    nvrhi::rt::AccelStructHandle m_TLASHistory;

    // ------------------------------------------------------------------
    // RTXDI context (owns frame-index tracking and buffer-index bookkeeping)
    // ------------------------------------------------------------------
    std::unique_ptr<rtxdi::ReSTIRDIContext> m_Context;

    // Tracks whether the context was created with checkerboard mode enabled,
    // so we can detect when the user toggles it and recreate the context.
    bool m_CheckerboardEnabled = false;

    void CreateRTXDIContext()
    {
        Renderer* renderer = Renderer::GetInstance();

        m_CheckerboardEnabled = renderer->m_ReSTIRDI_EnableCheckerboard;
        const rtxdi::CheckerboardMode newMode = m_CheckerboardEnabled
            ? rtxdi::CheckerboardMode::Black
            : rtxdi::CheckerboardMode::Off;

        const uint32_t width = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        // Create the ReSTIRDI context
        rtxdi::ReSTIRDIStaticParameters staticParams;
        staticParams.RenderWidth = width;
        staticParams.RenderHeight = height;
        staticParams.CheckerboardSamplingMode = newMode;
        m_Context = std::make_unique<rtxdi::ReSTIRDIContext>(staticParams);
    }

    void Initialize() override
    {
        CreateRTXDIContext();
    }

    void PostSceneLoad() override
    {
        Renderer* renderer = Renderer::GetInstance();
        nvrhi::IDevice* device = renderer->m_RHI->m_NvrhiDevice;

        // ---- TLAS History (for temporal effects) -------------------
        const uint32_t maxInstances = static_cast<uint32_t>(renderer->m_Scene.m_InstanceData.size());

        nvrhi::rt::AccelStructDesc tlasHistoryDesc;
        tlasHistoryDesc.topLevelMaxInstances = maxInstances;
        tlasHistoryDesc.debugName = "RTXDI_TLAS_History";
        tlasHistoryDesc.isTopLevel = true;
        m_TLASHistory = device->createAccelStruct(tlasHistoryDesc);

        nvrhi::CommandListHandle cl = renderer->AcquireCommandList();
        ScopedCommandList scopeCl{ cl, "RTXDI::Initialize" };
        scopeCl->buildTopLevelAccelStructFromBuffer(m_TLASHistory, renderer->m_Scene.m_RTInstanceDescBuffer, 0, (uint32_t)renderer->m_Scene.m_RTInstanceDescs.size());
    }

    // ------------------------------------------------------------------
    bool Setup(RenderGraph& renderGraph) override
    {
        Renderer* renderer = Renderer::GetInstance();

        // Only run in normal / IBL mode; skip for path tracer
        if (renderer->m_Mode == RenderingMode::ReferencePathTracer)
            return false;

        // If the user turned RTXDI off, we want the DeferredRenderer to take
        // the classic path — don't participate in the render graph at all.
        if (!renderer->m_EnableReSTIRDI)
            return false;

        nvrhi::IDevice* device = renderer->m_RHI->m_NvrhiDevice;
        const uint32_t width  = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        // Recreate the context when the user toggles checkerboard mode, because
        // CheckerboardSamplingMode is a static parameter that changes reservoir
        // buffer layout.  (The reservoir buffer is transient and will be
        // automatically resized in the same Setup() call.)
        if (renderer->m_ReSTIRDI_EnableCheckerboard != m_CheckerboardEnabled)
        {
            CreateRTXDIContext();
        }

        const uint32_t totalRISEntries = k_RISTileSize * k_RISTileCount; // 131 072

        // ------------------------------------------------------------------
        // Declare / retrieve the DI output texture (persistent across frames)
        // ------------------------------------------------------------------
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = Renderer::HDR_COLOR_FORMAT; // R11G11B10_FLOAT
            desc.m_NvrhiDesc.isUAV = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            desc.m_NvrhiDesc.debugName = "RTXDIDIOutput";
            renderGraph.DeclareTexture(desc, g_RG_RTXDIDIOutput);
        }

        // ------------------------------------------------------------------
        // Declare / retrieve persistent G-buffer history textures
        // ------------------------------------------------------------------
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = Renderer::GBUFFER_ALBEDO_FORMAT;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.m_NvrhiDesc.debugName = "GBufferAlbedoHistory";
            m_AlbedoHistoryIsNew = renderGraph.DeclarePersistentTexture(desc, m_GBufferAlbedoHistory);
        }

        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width  = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = Renderer::GBUFFER_ORM_FORMAT;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.m_NvrhiDesc.debugName = "GBufferORMHistory";
            m_ORMHistoryIsNew = renderGraph.DeclarePersistentTexture(desc, m_GBufferORMHistory);
        }

        // Previous-frame depth history (copy of depth buffer: same format, D24S8).
        // isRenderTarget=true so NVRHI creates it with ALLOW_DEPTH_STENCIL, which is
        // required to copy from the main depth buffer (also ALLOW_DEPTH_STENCIL) and
        // to create a proper depth-component SRV.
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width        = width;
            desc.m_NvrhiDesc.height       = height;
            desc.m_NvrhiDesc.format       = Renderer::DEPTH_FORMAT;
            desc.m_NvrhiDesc.isRenderTarget = true; // needed for ALLOW_DEPTH_STENCIL flag
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.m_NvrhiDesc.debugName    = "DepthHistory";
            m_DepthHistoryIsNew = renderGraph.DeclarePersistentTexture(desc, m_DepthHistory);
        }

        // Previous-frame G-buffer normals history.
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width       = width;
            desc.m_NvrhiDesc.height      = height;
            desc.m_NvrhiDesc.format      = Renderer::GBUFFER_NORMALS_FORMAT;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.m_NvrhiDesc.debugName   = "GBufferNormalsHistory";
            m_NormalsHistoryIsNew = renderGraph.DeclarePersistentTexture(desc, m_GbufferNormalsHistory);
        }

        {
            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize = m_Context->GetStaticParameters().NeighborOffsetCount * 2;
            bd.m_NvrhiDesc.format   = nvrhi::Format::RG8_SNORM; // 2 × int8 normalized [-1,1] per entry
            bd.m_NvrhiDesc.canHaveTypedViews = true;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            bd.m_NvrhiDesc.debugName    = "RTXDI_NeighborOffsets";
            m_NeighborOffsetsBufferIsNew = renderGraph.DeclarePersistentBuffer(bd, m_RG_NeighborOffsetsBuffer);
        }

        // ---- RIS buffer (transient) -----------------------------------------------
        // Sized for all presampled local light tiles.
        {
            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize     = static_cast<uint64_t>(totalRISEntries) * sizeof(uint32_t) * 2;
            bd.m_NvrhiDesc.structStride = sizeof(uint32_t) * 2;
            bd.m_NvrhiDesc.format       = nvrhi::Format::RG32_UINT;
            bd.m_NvrhiDesc.canHaveUAVs  = true;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bd.m_NvrhiDesc.debugName    = "RTXDI_RISBuffer";
            renderGraph.DeclareBuffer(bd, m_RG_RISBuffer);
        }

        // ---- Light reservoir buffer (transient) --------------------------------
        {
            RTXDI_ReservoirBufferParameters rbp = m_Context->GetReservoirBufferParameters();
            const uint32_t totalReservoirs = rbp.reservoirArrayPitch * rtxdi::c_NumReSTIRDIReservoirBuffers;

            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize = static_cast<uint64_t>(totalReservoirs) * sizeof(RTXDI_PackedDIReservoir);
            bd.m_NvrhiDesc.structStride = sizeof(RTXDI_PackedDIReservoir);
            bd.m_NvrhiDesc.canHaveUAVs = true;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bd.m_NvrhiDesc.debugName = "RTXDI_LightReservoirBuffer";
            renderGraph.DeclareBuffer(bd, m_RG_LightReservoirBuffer);
        }

        // ---- Compact light data buffer --------------------------------
        // Stores k_CompactSlotsPerEntry × uint4 per RIS tile entry.
        // Written by the presample pass, read by the initial sampling pass.
        // Must be updated every frame because light properties can change.
        {
            const uint64_t numUint4s = static_cast<uint64_t>(totalRISEntries) * k_CompactSlotsPerEntry;
            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize = numUint4s * sizeof(uint32_t) * 4; // each uint4 = 16 bytes
            bd.m_NvrhiDesc.structStride = sizeof(uint32_t) * 4;             // stride = 1 uint4
            bd.m_NvrhiDesc.canHaveUAVs = true;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bd.m_NvrhiDesc.keepInitialState = true;
            bd.m_NvrhiDesc.debugName = "RTXDI_RISLightDataBuffer";
            renderGraph.DeclareBuffer(bd, m_RG_RISLightDataBuffer);
        }

        // ---- Local-light PDF texture -----------------------------------------------
        // Sized so the Z-curve can address every local light in the scene.
        // Light count is fixed post-load, so this is created once.
        {
            // Local lights = all lights except index 0 (the sun/directional).
            const uint32_t localLightCount = (renderer->m_Scene.m_LightCount > 1u)
                ? renderer->m_Scene.m_LightCount - 1u
                : 0u;

            // Find smallest power-of-2 S such that S*S >= localLightCount.
            m_PDFTexSize = 1u;
            while (m_PDFTexSize* m_PDFTexSize < localLightCount)
                m_PDFTexSize <<= 1u;

            // Mip count: from full-res down to 1×1.
            m_PDFMipCount = 1u;
            for (uint32_t s = m_PDFTexSize; s > 1u; s >>= 1u)
                ++m_PDFMipCount;

            RGTextureDesc desc;
            desc.m_NvrhiDesc.width = m_PDFTexSize;
            desc.m_NvrhiDesc.height = m_PDFTexSize;
            desc.m_NvrhiDesc.mipLevels = m_PDFMipCount;
            desc.m_NvrhiDesc.format = nvrhi::Format::R32_FLOAT;
            desc.m_NvrhiDesc.isUAV = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            desc.m_NvrhiDesc.debugName = "RTXDI_LocalLightPDFTexture";
            renderGraph.DeclareTexture(desc, m_RG_LocalLightPDFTexture);
        }

        // ---- SPD atomic counter for mip generation ------
        {
            renderGraph.DeclareBuffer(RenderGraph::GetSPDAtomicCounterDesc("RTXDI PDF Mip SPD Atomic Counter"), m_RG_SPDAtomicCounter);
            renderGraph.WriteBuffer(m_RG_SPDAtomicCounter);
        }

        // Register accesses
        renderGraph.ReadTexture(g_RG_DepthTexture);
        renderGraph.ReadTexture(g_RG_GBufferAlbedo);
        renderGraph.ReadTexture(g_RG_GBufferNormals);
        renderGraph.ReadTexture(g_RG_GBufferORM);
        renderGraph.ReadTexture(g_RG_GBufferMotionVectors);

        return true;
    }

    // ------------------------------------------------------------------
    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_FUNCTION();
        Renderer* renderer = Renderer::GetInstance();
        nvrhi::IDevice* device = renderer->m_RHI->m_NvrhiDevice;

        // Advance the frame index so the context produces fresh buffer indices
        m_Context->SetFrameIndex(renderer->m_FrameNumber);

        // Apply app-level spatial sample count
        {
            ReSTIRDI_SpatialResamplingParameters spatial = m_Context->GetSpatialResamplingParameters();
            spatial.numSpatialSamples = static_cast<uint32_t>(renderer->m_ReSTIRDI_SpatialSamples);
            m_Context->SetSpatialResamplingParameters(spatial);
        }

        // Choose resampling mode
        {
            rtxdi::ReSTIRDI_ResamplingMode mode = rtxdi::ReSTIRDI_ResamplingMode::None;
            const bool wantTemporal = renderer->m_ReSTIRDI_EnableTemporal;
            const bool wantSpatial  = renderer->m_ReSTIRDI_EnableSpatial;
            if (wantTemporal && wantSpatial)
                mode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
            else if (wantTemporal)
                mode = rtxdi::ReSTIRDI_ResamplingMode::Temporal;
            else if (wantSpatial)
                mode = rtxdi::ReSTIRDI_ResamplingMode::Spatial;
            m_Context->SetResamplingMode(mode);
        }

        // ------------------------------------------------------------------
        // Build RTXDIConstants from context accessors
        // ------------------------------------------------------------------
        const RTXDI_ReservoirBufferParameters rbp = m_Context->GetReservoirBufferParameters();
        const RTXDI_RuntimeParameters         rtp = m_Context->GetRuntimeParams();
        const ReSTIRDI_BufferIndices          bix = m_Context->GetBufferIndices();
        const RTXDI_LightBufferParameters     lbp = BuildLightBufferParams(renderer);

        const uint32_t width  = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        // In checkerboard mode each frame operates on half the horizontal pixels;
        // the shader uses activeCheckerboardField to select which half.
        const bool checkerboard = m_Context->GetStaticParameters().CheckerboardSamplingMode != rtxdi::CheckerboardMode::Off;
        const uint32_t dispatchWidth = checkerboard ? width / 2 : width;

        RTXDIConstants cb{};
        cb.m_ViewportSize                        = { width, height };
        cb.m_FrameIndex                          = renderer->m_FrameNumber;
        cb.m_LightCount                          = renderer->m_Scene.m_LightCount;

        // RTXDI_RuntimeParameters
        cb.m_NeighborOffsetMask                  = rtp.neighborOffsetMask;
        cb.m_ActiveCheckerboardField             = rtp.activeCheckerboardField;

        // Local light region
        cb.m_LocalLightFirstIndex                = lbp.localLightBufferRegion.firstLightIndex;
        cb.m_LocalLightCount                     = lbp.localLightBufferRegion.numLights;

        // Infinite light region (sun = index 0 maps to infinite)
        cb.m_InfiniteLightFirstIndex             = lbp.infiniteLightBufferRegion.firstLightIndex;
        cb.m_InfiniteLightCount                  = lbp.infiniteLightBufferRegion.numLights;

        // Environment light
        cb.m_EnvLightPresent                     = lbp.environmentLightParams.lightPresent;
        cb.m_EnvLightIndex                       = lbp.environmentLightParams.lightIndex;

        // Reservoir buffer params
        cb.m_ReservoirBlockRowPitch              = rbp.reservoirBlockRowPitch;
        cb.m_ReservoirArrayPitch                 = rbp.reservoirArrayPitch;

        // Buffer indices
        cb.m_InitialSamplingOutputBufferIndex    = bix.initialSamplingOutputBufferIndex;
        cb.m_TemporalResamplingInputBufferIndex  = bix.temporalResamplingInputBufferIndex;
        cb.m_TemporalResamplingOutputBufferIndex = bix.temporalResamplingOutputBufferIndex;
        cb.m_SpatialResamplingInputBufferIndex   = bix.spatialResamplingInputBufferIndex;
        cb.m_SpatialResamplingOutputBufferIndex  = bix.spatialResamplingOutputBufferIndex;
        cb.m_ShadingInputBufferIndex             = bix.shadingInputBufferIndex;
        cb.m_EnableSky                           = renderer->m_EnableSky ? 1u : 0u;

        // ---- Local-light PDF texture size ----
        cb.m_LocalLightPDFTextureSize = { m_PDFTexSize, m_PDFTexSize };

        // ---- RIS buffer segment parameters for presampling ----
        cb.m_LocalRISBufferOffset = 0u;
        cb.m_LocalRISTileSize     = k_RISTileSize;
        cb.m_LocalRISTileCount    = k_RISTileCount;
        cb.m_LocalRISPad          = 0u;

        // Environment lights are not presampled — leave these as zeros.
        cb.m_EnvRISBufferOffset   = 0u;
        cb.m_EnvRISTileSize       = 0u;
        cb.m_EnvRISTileCount      = 0u;
        cb.m_EnvRISPad            = 0u;

        // Spatial resampling parameters
        const ReSTIRDI_SpatialResamplingParameters& spatialParams = m_Context->GetSpatialResamplingParameters();
        cb.m_SpatialNumSamples        = spatialParams.numSpatialSamples;
        cb.m_SpatialSamplingRadius    = spatialParams.spatialSamplingRadius;

        // View matrices
        cb.m_View     = renderer->m_Scene.m_View;
        cb.m_PrevView = renderer->m_Scene.m_ViewPrev;

        cb.m_SunDirection = renderer->m_Scene.m_SunDirection;

        // Upload constant buffer (volatile — recreated every frame)
        const nvrhi::BufferHandle rtxdiCB = device->createBuffer(
            nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(RTXDIConstants), "RTXDIConstantsCB", 1));
        commandList->writeBuffer(rtxdiCB, &cb, sizeof(cb));

        // Retrieve render graph resources
        nvrhi::TextureHandle depthTex   = renderGraph.GetTexture(g_RG_DepthTexture,          RGResourceAccessMode::Read);
        nvrhi::TextureHandle albedoTex  = renderGraph.GetTexture(g_RG_GBufferAlbedo,         RGResourceAccessMode::Read);
        nvrhi::TextureHandle normalsTex = renderGraph.GetTexture(g_RG_GBufferNormals,        RGResourceAccessMode::Read);
        nvrhi::TextureHandle ormTex     = renderGraph.GetTexture(g_RG_GBufferORM,            RGResourceAccessMode::Read);
        nvrhi::TextureHandle motionTex  = renderGraph.GetTexture(g_RG_GBufferMotionVectors,  RGResourceAccessMode::Read);
        nvrhi::TextureHandle diOutput   = renderGraph.GetTexture(g_RG_RTXDIDIOutput,         RGResourceAccessMode::Write);
        nvrhi::TextureHandle albedoHistoryTex  = renderGraph.GetTexture(m_GBufferAlbedoHistory, RGResourceAccessMode::Write);
        nvrhi::TextureHandle ormHistoryTex     = renderGraph.GetTexture(m_GBufferORMHistory,    RGResourceAccessMode::Write);
        nvrhi::TextureHandle depthHistoryTex   = renderGraph.GetTexture(m_DepthHistory,         RGResourceAccessMode::Write);
        nvrhi::TextureHandle normalsHistoryTex = renderGraph.GetTexture(m_GbufferNormalsHistory, RGResourceAccessMode::Write);
        nvrhi::BufferHandle neighborOffsetsBuffer = renderGraph.GetBuffer(m_RG_NeighborOffsetsBuffer, RGResourceAccessMode::Read);
        nvrhi::BufferHandle risBuffer = renderGraph.GetBuffer(m_RG_RISBuffer, RGResourceAccessMode::Read);
        nvrhi::BufferHandle lightReservoirBuffer = renderGraph.GetBuffer(m_RG_LightReservoirBuffer, RGResourceAccessMode::Read);
        nvrhi::BufferHandle risLightDataBuffer = renderGraph.GetBuffer(m_RG_RISLightDataBuffer, RGResourceAccessMode::Write);
        nvrhi::TextureHandle localLightPDFTex  = renderGraph.GetTexture(m_RG_LocalLightPDFTexture, RGResourceAccessMode::Write);

        // ------------------------------------------------------------------
        // Initialize history textures on first frame
        // ------------------------------------------------------------------
        if (m_AlbedoHistoryIsNew)
        {
            commandList->copyTexture(albedoHistoryTex, nvrhi::TextureSlice{}, albedoTex, nvrhi::TextureSlice{});
        }
        if (m_ORMHistoryIsNew)
        {
            commandList->copyTexture(ormHistoryTex, nvrhi::TextureSlice{}, ormTex, nvrhi::TextureSlice{});
        }
        if (m_DepthHistoryIsNew)
        {
            commandList->copyTexture(depthHistoryTex, nvrhi::TextureSlice{}, depthTex, nvrhi::TextureSlice{});
        }
        if (m_NormalsHistoryIsNew)
        {
            commandList->copyTexture(normalsHistoryTex, nvrhi::TextureSlice{}, normalsTex, nvrhi::TextureSlice{});
        }

        // ------------------------------------------------------------------
        // Initialize neighbor offsets buffer on first allocation
        // ------------------------------------------------------------------
        if (m_NeighborOffsetsBufferIsNew)
        {
            const uint32_t count = m_Context->GetStaticParameters().NeighborOffsetCount;
            std::vector<uint8_t> offsets(count * 2); // two int8/uint8 per entry
            rtxdi::FillNeighborOffsetBuffer(offsets.data(), count);
            commandList->writeBuffer(neighborOffsetsBuffer, offsets.data(), offsets.size());
        }

        nvrhi::BindingSetDesc bset;
        bset.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(1, rtxdiCB),
            nvrhi::BindingSetItem::Texture_SRV(1,  depthTex),
            nvrhi::BindingSetItem::Texture_SRV(2,  normalsTex),
            nvrhi::BindingSetItem::Texture_SRV(3,  albedoTex),
            nvrhi::BindingSetItem::Texture_SRV(4,  ormTex),
            nvrhi::BindingSetItem::Texture_SRV(5,  motionTex),
            nvrhi::BindingSetItem::Texture_SRV(8,  albedoHistoryTex),
            nvrhi::BindingSetItem::Texture_SRV(9,  ormHistoryTex),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(6, renderer->m_Scene.m_LightBuffer),
            nvrhi::BindingSetItem::RayTracingAccelStruct(7, renderer->m_Scene.m_TLAS),
            nvrhi::BindingSetItem::RayTracingAccelStruct(10, m_TLASHistory),
            nvrhi::BindingSetItem::TypedBuffer_SRV(0, neighborOffsetsBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(0, risBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(1, lightReservoirBuffer),
            nvrhi::BindingSetItem::Texture_UAV(2, diOutput),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(3, risLightDataBuffer),
            nvrhi::BindingSetItem::Texture_SRV(11, localLightPDFTex),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(12, renderer->m_Scene.m_InstanceDataBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(13, renderer->m_Scene.m_MaterialConstantsBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(14, renderer->m_Scene.m_VertexBufferQuantized),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(15, renderer->m_Scene.m_MeshDataBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(16, renderer->m_Scene.m_IndexBuffer),
            nvrhi::BindingSetItem::Texture_SRV(17, depthHistoryTex),   // previous-frame depth
            nvrhi::BindingSetItem::Texture_SRV(18, normalsHistoryTex), // previous-frame normals
        };

        // ------------------------------------------------------------------
        // Build Local-Light PDF (mip 0)
        // Writes luminance(light.radiance) into the PDF texture's mip 0 at
        // each local light's Z-curve position.  Must run every frame because
        // light intensities can change between frames.
        // ------------------------------------------------------------------
        if (lbp.localLightBufferRegion.numLights > 0)
        {
            {
                nvrhi::BindingSetDesc buildPDFBset;
                buildPDFBset.bindings = {
                    nvrhi::BindingSetItem::ConstantBuffer(1, rtxdiCB),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(6, renderer->m_Scene.m_LightBuffer),
                    // u4: mip-0 UAV — written by the build shader
                    nvrhi::BindingSetItem::Texture_UAV(4, localLightPDFTex,
                        nvrhi::Format::UNKNOWN,
                        nvrhi::TextureSubresourceSet{0, 1, 0, 1}),
                };
                Renderer::RenderPassParams params{
                    .commandList = commandList,
                    .shaderName = "RTXDIBuildLocalLightPDF_CSMain",
                    .bindingSetDesc = buildPDFBset,
                    .dispatchParams = {
                        .x = DivideAndRoundUp(m_PDFTexSize, 8u),
                        .y = DivideAndRoundUp(m_PDFTexSize, 8u),
                        .z = 1u
                    }
                };
                renderer->AddComputePass(params);
            }

            // ------------------------------------------------------------------
            // Generate PDF mip chain using SPD
            // Required so RTXDI_PresampleLocalLights can do hierarchical CDF
            // traversal across all mip levels.
            // ------------------------------------------------------------------
            if (m_PDFMipCount > 1u)
            {
                nvrhi::BufferHandle spdAtomicCounter = renderGraph.GetBuffer(m_RG_SPDAtomicCounter, RGResourceAccessMode::Write);
                renderer->GenerateMipsUsingSPD(localLightPDFTex, spdAtomicCounter, commandList, "Generate Local Light PDF Mips", SPD_REDUCTION_AVERAGE);
            }

            // ------------------------------------------------------------------
            // Presample Local Lights (Power-RIS via RTXDI SDK)
            // Reads the full PDF mip chain via RTXDI_PresampleLocalLights to fill
            // the RIS tiles with importance-sampled lights, storing compact data.
            // ------------------------------------------------------------------
            {
                const uint32_t presampleGroupsX = DivideAndRoundUp(k_RISTileSize, 256u);
                Renderer::RenderPassParams params{
                    .commandList = commandList,
                    .shaderName = "RTXDIPresampleLights_CSMain",
                    .bindingSetDesc = bset,
                    .dispatchParams = {
                        .x = presampleGroupsX,
                        .y = k_RISTileCount,
                        .z = 1u
                    }
                };
                renderer->AddComputePass(params);
            }
        }

        // ------------------------------------------------------------------
        // Generate Initial Samples
        // ------------------------------------------------------------------
        {
            Renderer::RenderPassParams params{
                .commandList  = commandList,
                .shaderName   = "RTXDIGenerateInitialSamples_CSMain",
                .bindingSetDesc = bset,
                .dispatchParams = {
                    .x = DivideAndRoundUp(dispatchWidth, 8u),
                    .y = DivideAndRoundUp(height,        8u),
                    .z = 1u
                }
            };
            renderer->AddComputePass(params);
        }

        // ------------------------------------------------------------------
        // Temporal Resampling (conditional)
        // ------------------------------------------------------------------
        if (renderer->m_ReSTIRDI_EnableTemporal)
        {
            Renderer::RenderPassParams params{
                .commandList  = commandList,
                .shaderName   = "RTXDITemporalResampling_CSMain",
                .bindingSetDesc = bset,
                .dispatchParams = {
                    .x = DivideAndRoundUp(dispatchWidth, 8u),
                    .y = DivideAndRoundUp(height,        8u),
                    .z = 1u
                }
            };
            renderer->AddComputePass(params);
        }

        // ------------------------------------------------------------------
        // Spatial Resampling (conditional)
        // ------------------------------------------------------------------
        if (renderer->m_ReSTIRDI_EnableSpatial)
        {
            Renderer::RenderPassParams params{
                .commandList  = commandList,
                .shaderName   = "RTXDISpatialResampling_CSMain",
                .bindingSetDesc = bset,
                .dispatchParams = {
                    .x = DivideAndRoundUp(dispatchWidth, 8u),
                    .y = DivideAndRoundUp(height,        8u),
                    .z = 1u
                }
            };
            renderer->AddComputePass(params);
        }

        // ------------------------------------------------------------------
        // Shade Samples → write to DI output
        // ------------------------------------------------------------------
        {
            Renderer::RenderPassParams params{
                .commandList  = commandList,
                .shaderName   = "RTXDIShadeSamples_CSMain",
                .bindingSetDesc = bset,
                .dispatchParams = {
                    .x = DivideAndRoundUp(dispatchWidth, 8u),
                    .y = DivideAndRoundUp(height,        8u),
                    .z = 1u
                }
            };
            renderer->AddComputePass(params);
        }

        // ------------------------------------------------------------------
        // Copy current G-buffer to history textures for next frame
        // ------------------------------------------------------------------
        commandList->copyTexture(albedoHistoryTex,  nvrhi::TextureSlice{}, albedoTex,   nvrhi::TextureSlice{});
        commandList->copyTexture(ormHistoryTex,     nvrhi::TextureSlice{}, ormTex,      nvrhi::TextureSlice{});
        commandList->copyTexture(depthHistoryTex,   nvrhi::TextureSlice{}, depthTex,    nvrhi::TextureSlice{});
        commandList->copyTexture(normalsHistoryTex, nvrhi::TextureSlice{}, normalsTex,  nvrhi::TextureSlice{});

        // Copy current TLAS to history for next frame
        if (m_TLASHistory && renderer->m_Scene.m_TLAS)
        {
            commandList->copyRaytracingAccelerationStructure(m_TLASHistory, renderer->m_Scene.m_TLAS);
        }
    }

    const char* GetName() const override { return "RTXDIRenderer"; }

private:
    // ------------------------------------------------------------------
    // Helper: build RTXDI_LightBufferParameters from current scene lights.
    // Light index 0 is always the directional/sun → infinite light slot.
    // All others are local (point/spot).
    // ------------------------------------------------------------------
    static RTXDI_LightBufferParameters BuildLightBufferParams(const Renderer* renderer)
    {
        RTXDI_LightBufferParameters lbp{};
        const uint32_t totalLights = renderer->m_Scene.m_LightCount;

        if (totalLights == 0)
            return lbp;

        // Sun (always index 0) → infinite light region
        lbp.infiniteLightBufferRegion.firstLightIndex = 0;
        lbp.infiniteLightBufferRegion.numLights       = 1;

        // Remaining lights → local light region
        if (totalLights > 1)
        {
            lbp.localLightBufferRegion.firstLightIndex = 1;
            lbp.localLightBufferRegion.numLights       = totalLights - 1;
        }

        // No environment map in Phase 1
        lbp.environmentLightParams.lightPresent = 0;
        lbp.environmentLightParams.lightIndex   = 0;

        return lbp;
    }
};

REGISTER_RENDERER(RTXDIRenderer);
