#include "Renderer.h"
#include "CommonResources.h"
#include "Utilities.h"

#include "shaders/srrhi/cpp/SHARC.h"

#include <imgui.h>

// ============================================================================
// Render Graph texture handles shared between SHARC passes
// ============================================================================

// HDR color output (written by SharcQuery, read by post-processing)
extern RGTextureHandle g_RG_HDRColor;

// Depth and GBuffer (written by BasePass, read by SharcDebugViz for hash grid)
extern RGTextureHandle g_RG_DepthTexture;
extern RGTextureHandle g_RG_GBufferNormals;

// Per-pixel bounce count (written by SharcQuery, read by DebugViz)
static RGTextureHandle g_RG_SharcBounceCount;

// Per-pixel cached radiance (written by SharcQuery, read by DebugViz)
static RGTextureHandle g_RG_SharcCachedRadiance;

// ============================================================================
// Per-frame state
// ============================================================================

// When true
// When true, all three cache buffers are cleared to zero at the start of the
// next frame (first use, scene reload, or user-triggered "Clear Cache").
static bool g_SharcNeedsClear = true;

// SHARC-specific configuration
struct SharcConfig
{
    // Cache capacity: 2^22 entries (~160 MB total across all 3 buffers)
    static constexpr uint32_t kCacheCapacity = (1u << 22);

    // Maximum number of frames a cache entry survives without new samples
    // before being evicted. Fixed at 100 frames per spec.
    static constexpr uint32_t kStaleFrameNumMax = 100;

    // Maximum number of frames used for temporal accumulation window
    uint32_t m_AccumulationFrameNum = 64;

    // Scene scale: controls voxel size distribution. Larger = coarser voxels.
    float m_SceneScale = 50.0f;

    // Minimum roughness clamped on hit materials during the update pass.
    // Prevents near-mirror surfaces from polluting the cache with
    // high-variance entries. Range [0.0, 1.0], RTXGI default = 0.4.
    float m_RoughnessThreshold = 0.4f;
} g_SharcConfig;

// ============================================================================
// SharcIMGUISettings — called from ImGuiLayer when SHARC is selected
// ============================================================================

void SharcIMGUISettings()
{
    ImGui::Indent();

    ImGui::Text("Cache: 2^22 entries (~160 MB)");
    ImGui::Separator();

    ImGui::SliderInt("Accumulation Frames",
        (int*)&g_SharcConfig.m_AccumulationFrameNum,
        1, 120);
    ImGui::SetItemTooltip("Number of frames for temporal accumulation window.\n"
                          "Higher = smoother but slower to respond to changes.");

    ImGui::SliderFloat("Scene Scale",
        &g_SharcConfig.m_SceneScale,
        5.0f, 200.0f);
    ImGui::SetItemTooltip("Controls voxel size distribution.\n"
                          "Larger = coarser voxels, better for large scenes.");

    ImGui::SliderFloat("Roughness Threshold",
        &g_SharcConfig.m_RoughnessThreshold,
        0.0f, 1.0f);
    ImGui::SetItemTooltip("Minimum roughness clamped during the update pass.\n"
                          "Higher values prevent mirror-like surfaces from polluting the cache.\n"
                          "RTXGI default = 0.4.");

    ImGui::Separator();

    if (ImGui::Button("Clear Cache"))
        g_SharcNeedsClear = true;
    ImGui::SetItemTooltip("Force-clear all SHARC cache buffers on the next frame.");

    ImGui::Unindent();
}

// ============================================================================
// Buffer layout constants (must match SharcCommon.h)
// ============================================================================

static constexpr uint32_t kHashEntryStride    = sizeof(uint64_t);   // 8 B
static constexpr uint32_t kAccumulationStride = 4 * sizeof(uint32_t); // 16 B
static constexpr uint32_t kResolvedStride     = 16;                  // 16 B

// ============================================================================
// Helper: build and write the SHARC constant buffer
// ============================================================================

static nvrhi::BufferHandle BuildSharcCB(nvrhi::CommandListHandle commandList)
{
    const nvrhi::BufferDesc cbDesc = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(srrhi::SharcConstants), "SharcCB", 1);
    nvrhi::BufferHandle cb = g_Renderer.m_RHI->m_NvrhiDevice->createBuffer(cbDesc);

    const srrhi::PlanarViewConstants& view = g_Renderer.m_Scene.m_View;
    const srrhi::PlanarViewConstants& viewPrev = g_Renderer.m_Scene.m_ViewPrev;

    srrhi::SharcConstants constants;
    constants.SetCameraPosition(GetTranslation(view.m_MatViewToWorld));
    constants.SetCameraPositionPrev(GetTranslation(viewPrev.m_MatViewToWorld));
    constants.SetEntriesNum(SharcConfig::kCacheCapacity);
    constants.SetSceneScale(g_SharcConfig.m_SceneScale);
    constants.SetAccumulationFrameNum(g_SharcConfig.m_AccumulationFrameNum);
    constants.SetStaleFrameNumMax(SharcConfig::kStaleFrameNumMax);
    constants.SetFrameIndex(g_Renderer.m_FrameNumber);
    constants.SetViewportSize(view.m_ViewportSize);
    constants.SetViewportSizeInv(view.m_ViewportSizeInv);
    constants.SetMatClipToWorldNoOffset(view.m_MatClipToWorldNoOffset);
    constants.SetSunDirection(g_Renderer.m_Scene.GetSunDirection());
    constants.SetDebugMode((uint32_t)g_Renderer.m_DebugMode);
    constants.SetLightCount(g_Renderer.m_Scene.m_LightCount);
    constants.SetRoughnessThreshold(g_SharcConfig.m_RoughnessThreshold);

    commandList->writeBuffer(cb, &constants, sizeof(constants));
    return cb;
}

// ============================================================================
// SharcRenderer — orchestrates Update → Resolve → Query passes
// ============================================================================

class SharcRenderer : public IRenderer
{
    // ============================================================================
    // Persistent SHARC cache buffer handles
    // (declared as persistent RG resources so they survive across frames)
    // ============================================================================
    RGBufferHandle m_Sharc_HashEntries;    // uint64 per entry  — 8 B × 2^22 = 32 MB
    RGBufferHandle m_Sharc_Accumulation;   // uint4  per entry  — 16 B × 2^22 = 64 MB
    RGBufferHandle m_Sharc_Resolved;       // uint4  per entry  — 16 B × 2^22 = 64 MB

public:
    bool Setup(RenderGraph& renderGraph) override
    {
        // Only active when SHARC is the selected indirect lighting technique
        if (g_Renderer.m_IndirectLightingTechnique != IndirectLightingTechnique::SHARC)
            return false;

        const uint32_t cap = SharcConfig::kCacheCapacity;

        // Hash Entries — uint64 per entry
        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.byteSize = (uint64_t)cap * kHashEntryStride;
            desc.m_NvrhiDesc.structStride = kHashEntryStride;
            desc.m_NvrhiDesc.canHaveUAVs = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            desc.m_NvrhiDesc.keepInitialState = true;
            desc.m_NvrhiDesc.debugName = "SHARC_HashEntries";
            renderGraph.DeclarePersistentBuffer(desc, m_Sharc_HashEntries);
        }

        // Accumulation — uint4 per entry
        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.byteSize = (uint64_t)cap * kAccumulationStride;
            desc.m_NvrhiDesc.structStride = kAccumulationStride;
            desc.m_NvrhiDesc.canHaveUAVs = true;
            desc.m_NvrhiDesc.canHaveRawViews = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            desc.m_NvrhiDesc.keepInitialState = true;
            desc.m_NvrhiDesc.debugName = "SHARC_Accumulation";
            renderGraph.DeclarePersistentBuffer(desc, m_Sharc_Accumulation);
        }

        // Resolved — uint4 per entry
        {
            RGBufferDesc desc;
            desc.m_NvrhiDesc.byteSize = (uint64_t)cap * kResolvedStride;
            desc.m_NvrhiDesc.structStride = kResolvedStride;
            desc.m_NvrhiDesc.canHaveUAVs = true;
            desc.m_NvrhiDesc.canHaveRawViews = true;
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            desc.m_NvrhiDesc.keepInitialState = true;
            desc.m_NvrhiDesc.debugName = "SHARC_Resolved";
            renderGraph.DeclarePersistentBuffer(desc, m_Sharc_Resolved);
        }

        // Declare per-pixel bounce count texture (transient, written by Query, read by DebugViz)
        // Only needed for the Bounce Heatmap debug visualization mode
        if (g_Renderer.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_SHARC_BOUNCE_HEATMAP)
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width        = g_Renderer.m_RHI->m_SwapchainExtent.x;
            desc.m_NvrhiDesc.height       = g_Renderer.m_RHI->m_SwapchainExtent.y;
            desc.m_NvrhiDesc.format       = nvrhi::Format::R32_UINT;
            desc.m_NvrhiDesc.isUAV        = true;
            desc.m_NvrhiDesc.debugName    = "SHARC_BounceCount";
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            renderGraph.DeclareTexture(desc, g_RG_SharcBounceCount);
        }

        // Declare per-pixel cached radiance texture (transient, written by Query, read by DebugViz)
        // Only needed for the Cached Radiance debug visualization mode
        if (g_Renderer.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_SHARC_CACHED_RADIANCE)
        {
            RGTextureDesc desc;
            desc.m_NvrhiDesc.width        = g_Renderer.m_RHI->m_SwapchainExtent.x;
            desc.m_NvrhiDesc.height       = g_Renderer.m_RHI->m_SwapchainExtent.y;
            desc.m_NvrhiDesc.format       = nvrhi::Format::R11G11B10_FLOAT;
            desc.m_NvrhiDesc.isUAV        = true;
            desc.m_NvrhiDesc.debugName    = "SHARC_CachedRadiance";
            desc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            renderGraph.DeclareTexture(desc, g_RG_SharcCachedRadiance);
        }

        renderGraph.WriteTexture(g_RG_HDRColor);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_FUNCTION();

        nvrhi::BufferHandle sharcCB = BuildSharcCB(commandList);

        // Clear on first use or when explicitly requested
        if (g_SharcNeedsClear)
        {
            nvrhi::BufferHandle hashBuf  = renderGraph.GetBuffer(m_Sharc_HashEntries,  RGResourceAccessMode::Write);
            nvrhi::BufferHandle accumBuf = renderGraph.GetBuffer(m_Sharc_Accumulation, RGResourceAccessMode::Write);
            nvrhi::BufferHandle resBuf   = renderGraph.GetBuffer(m_Sharc_Resolved,     RGResourceAccessMode::Write);

            // Hash entries must be 0u (HASH_GRID_INVALID_HASH_KEY)
            commandList->clearBufferUInt(hashBuf,  0u);
            commandList->clearBufferUInt(accumBuf, 0u);
            commandList->clearBufferUInt(resBuf,   0u);

            g_SharcNeedsClear = false;
        }

        // ── Pass 1: SHARC Update ─────────────────────────────────────────────
        RenderSharcUpdate(commandList, renderGraph, sharcCB);

        // ── Pass 2: SHARC Resolve ────────────────────────────────────────────
        RenderSharcResolve(commandList, renderGraph, sharcCB);

        // ── Pass 3: SHARC Query ──────────────────────────────────────────────
        RenderSharcQuery(commandList, renderGraph, sharcCB);

    }

    const char* GetName() const override { return "SharcRenderer"; }

private:
    void RenderSharcUpdate(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph, nvrhi::BufferHandle sharcCB)
    {
        PROFILE_GPU_SCOPED("SharcUpdate", commandList);

        nvrhi::BufferHandle hashBuf  = renderGraph.GetBuffer(m_Sharc_HashEntries,  RGResourceAccessMode::Write);
        nvrhi::BufferHandle accumBuf = renderGraph.GetBuffer(m_Sharc_Accumulation, RGResourceAccessMode::Write);
        nvrhi::BufferHandle resBuf   = renderGraph.GetBuffer(m_Sharc_Resolved,     RGResourceAccessMode::Write);

        srrhi::SharcUpdateInputs inputs;
        inputs.SetSharcCB(sharcCB);
        inputs.SetSceneAS(g_Renderer.m_Scene.m_TLAS);
        inputs.SetLights(g_Renderer.m_Scene.m_LightBuffer);
        inputs.SetInstances(g_Renderer.m_Scene.m_InstanceDataBuffer);
        inputs.SetMeshData(g_Renderer.m_Scene.m_MeshDataBuffer);
        inputs.SetMaterials(g_Renderer.m_Scene.m_MaterialConstantsBuffer);
        inputs.SetIndices(g_Renderer.m_Scene.m_IndexBuffer);
        inputs.SetVertices(g_Renderer.m_Scene.m_VertexBufferQuantized);
        inputs.SetHashEntries(hashBuf);
        inputs.SetAccumulationBuffer(accumBuf);
        inputs.SetResolvedBuffer(resBuf);

        // Dispatch over downscaled grid (~4% of pixels, 1 per 5×5 block)
        const uint32_t kDownscale = 5u;
        const uint32_t dispW = DivideAndRoundUp(g_Renderer.m_RHI->m_SwapchainExtent.x, kDownscale * 8u);
        const uint32_t dispH = DivideAndRoundUp(g_Renderer.m_RHI->m_SwapchainExtent.y, kDownscale * 8u);

        g_Renderer.AddComputePass({
            .commandList    = commandList,
            .shaderID       = ShaderID::SHARCUPDATE_SHARCUPDATE_CSMAIN_HASH_GRID_ENABLE_64_BIT_ATOMICS_1_SHARC_UPDATE_1,
            .bindingSetDesc = Renderer::CreateBindingSetDesc(inputs),
            .dispatchParams = { .x = dispW, .y = dispH, .z = 1 }
        });
    }

    void RenderSharcResolve(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph, nvrhi::BufferHandle sharcCB)
    {
        PROFILE_GPU_SCOPED("SharcResolve", commandList);

        nvrhi::BufferHandle hashBuf  = renderGraph.GetBuffer(m_Sharc_HashEntries,  RGResourceAccessMode::Write);
        nvrhi::BufferHandle accumBuf = renderGraph.GetBuffer(m_Sharc_Accumulation, RGResourceAccessMode::Write);
        nvrhi::BufferHandle resBuf   = renderGraph.GetBuffer(m_Sharc_Resolved,     RGResourceAccessMode::Write);

        srrhi::SharcResolveInputs inputs;
        inputs.SetSharcCB(sharcCB);
        inputs.SetHashEntries(hashBuf);
        inputs.SetAccumulationBuffer(accumBuf);
        inputs.SetResolvedBuffer(resBuf);

        // One thread per cache entry, 256 threads per group
        const uint32_t dispX = DivideAndRoundUp(SharcConfig::kCacheCapacity, 256u);

        g_Renderer.AddComputePass({
            .commandList    = commandList,
            .shaderID       = ShaderID::SHARCRESOLVE_SHARCRESOLVE_CSMAIN_HASH_GRID_ENABLE_64_BIT_ATOMICS_1,
            .bindingSetDesc = Renderer::CreateBindingSetDesc(inputs),
            .dispatchParams = { .x = dispX, .y = 1, .z = 1 }
        });
    }

    void RenderSharcQuery(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph, nvrhi::BufferHandle sharcCB)
    {
        PROFILE_GPU_SCOPED("SharcQuery", commandList);

        nvrhi::BufferHandle  hashBuf     = renderGraph.GetBuffer(m_Sharc_HashEntries, RGResourceAccessMode::Read);
        nvrhi::BufferHandle  resBuf      = renderGraph.GetBuffer(m_Sharc_Resolved,    RGResourceAccessMode::Read);
        nvrhi::TextureHandle hdrColor    = renderGraph.GetTexture(g_RG_HDRColor,           RGResourceAccessMode::Write);
        nvrhi::TextureHandle bounceCount = (g_Renderer.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_SHARC_BOUNCE_HEATMAP)
            ? renderGraph.GetTexture(g_RG_SharcBounceCount, RGResourceAccessMode::Write)
            : CommonResources::GetInstance().DummyUAVTexture;
        nvrhi::TextureHandle cachedRad   = (g_Renderer.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_SHARC_CACHED_RADIANCE)
            ? renderGraph.GetTexture(g_RG_SharcCachedRadiance, RGResourceAccessMode::Write)
            : CommonResources::GetInstance().DummyUAVTexture;

        srrhi::SharcQueryInputs inputs;
        inputs.SetSharcCB(sharcCB);
        inputs.SetSceneAS(g_Renderer.m_Scene.m_TLAS);
        inputs.SetLights(g_Renderer.m_Scene.m_LightBuffer);
        inputs.SetInstances(g_Renderer.m_Scene.m_InstanceDataBuffer);
        inputs.SetMeshData(g_Renderer.m_Scene.m_MeshDataBuffer);
        inputs.SetMaterials(g_Renderer.m_Scene.m_MaterialConstantsBuffer);
        inputs.SetIndices(g_Renderer.m_Scene.m_IndexBuffer);
        inputs.SetVertices(g_Renderer.m_Scene.m_VertexBufferQuantized);
        inputs.SetHashEntries(hashBuf);
        inputs.SetDummyAccumulationBuffer(CommonResources::GetInstance().DummyUAVStructuredBuffer); // Not used by Query, but shader expects a valid UAV
        inputs.SetResolvedBuffer(resBuf);
        inputs.SetOutput(hdrColor, 0);
        inputs.SetBounceCountOutput(bounceCount, 0);
        inputs.SetCachedRadianceOutput(cachedRad, 0);

        const uint32_t dispW = DivideAndRoundUp(g_Renderer.m_RHI->m_SwapchainExtent.x, 8u);
        const uint32_t dispH = DivideAndRoundUp(g_Renderer.m_RHI->m_SwapchainExtent.y, 8u);

        g_Renderer.AddComputePass({
            .commandList    = commandList,
            .shaderID       = ShaderID::SHARCQUERY_SHARCQUERY_CSMAIN_HASH_GRID_ENABLE_64_BIT_ATOMICS_1_SHARC_QUERY_1,
            .bindingSetDesc = Renderer::CreateBindingSetDesc(inputs),
            .dispatchParams = { .x = dispW, .y = dispH, .z = 1 }
        });
    }
};

REGISTER_RENDERER(SharcRenderer);

class SharcDebugVisualizationRenderer : public IRenderer
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        if (g_Renderer.m_IndirectLightingTechnique != IndirectLightingTechnique::SHARC)
            return false;
        if (g_Renderer.m_DebugMode != srrhi::CommonConsts::DEBUG_MODE_SHARC_BOUNCE_HEATMAP &&
            g_Renderer.m_DebugMode != srrhi::CommonConsts::DEBUG_MODE_SHARC_HASH_GRID &&
            g_Renderer.m_DebugMode != srrhi::CommonConsts::DEBUG_MODE_SHARC_CACHED_RADIANCE)
            return false;

        if (g_Renderer.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_SHARC_BOUNCE_HEATMAP)
            renderGraph.ReadTexture(g_RG_SharcBounceCount);
        if (g_Renderer.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_SHARC_CACHED_RADIANCE)
            renderGraph.ReadTexture(g_RG_SharcCachedRadiance);
        renderGraph.WriteTexture(g_RG_HDRColor);
        renderGraph.ReadTexture(g_RG_DepthTexture);
        renderGraph.ReadTexture(g_RG_GBufferNormals);

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_FUNCTION();
        PROFILE_GPU_SCOPED("SharcDebugViz", commandList);

        nvrhi::BufferHandle  sharcCB      = BuildSharcCB(commandList);
        nvrhi::TextureHandle bounceCount  = (g_Renderer.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_SHARC_BOUNCE_HEATMAP)
            ? renderGraph.GetTexture(g_RG_SharcBounceCount, RGResourceAccessMode::Read)
            : CommonResources::GetInstance().DefaultTextureBlack;
        nvrhi::TextureHandle cachedRad    = (g_Renderer.m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_SHARC_CACHED_RADIANCE)
            ? renderGraph.GetTexture(g_RG_SharcCachedRadiance, RGResourceAccessMode::Read)
            : CommonResources::GetInstance().DefaultTextureBlack;
        nvrhi::TextureHandle hdrColor     = renderGraph.GetTexture(g_RG_HDRColor,           RGResourceAccessMode::Write);
        nvrhi::TextureHandle depth        = renderGraph.GetTexture(g_RG_DepthTexture,       RGResourceAccessMode::Read);
        nvrhi::TextureHandle normals      = renderGraph.GetTexture(g_RG_GBufferNormals,    RGResourceAccessMode::Read);

        srrhi::SharcDebugVizInputs inputs;
        inputs.SetSharcCB(sharcCB);
        inputs.SetBounceCountInput(bounceCount);
        inputs.SetCachedRadianceInput(cachedRad);
        inputs.SetHeatmapOutput(hdrColor, 0);
        inputs.SetDepth(depth);
        inputs.SetGBufferNormals(normals);

        const uint32_t dispW = DivideAndRoundUp(g_Renderer.m_RHI->m_SwapchainExtent.x, 8u);
        const uint32_t dispH = DivideAndRoundUp(g_Renderer.m_RHI->m_SwapchainExtent.y, 8u);

        g_Renderer.AddComputePass({
            .commandList    = commandList,
            .shaderID       = ShaderID::SHARCDEBUGVIZ_SHARCDEBUGVIZ_CSMAIN,
            .bindingSetDesc = Renderer::CreateBindingSetDesc(inputs),
            .dispatchParams = { .x = dispW, .y = dispH, .z = 1 }
        });
    }

    const char* GetName() const override { return "SharcDebugViz"; }
};

REGISTER_RENDERER(SharcDebugVisualizationRenderer);
