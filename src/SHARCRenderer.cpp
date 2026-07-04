#include "Renderer.h"
#include "CommonResources.h"
#include "Utilities.h"

#include "shaders/srrhi/cpp/SHARC.h"

// ============================================================================
// SHARCRenderer — Spatial Hash Radiance Cache indirect lighting renderer.
//
// Pass order each frame:
//   1. Update  — sparse BRDF-ray compute pass; populates the accumulation cache.
//   2. Resolve — combines accumulation with the resolved buffer (Phase 3).
//   3. Query   — screen-space lookup; writes g_RG_SHARCIndirect (Phase 3).
// ============================================================================

// ---------------------------------------------------------------------------
// GBuffer handles declared by CommonRenderers / BasePassRenderer
// ---------------------------------------------------------------------------
extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_GBufferNormals;
extern RGTextureHandle g_RG_GBufferAlbedo;
extern RGTextureHandle g_RG_GBufferORM;

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

    bool Setup(RenderGraph& renderGraph) override
    {
        // Participate when SHARC is the selected indirect technique, or when
        // the combined ReSTIR GI + SHARC mode is active (SHARC provides the
        // secondary-surface radiance cache; the Query pass is skipped in that mode).
        if (g_Renderer.m_IndirectLightingTechnique != srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_SHARC &&
            g_Renderer.m_IndirectLightingTechnique != srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_RESTIR_GI_SHARC)
            return false;

        const uint32_t width  = g_Renderer.m_RHI->m_SwapchainExtent.x;
        const uint32_t height = g_Renderer.m_RHI->m_SwapchainExtent.y;

        // ------------------------------------------------------------------
        // Hash entries buffer  —  RWStructuredBuffer<uint64_t>
        //   stride = 8 bytes,  count = srrhi::SHARCConsts::SHARC_CACHE_ENTRIES
        //   total  = 8 MiB  (2^20) / 32 MiB (2^22)
        // ------------------------------------------------------------------
        {
            RGBufferDesc bd;
            bd.m_NvrhiDesc.byteSize     = static_cast<uint64_t>(srrhi::SHARCConsts::SHARC_CACHE_ENTRIES) * sizeof(uint64_t);
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
            bd.m_NvrhiDesc.byteSize     = static_cast<uint64_t>(srrhi::SHARCConsts::SHARC_CACHE_ENTRIES) * 16u; // sizeof(uint4)
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
            bd.m_NvrhiDesc.byteSize     = static_cast<uint64_t>(srrhi::SHARCConsts::SHARC_CACHE_ENTRIES) * 16u; // sizeof(SharcPackedData)
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

        renderGraph.ReadTexture(g_RG_DepthTexture);
        renderGraph.ReadTexture(g_RG_GBufferNormals);
        renderGraph.ReadTexture(g_RG_GBufferAlbedo);
        renderGraph.ReadTexture(g_RG_GBufferORM);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_FUNCTION();

        const uint32_t width  = g_Renderer.m_RHI->m_SwapchainExtent.x;
        const uint32_t height = g_Renderer.m_RHI->m_SwapchainExtent.y;

        // ── Resolve physical buffer handles ──────────────────────────────────
        nvrhi::BufferHandle  hashEntries  = renderGraph.GetBuffer (g_RG_SHARCHashEntries,  RGResourceAccessMode::Write);
        nvrhi::BufferHandle  accumulation = renderGraph.GetBuffer (g_RG_SHARCAccumulation, RGResourceAccessMode::Write);
        nvrhi::BufferHandle  resolved     = renderGraph.GetBuffer (g_RG_SHARCResolved,     RGResourceAccessMode::Write);

        // ── Clear stale persistent state when switching TO SHARC ─────────────
        // Stale hash entries, accumulated radiance, and resolved radiance from a
        // previous technique (or a previous scene) will produce garbage / NaNs on
        // the first frames after the switch.  Zero all three buffers so the cache
        // starts fresh.
        if (m_bClearOnNextRender)
        {
            commandList->clearBufferUInt(hashEntries,  0u);
            commandList->clearBufferUInt(accumulation, 0u);
            commandList->clearBufferUInt(resolved,     0u);
            m_bClearOnNextRender = false;
        }

        nvrhi::TextureHandle depth        = renderGraph.GetTexture(g_RG_DepthTexture,      RGResourceAccessMode::Read);
        nvrhi::TextureHandle normals      = renderGraph.GetTexture(g_RG_GBufferNormals,    RGResourceAccessMode::Read);
        nvrhi::TextureHandle albedo       = renderGraph.GetTexture(g_RG_GBufferAlbedo,     RGResourceAccessMode::Read);
        nvrhi::TextureHandle orm          = renderGraph.GetTexture(g_RG_GBufferORM,        RGResourceAccessMode::Read);

        // ── Per-frame camera state ────────────────────────────────────────────
        const Vector3 camPos = g_Renderer.m_Scene.m_Camera.GetPosition();

        // Sun angular radius
        const float angularSizeDeg = !g_Renderer.m_Scene.m_Lights.empty()
            ? g_Renderer.m_Scene.m_Lights.back().m_AngularSize
            : 0.533f;
        const float halfAngleRad = angularSizeDeg * 0.5f * (std::numbers::pi_v<float> / 180.0f);

        // ── Build SHARCConstants ─────────────────────────────────────────────
        const nvrhi::BufferDesc cbDesc = nvrhi::utils::CreateVolatileConstantBufferDesc(
            sizeof(srrhi::SHARCConstants), "SHARC_CB", 1);
        nvrhi::BufferHandle cb = g_Renderer.m_RHI->m_NvrhiDevice->createBuffer(cbDesc);

        srrhi::SHARCConstants constants;
        constants.SetFrameIndex(g_Renderer.m_FrameNumber);
        constants.SetCameraPosition(Vector3{ camPos.x, camPos.y, camPos.z });
        constants.SetLightCount(g_Renderer.m_Scene.m_LightCount);
        constants.SetSunDirection(g_Renderer.m_Scene.GetSunDirection());
        constants.SetCosSunAngularRadius(cosf(halfAngleRad));
        constants.SetViewportSize(Vector2U{ width, height });
        constants.SetViewportSizeInv(Vector2{ 1.0f / width, 1.0f / height });
        constants.SetMatClipToWorld(g_Renderer.m_Scene.m_Camera.GetInvViewProjMatrix());
        // Previous-frame camera position from Scene's ViewPrev matrix.
        {
            const auto& vt = g_Renderer.m_Scene.m_ViewPrev.m_MatViewToWorld;
            constants.SetCameraPositionPrev(DirectX::XMFLOAT3{ vt.m[3][0], vt.m[3][1], vt.m[3][2] });
        }
        constants.SetSHARCDebugMode(g_Renderer.m_SHARCDebugMode);

        commandList->writeBuffer(cb, &constants, sizeof(constants));

        // ── Pass 1: SHARC Update ─────────────────────────────────────────────
        // Dispatch over the full viewport; the shader itself skips non-selected
        // pixels via the sparse block selection logic.
        {
            srrhi::SHARCUpdateInputs inputs;
            inputs.SetConst(cb);
            inputs.SetDepth(depth);
            inputs.SetNormals(normals);
            inputs.SetAlbedo(albedo);
            inputs.SetORM(orm);
            inputs.SetHashEntries(hashEntries);
            inputs.SetAccumulation(accumulation);
            inputs.SetResolved(resolved);
            inputs.SetSceneAS(g_Renderer.m_Scene.m_TLAS);
            inputs.SetLights(g_Renderer.m_Scene.m_LightBuffer);
            inputs.SetInstances(g_Renderer.m_Scene.m_InstanceDataBuffer);
            inputs.SetMeshData(g_Renderer.m_Scene.m_MeshDataBuffer);
            inputs.SetMaterials(g_Renderer.m_Scene.m_MaterialConstantsBuffer);
            inputs.SetVertices(g_Renderer.m_Scene.m_VertexBufferQuantized);
            inputs.SetIndices(g_Renderer.m_Scene.m_IndexBuffer);

            Renderer::RenderPassParams params{};
            params.commandList    = commandList;
            params.shaderID       = ShaderID::SHARCUPDATE_SHARCUPDATE_CSMAIN;
            params.bindingSetDesc = Renderer::CreateBindingSetDesc(inputs);
            params.dispatchParams = {
                .x = DivideAndRoundUp(width,  8u),
                .y = DivideAndRoundUp(height, 8u),
                .z = 1u
            };
            g_Renderer.AddComputePass(params);
        }

        // ── Pass 2: SHARC Resolve ────────────────────────────────────────────
        // Iterates over every cache entry (linear dispatch) and performs
        // temporal EMA blending, staleness eviction, and accumulation clear.
        {
            srrhi::SHARCResolveInputs inputs;
            inputs.SetConst(cb);
            inputs.SetHashEntries(hashEntries);
            inputs.SetAccumulation(accumulation);
            inputs.SetResolved(resolved);

            const uint32_t resolveGroups = DivideAndRoundUp(
                static_cast<uint32_t>(srrhi::SHARCConsts::SHARC_CACHE_ENTRIES),
                static_cast<uint32_t>(srrhi::SHARCConsts::LINEAR_BLOCK_SIZE));

            Renderer::RenderPassParams params{};
            params.commandList    = commandList;
            params.shaderID       = ShaderID::SHARCRESOLVE_SHARCRESOLVE_CSMAIN;
            params.bindingSetDesc = Renderer::CreateBindingSetDesc(inputs);
            params.dispatchParams = { .x = resolveGroups, .y = 1u, .z = 1u };
            g_Renderer.AddComputePass(params);
        }

        // ── Pass 3: SHARC Query ──────────────────────────────────────────────
        // Fullscreen compute pass: reads GBuffer depth + normals, calls
        // SharcGetCachedRadiance() at each primary surface, and writes the
        // resulting indirect radiance to g_RG_SHARCIndirect for compositing
        // in DeferredLighting.
        //
        // In combined mode (RESTIR_GI_SHARC) the query happens inside
        // ShadeSecondarySurfaces instead — skip this standalone pass.
        const bool bCombinedMode = (g_Renderer.m_IndirectLightingTechnique ==
            srrhi::IndirectLightingMode::INDIRECT_LIGHTING_MODE_RESTIR_GI_SHARC);
        if (!bCombinedMode)
        {
            // The Resolve pass left the resolved buffer in UAV state; the Query
            // pass reads it as an SRV.  Obtain it with Read access so the
            // render-graph inserts the UAV→SRV barrier automatically.
            nvrhi::BufferHandle  resolvedSRV    = renderGraph.GetBuffer (g_RG_SHARCResolved,    RGResourceAccessMode::Read);
            nvrhi::BufferHandle  hashEntriesSRV = renderGraph.GetBuffer (g_RG_SHARCHashEntries, RGResourceAccessMode::Read);
            nvrhi::BufferHandle  accumBuffer    = renderGraph.GetBuffer (g_RG_SHARCAccumulation, RGResourceAccessMode::Read);
            nvrhi::TextureHandle indirectOutput = renderGraph.GetTexture(g_RG_SHARCIndirect,    RGResourceAccessMode::Write);

            srrhi::SHARCQueryInputs inputs;
            inputs.SetConst(cb);
            inputs.SetDepth(depth);
            inputs.SetNormals(normals);
            inputs.SetAlbedo(albedo);
            inputs.SetORM(orm);
            inputs.SetHashEntries(hashEntriesSRV);
            inputs.SetResolved(resolvedSRV);
            inputs.SetAccumulation(accumBuffer);
            inputs.SetIndirectOutput(indirectOutput, 0);
            inputs.SetSceneAS(g_Renderer.m_Scene.m_TLAS);
            inputs.SetInstances(g_Renderer.m_Scene.m_InstanceDataBuffer);
            inputs.SetMeshData(g_Renderer.m_Scene.m_MeshDataBuffer);
            inputs.SetMaterials(g_Renderer.m_Scene.m_MaterialConstantsBuffer);
            inputs.SetVertices(g_Renderer.m_Scene.m_VertexBufferQuantized);
            inputs.SetIndices(g_Renderer.m_Scene.m_IndexBuffer);

            Renderer::RenderPassParams params{};
            params.commandList    = commandList;
            params.shaderID       = ShaderID::SHARCQUERY_SHARCQUERY_CSMAIN;
            params.bindingSetDesc = Renderer::CreateBindingSetDesc(inputs);
            params.dispatchParams = {
                .x = DivideAndRoundUp(width,  8u),
                .y = DivideAndRoundUp(height, 8u),
                .z = 1u
            };
            g_Renderer.AddComputePass(params);
        }
    }
};

// ---------------------------------------------------------------------------
// Self-registration via the REGISTER_RENDERER macro
// ---------------------------------------------------------------------------
REGISTER_RENDERER(SHARCRenderer)
