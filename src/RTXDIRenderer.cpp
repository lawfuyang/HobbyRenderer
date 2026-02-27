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

RGTextureHandle g_RG_RTXDIDIOutput;
extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_GBufferAlbedo;
extern RGTextureHandle g_RG_GBufferNormals;
extern RGTextureHandle g_RG_GBufferORM;
extern RGTextureHandle g_RG_GBufferMotionVectors;

// ============================================================================

class RTXDIRenderer : public IRenderer
{
public:
    // ------------------------------------------------------------------
    // Persistent GPU buffers (survive across frames)
    // ------------------------------------------------------------------
    nvrhi::BufferHandle m_NeighborOffsetsBuffer;
    nvrhi::BufferHandle m_RISBuffer;
    nvrhi::BufferHandle m_LightReservoirBuffer;

    // ------------------------------------------------------------------
    // Persistent textures (G-buffer history for previous frame)
    // ------------------------------------------------------------------
    RGTextureHandle m_GBufferAlbedoHistory;
    RGTextureHandle m_GBufferORMHistory;

    // Tracks if history textures are newly created in current frame
    bool m_AlbedoHistoryIsNew = false;
    bool m_ORMHistoryIsNew    = false;

    // ------------------------------------------------------------------
    // RTXDI context (owns frame-index tracking and buffer-index bookkeeping)
    // ------------------------------------------------------------------
    std::unique_ptr<rtxdi::ReSTIRDIContext> m_Context;

    void Initialize() override
    {
        Renderer* renderer = Renderer::GetInstance();
        nvrhi::IDevice* device = renderer->m_RHI->m_NvrhiDevice;

        const uint32_t width = renderer->m_RHI->m_SwapchainExtent.x;
        const uint32_t height = renderer->m_RHI->m_SwapchainExtent.y;

        // Create the ReSTIRDI context
        rtxdi::ReSTIRDIStaticParameters staticParams;
        staticParams.RenderWidth = width;
        staticParams.RenderHeight = height;
        m_Context = std::make_unique<rtxdi::ReSTIRDIContext>(staticParams);

        // Tweak default resampling settings
        {
            ReSTIRDI_TemporalResamplingParameters temporal = m_Context->GetTemporalResamplingParameters();
            temporal.maxHistoryLength = 20;
            temporal.enableBoilingFilter = true;
            temporal.boilingFilterStrength = 0.25f;
            m_Context->SetTemporalResamplingParameters(temporal);

            ReSTIRDI_SpatialResamplingParameters spatial = m_Context->GetSpatialResamplingParameters();
            spatial.numSpatialSamples = 1;
            m_Context->SetSpatialResamplingParameters(spatial);

            ReSTIRDI_InitialSamplingParameters initial = m_Context->GetInitialSamplingParameters();
            initial.numPrimaryLocalLightSamples = 1;
            initial.numPrimaryInfiniteLightSamples = 1;
            initial.numPrimaryBrdfSamples = 0;
            initial.numPrimaryEnvironmentSamples = 0;
            initial.enableInitialVisibility = false;
            m_Context->SetInitialSamplingParameters(initial);
        }

        // ---- Neighbor offsets buffer --------------------------------
        {
            const uint32_t count = staticParams.NeighborOffsetCount;
            std::vector<uint8_t> offsets(count * 2); // two int8/uint8 per entry
            rtxdi::FillNeighborOffsetBuffer(offsets.data(), count);

            nvrhi::BufferDesc bd;
            bd.byteSize = offsets.size();
            bd.structStride = sizeof(uint8_t) * 2;
            bd.format = nvrhi::Format::RG8_UNORM;
            bd.initialState = nvrhi::ResourceStates::ShaderResource;
            bd.keepInitialState = true;
            bd.debugName = "RTXDI_NeighborOffsets";
            m_NeighborOffsetsBuffer = device->createBuffer(bd);

            // Upload immediately via a transient command list
            nvrhi::CommandListHandle cl = renderer->AcquireCommandList();
            ScopedCommandList scopeCl {cl, "Upload RTXDI Neighbor Offsets"};
            scopeCl->writeBuffer(m_NeighborOffsetsBuffer, offsets.data(), offsets.size());
        }

        // ---- RIS buffer --------------------------------------------
        // The context doesn't have presampling so size can be minimal.
        // Minimum of 1 tile × 1024 elements = 1024 uint2 entries.
        {
            const uint32_t risEntries = 1024u;
            nvrhi::BufferDesc bd;
            bd.byteSize = static_cast<uint64_t>(risEntries) * sizeof(uint32_t) * 2;
            bd.structStride = sizeof(uint32_t) * 2;
            bd.format = nvrhi::Format::RG32_UINT;
            bd.canHaveUAVs = true;
            bd.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bd.keepInitialState = true;
            bd.debugName = "RTXDI_RISBuffer";
            m_RISBuffer = device->createBuffer(bd);
        }

        // ---- Light reservoir buffer --------------------------------
        {
            RTXDI_ReservoirBufferParameters rbp = m_Context->GetReservoirBufferParameters();
            const uint32_t totalReservoirs = rbp.reservoirArrayPitch * rtxdi::c_NumReSTIRDIReservoirBuffers;

            nvrhi::BufferDesc bd;
            bd.byteSize = static_cast<uint64_t>(totalReservoirs) * sizeof(RTXDI_PackedDIReservoir);
            bd.structStride = sizeof(RTXDI_PackedDIReservoir);
            bd.canHaveUAVs = true;
            bd.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bd.keepInitialState = true;
            bd.debugName = "RTXDI_LightReservoirBuffer";
            m_LightReservoirBuffer = device->createBuffer(bd);
        }
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
            desc.m_NvrhiDesc.width = width;
            desc.m_NvrhiDesc.height = height;
            desc.m_NvrhiDesc.format = Renderer::GBUFFER_ORM_FORMAT;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.m_NvrhiDesc.debugName = "GBufferORMHistory";
            m_ORMHistoryIsNew = renderGraph.DeclarePersistentTexture(desc, m_GBufferORMHistory);
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
        nvrhi::TextureHandle albedoHistoryTex = renderGraph.GetTexture(m_GBufferAlbedoHistory, RGResourceAccessMode::Write);
        nvrhi::TextureHandle ormHistoryTex    = renderGraph.GetTexture(m_GBufferORMHistory,    RGResourceAccessMode::Write);

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
            nvrhi::BindingSetItem::RayTracingAccelStruct(10, renderer->m_Scene.m_TLAS), // TODO: Enable when previous frame TLAS is available
            nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_NeighborOffsetsBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_RISBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_LightReservoirBuffer),
            nvrhi::BindingSetItem::Texture_UAV(2, diOutput),
        };

        // ------------------------------------------------------------------
        // Pass 1 — Generate Initial Samples
        // ------------------------------------------------------------------
        {
            Renderer::RenderPassParams params{
                .commandList  = commandList,
                .shaderName   = "RTXDIGenerateInitialSamples_CSMain",
                .bindingSetDesc = bset,
                .dispatchParams = {
                    .x = DivideAndRoundUp(width,  8u),
                    .y = DivideAndRoundUp(height, 8u),
                    .z = 1u
                }
            };
            renderer->AddComputePass(params);
        }

        // ------------------------------------------------------------------
        // Pass 2 — Temporal Resampling (conditional)
        // ------------------------------------------------------------------
        if (renderer->m_ReSTIRDI_EnableTemporal)
        {
            Renderer::RenderPassParams params{
                .commandList  = commandList,
                .shaderName   = "RTXDITemporalResampling_CSMain",
                .bindingSetDesc = bset,
                .dispatchParams = {
                    .x = DivideAndRoundUp(width,  8u),
                    .y = DivideAndRoundUp(height, 8u),
                    .z = 1u
                }
            };
            renderer->AddComputePass(params);
        }

        // ------------------------------------------------------------------
        // Pass 3 — Shade Samples → write to DI output
        // ------------------------------------------------------------------
        {
            Renderer::RenderPassParams params{
                .commandList  = commandList,
                .shaderName   = "RTXDIShadeSamples_CSMain",
                .bindingSetDesc = bset,
                .dispatchParams = {
                    .x = DivideAndRoundUp(width,  8u),
                    .y = DivideAndRoundUp(height, 8u),
                    .z = 1u
                }
            };
            renderer->AddComputePass(params);
        }

        // ------------------------------------------------------------------
        // Copy current G-buffer to history textures for next frame
        // ------------------------------------------------------------------
        commandList->copyTexture(albedoHistoryTex, nvrhi::TextureSlice{}, albedoTex, nvrhi::TextureSlice{});
        commandList->copyTexture(ormHistoryTex, nvrhi::TextureSlice{}, ormTex, nvrhi::TextureSlice{});
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
