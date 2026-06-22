#include "Renderer.h"
#include "CommonResources.h"

// ============================================================================
// SHARCRenderer — Spatial Hash Radiance Cache indirect lighting renderer.
//
// Manages the three SHARC GPU buffers (hash entries, accumulation, resolved)
// plus a screen-space indirect output texture.  The actual Update / Resolve /
// Query passes will be added in Phase 2; this phase only establishes the
// resource infrastructure.
// ============================================================================

// ---------------------------------------------------------------------------
// Cache entry count — start at 2^20 (≈1 M entries, ~64 MiB total).
// Bump to 2^22 (~168 MiB) once the pipeline is validated.
// ---------------------------------------------------------------------------
static constexpr uint32_t k_SHARCCacheEntries = 1u << 20; // 1,048,576

// ---------------------------------------------------------------------------
// Global render-graph handles (read by DeferredRenderer to composite indirect)
// ---------------------------------------------------------------------------
RGBufferHandle  g_RG_SHARCHashEntries;   // RWStructuredBuffer<uint64_t>
RGBufferHandle  g_RG_SHARCAccumulation;  // RWStructuredBuffer<SharcAccumulationData>  (uint4 per entry)
RGBufferHandle  g_RG_SHARCResolved;      // RWStructuredBuffer<SharcPackedData>         (float16x4 + 2×uint)
RGTextureHandle g_RG_SHARCIndirect;      // RWTexture2D<float4>  — screen-space indirect output

// ============================================================================
class SHARCRenderer : public IRenderer
{
public:
    // ------------------------------------------------------------------
    // IRenderer interface
    // ------------------------------------------------------------------
    const char* GetName() const override { return "SHARCRenderer"; }

    void Initialize() override
    {
        // Nothing to initialize yet — shader constants will be set up in Phase 2.
    }

    bool Setup(RenderGraph& renderGraph) override
    {
        // Only participate when SHARC is the selected indirect technique.
        if (g_Renderer.m_IndirectLightingTechnique != IndirectLightingTechnique::SHARC)
            return false;

        const uint32_t width  = g_Renderer.m_RHI->m_SwapchainExtent.x;
        const uint32_t height = g_Renderer.m_RHI->m_SwapchainExtent.y;

        // ------------------------------------------------------------------
        // Hash entries buffer  —  RWStructuredBuffer<uint64_t>
        //   stride = 8 bytes,  count = k_SHARCCacheEntries
        //   total  = 8 MiB  (2^20) / 32 MiB (2^22)
        // ------------------------------------------------------------------
        {
            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize     = static_cast<uint64_t>(k_SHARCCacheEntries) * sizeof(uint64_t);
            bd.m_NvrhiDesc.structStride = sizeof(uint64_t);   // 8 bytes
            bd.m_NvrhiDesc.canHaveUAVs  = true;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bd.m_NvrhiDesc.keepInitialState = true;
            bd.m_NvrhiDesc.debugName    = "SHARC_HashEntries";
            renderGraph.DeclarePersistentBuffer(bd, g_RG_SHARCHashEntries);
        }

        // ------------------------------------------------------------------
        // Accumulation buffer  —  RWStructuredBuffer<SharcAccumulationData>
        //   SharcAccumulationData = uint4  →  stride = 16 bytes
        //   total = 16 MiB (2^20) / 64 MiB (2^22)
        // ------------------------------------------------------------------
        {
            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize     = static_cast<uint64_t>(k_SHARCCacheEntries) * 16u; // sizeof(uint4)
            bd.m_NvrhiDesc.structStride = 16u;
            bd.m_NvrhiDesc.canHaveUAVs  = true;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bd.m_NvrhiDesc.keepInitialState = true;
            bd.m_NvrhiDesc.debugName    = "SHARC_Accumulation";
            renderGraph.DeclarePersistentBuffer(bd, g_RG_SHARCAccumulation);
        }

        // ------------------------------------------------------------------
        // Resolved buffer  —  RWStructuredBuffer<SharcPackedData>
        //   SharcPackedData = float16_t4 (8 B) + uint (4 B) + uint (4 B) = 16 bytes
        //   total = 16 MiB (2^20) / 64 MiB (2^22)
        // ------------------------------------------------------------------
        {
            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize     = static_cast<uint64_t>(k_SHARCCacheEntries) * 16u; // sizeof(SharcPackedData)
            bd.m_NvrhiDesc.structStride = 16u;
            bd.m_NvrhiDesc.canHaveUAVs  = true;
            bd.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            bd.m_NvrhiDesc.keepInitialState = true;
            bd.m_NvrhiDesc.debugName    = "SHARC_Resolved";
            renderGraph.DeclarePersistentBuffer(bd, g_RG_SHARCResolved);
        }

        // ------------------------------------------------------------------
        // Indirect output texture  —  RWTexture2D<float4>  (R11G11B10_FLOAT)
        //   Screen-resolution; written by the Query pass, read by DeferredRenderer.
        // ------------------------------------------------------------------
        {
            RGTextureDesc td;
            td.m_NvrhiDesc.width        = width;
            td.m_NvrhiDesc.height       = height;
            td.m_NvrhiDesc.format       = nvrhi::Format::R11G11B10_FLOAT;
            td.m_NvrhiDesc.isUAV        = true;
            td.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            td.m_NvrhiDesc.debugName    = "SHARC_Indirect";
            renderGraph.DeclareTexture(td, g_RG_SHARCIndirect);
        }

        // Register write access for all resources this renderer owns.
        renderGraph.WriteBuffer(g_RG_SHARCHashEntries);
        renderGraph.WriteBuffer(g_RG_SHARCAccumulation);
        renderGraph.WriteBuffer(g_RG_SHARCResolved);
        renderGraph.WriteTexture(g_RG_SHARCIndirect);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        // Phase 1: resource infrastructure only.
        // Update / Resolve / Query passes will be implemented in Phase 2.
        (void)commandList;
        (void)renderGraph;
    }
};

// ---------------------------------------------------------------------------
// Self-registration via the REGISTER_RENDERER macro
// ---------------------------------------------------------------------------
REGISTER_RENDERER(SHARCRenderer)
