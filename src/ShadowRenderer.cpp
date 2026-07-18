#include "Renderer.h"
#include "BasePassCommon.h"
#include "CommonResources.h"
#include "Camera.h"

#include "shaders/srrhi/cpp/ShadowDepth.h"
#include "shaders/srrhi/cpp/GPUCulling.h"

// ---------------------------------------------------------------------------
// Render Graph handle — defined here, extern'd by ShadowMaskRenderer and
// CSMDebugRenderer.
// ---------------------------------------------------------------------------
RGTextureHandle g_RG_CSMShadowMap;

// ---------------------------------------------------------------------------
// ShadowRenderer — 4-cascade CSM depth array (4 × 2048² D32_FLOAT)
// ---------------------------------------------------------------------------
class ShadowRenderer : public IRenderer
{
public:
    void Initialize()
    {
        m_OpaqueResources.Initialize();
        m_MaskedResources.Initialize();
    }

    bool Setup(RenderGraph& renderGraph) override
    {
        if (g_Renderer.m_Mode != RenderingMode::NormalBasic || !g_Renderer.m_EnableCSMShadows)
            return false;

        // Declare the CSM shadow map texture array
        RGTextureDesc shadowMapDesc;
        shadowMapDesc.m_NvrhiDesc.dimension  = nvrhi::TextureDimension::Texture2DArray;
        shadowMapDesc.m_NvrhiDesc.width      = srrhi::CommonConsts::kShadowMapResolution;
        shadowMapDesc.m_NvrhiDesc.height     = srrhi::CommonConsts::kShadowMapResolution;
        shadowMapDesc.m_NvrhiDesc.arraySize  = g_Renderer.m_NumCSMCascades;
        shadowMapDesc.m_NvrhiDesc.format     = nvrhi::Format::D32;
        shadowMapDesc.m_NvrhiDesc.isRenderTarget = true;
        shadowMapDesc.m_NvrhiDesc.debugName  = "CSMShadowMap_RG";
        shadowMapDesc.m_NvrhiDesc.initialState = nvrhi::ResourceStates::DepthWrite;
        shadowMapDesc.m_NvrhiDesc.keepInitialState = true;
        shadowMapDesc.m_NvrhiDesc.setClearValue(nvrhi::Color{ 1.0f, 0.0f, 0.0f, 0.0f }); // standard depth: far=1.0
        renderGraph.DeclareTexture(shadowMapDesc, g_RG_CSMShadowMap);

        // Declare GPU culling buffers for opaque and masked buckets
        m_OpaqueResources.DeclareResources(renderGraph, "Shadow_Opaque");
        m_MaskedResources.DeclareResources(renderGraph, "Shadow_Masked");

        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        nvrhi::TextureHandle shadowMap = renderGraph.GetTexture(g_RG_CSMShadowMap, RGResourceAccessMode::Write);

        commandList->clearDepthStencilTexture(shadowMap, nvrhi::AllSubresources, true, 1.0f, false, 0); // standard depth: clear to far

        // Resolve RG buffer handles once for both buckets — reused across all cascade iterations
        BucketHandles opaque = ResolveWrite(renderGraph, m_OpaqueResources);
        BucketHandles masked = ResolveWrite(renderGraph, m_MaskedResources);

        for (uint32_t i = 0; i < g_Renderer.m_NumCSMCascades; i++)
        {
            RenderCascade(i, commandList, shadowMap, opaque, masked);
        }
    }

    const char* GetName() const override { return "Shadow (CSM)"; }

private:
    BasePassResources m_OpaqueResources;
    BasePassResources m_MaskedResources;

    struct BucketHandles
    {
        nvrhi::BufferHandle visibleIndirect;
        nvrhi::BufferHandle visibleCount;
        nvrhi::BufferHandle meshletJobs;
        nvrhi::BufferHandle meshletJobCount;
        nvrhi::BufferHandle meshletIndirect;
    };

    static BucketHandles ResolveWrite(const RenderGraph& renderGraph, BasePassResources& res)
    {
        return {
            renderGraph.GetBuffer(res.m_VisibleIndirectBuffer,  RGResourceAccessMode::Write),
            renderGraph.GetBuffer(res.m_VisibleCountBuffer,     RGResourceAccessMode::Write),
            renderGraph.GetBuffer(res.m_MeshletJobBuffer,       RGResourceAccessMode::Write),
            renderGraph.GetBuffer(res.m_MeshletJobCountBuffer,  RGResourceAccessMode::Write),
            renderGraph.GetBuffer(res.m_MeshletIndirectBuffer,  RGResourceAccessMode::Write),
        };
    }

    // -----------------------------------------------------------------------
    // Render one cascade slice into the shadow map array
    // -----------------------------------------------------------------------
    void RenderCascade(uint32_t cascadeIndex, nvrhi::CommandListHandle commandList, nvrhi::TextureHandle shadowMap, const BucketHandles& opaque, const BucketHandles& masked)
    {
        PROFILE_FUNCTION();

        char marker[64]{};
        snprintf(marker, sizeof(marker), "Shadow Cascade %u", cascadeIndex);
        PROFILE_GPU_SCOPED(marker, commandList);

        nvrhi::DeviceHandle device = g_Renderer.m_RHI->m_NvrhiDevice;

        // Build ShadowDepthCB
        const nvrhi::BufferDesc cbDesc = nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(srrhi::ShadowDepthConstants), "ShadowDepthCB", 1);
        const nvrhi::BufferHandle shadowDepthCB = device->createBuffer(cbDesc);

        srrhi::ShadowDepthConstants cb;
        cb.SetShadowViewProj(g_Renderer.m_CSMCascades[cascadeIndex].m_ViewProj);
        cb.SetCascadeIndex(cascadeIndex);
        commandList->writeBuffer(shadowDepthCB, &cb, sizeof(cb), 0);

        // Compute axis-aligned frustum planes in light view space from the cascade AABB.
        // These planes have inward-facing normals, matching FrustumSphereTest convention.
        using namespace DirectX;
        const Vector3& aabbMin = g_Renderer.m_CSMCascades[cascadeIndex].m_LightAABBMin;
        const Vector3& aabbMax = g_Renderer.m_CSMCascades[cascadeIndex].m_LightAABBMax;

        Vector4 frustumPlanes[5];
        XMStoreFloat4(&frustumPlanes[0], XMVectorSet( 1.0f,  0.0f,  0.0f, -aabbMin.x)); // Left:  x >= min
        XMStoreFloat4(&frustumPlanes[1], XMVectorSet(-1.0f,  0.0f,  0.0f,  aabbMax.x)); // Right: x <= max
        XMStoreFloat4(&frustumPlanes[2], XMVectorSet( 0.0f,  1.0f,  0.0f, -aabbMin.y)); // Bottom: y >= min
        XMStoreFloat4(&frustumPlanes[3], XMVectorSet( 0.0f, -1.0f,  0.0f,  aabbMax.y)); // Top: y <= max
        XMStoreFloat4(&frustumPlanes[4], XMVectorSet( 0.0f,  0.0f,  1.0f, -aabbMin.z)); // Near: z >= min

        // Cull and draw both buckets
        CullAndDraw(cascadeIndex, commandList, shadowMap, shadowDepthCB, frustumPlanes, opaque, /*bAlphaTest=*/false);
        CullAndDraw(cascadeIndex, commandList, shadowMap, shadowDepthCB, frustumPlanes, masked, /*bAlphaTest=*/true);
    }

    // -----------------------------------------------------------------------
    // GPU cull one bucket then issue the meshlet depth draw for one cascade
    // -----------------------------------------------------------------------
    void CullAndDraw(uint32_t cascadeIndex,
                     nvrhi::CommandListHandle commandList,
                     nvrhi::TextureHandle shadowMap,
                     nvrhi::BufferHandle shadowDepthCB,
                     const Vector4 frustumPlanes[5],
                     const BucketHandles& h,
                     bool bAlphaTest)
    {
        nvrhi::DeviceHandle device = g_Renderer.m_RHI->m_NvrhiDevice;
        const uint32_t numInstances = (uint32_t)g_Renderer.m_Scene.m_InstanceData.size();

        // ---- GPU Culling ----
        commandList->clearBufferUInt(h.visibleCount,     0);
        commandList->clearBufferUInt(h.meshletJobCount,  0);

        const nvrhi::BufferDesc cullCBDesc = nvrhi::utils::CreateVolatileConstantBufferDesc(
            sizeof(srrhi::CullingConstants), "ShadowCullCB", 1);
        const nvrhi::BufferHandle cullCB = device->createBuffer(cullCBDesc);

        srrhi::CullingConstants cullData;
        cullData.SetNumPrimitives(numInstances);
        cullData.SetFrustumPlanes(reinterpret_cast<const DirectX::XMFLOAT4*>(frustumPlanes));
        // Use the light view matrix so sphere centers are transformed to light view space,
        // matching the view-space frustum planes derived from the light projection matrix.
        cullData.SetView(g_Renderer.m_CSMCascades[cascadeIndex].m_View);
        cullData.SetViewProj(g_Renderer.m_CSMCascades[cascadeIndex].m_ViewProj);
        cullData.SetEnableFrustumCulling(g_Renderer.m_EnableFrustumCulling ? 1 : 0);
        cullData.SetEnableOcclusionCulling(0);
        cullData.SetHZBWidth(0);
        cullData.SetHZBHeight(0);
        cullData.SetPhase(0);
        cullData.SetUseMeshletRendering(g_Renderer.m_UseMeshletRendering ? 1 : 0);
        // P00/P11 are not used (occlusion culling is disabled for shadows), set to 0.
        cullData.SetP00(0.0f);
        cullData.SetP11(0.0f);
        cullData.SetForcedLOD(g_Renderer.m_ForcedLOD);
        cullData.SetInstanceBaseIndex(0);
        commandList->writeBuffer(cullCB, &cullData, sizeof(cullData), 0);

        srrhi::GPUCullingInputs cullInputs;
        cullInputs.SetCullingCB(cullCB);
        cullInputs.SetInstanceData(g_Renderer.m_Scene.m_InstanceDataBuffer);
        cullInputs.SetHZB(CommonResources::GetInstance().DefaultTextureBlack);
        cullInputs.SetMeshData(g_Renderer.m_Scene.m_MeshDataBuffer);
        cullInputs.SetVisibleArgs(h.visibleIndirect);
        cullInputs.SetVisibleCount(h.visibleCount);
        cullInputs.SetOccludedIndices(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        cullInputs.SetOccludedCount(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        cullInputs.SetDispatchIndirectArgs(CommonResources::GetInstance().DummyUAVStructuredBuffer);
        cullInputs.SetMeshletJobs(h.meshletJobs);
        cullInputs.SetMeshletJobCount(h.meshletJobCount);
        cullInputs.SetMeshletIndirectArgs(h.meshletIndirect);
        cullInputs.SetInstanceLOD(g_Renderer.m_Scene.m_InstanceLODBuffer);

        nvrhi::BindingSetDesc cullBset = Renderer::CreateBindingSetDesc(cullInputs);
        const uint32_t dispatchX = DivideAndRoundUp(numInstances, srrhi::CommonConsts::kThreadsPerGroup);

        {
            Renderer::RenderPassParams params;
            params.commandList    = commandList;
            params.shaderID       = ShaderID::GPUCULLING_CULLING_CSMAIN;
            params.bindingSetDesc = cullBset;
            params.dispatchParams = { .x = dispatchX, .y = 1, .z = 1 };
            g_Renderer.AddComputePass(params);
        }
        {
            Renderer::RenderPassParams params;
            params.commandList    = commandList;
            params.shaderID       = ShaderID::GPUCULLING_BUILDINDIRECT_CSMAIN;
            params.bindingSetDesc = cullBset;
            params.dispatchParams = { .x = 1, .y = 1, .z = 1 };
            g_Renderer.AddComputePass(params);
        }

        // ---- Depth-only draw ----
        DrawShadowMeshlets(cascadeIndex, commandList, shadowMap, shadowDepthCB, h.meshletJobs, h.meshletJobCount, h.meshletIndirect, bAlphaTest);
    }

    // -----------------------------------------------------------------------
    // Issue the meshlet depth draw for one cascade bucket
    // -----------------------------------------------------------------------
    void DrawShadowMeshlets(uint32_t cascadeIndex,
                            nvrhi::CommandListHandle commandList,
                            nvrhi::TextureHandle shadowMap,
                            nvrhi::BufferHandle shadowDepthCB,
                            nvrhi::BufferHandle meshletJobs,
                            nvrhi::BufferHandle meshletJobCount,
                            nvrhi::BufferHandle meshletIndirect,
                            bool bAlphaTest)
    {
        nvrhi::DeviceHandle device = g_Renderer.m_RHI->m_NvrhiDevice;
        const uint32_t resolution  = srrhi::CommonConsts::kShadowMapResolution;

        // Depth-only framebuffer targeting cascade slice
        nvrhi::FramebufferDesc fbDesc;
        fbDesc.setDepthAttachment(
            nvrhi::FramebufferAttachment()
                .setTexture(shadowMap)
                .setArraySlice(cascadeIndex)
                .setMipLevel(0));
        nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(fbDesc);

        nvrhi::FramebufferInfoEx fbInfo;
        fbInfo.setDepthFormat(nvrhi::Format::D32);

        nvrhi::ViewportState viewportState;
        viewportState.viewports.push_back(nvrhi::Viewport(0.0f, (float)resolution, 0.0f, (float)resolution, 0.0f, 1.0f));
        viewportState.scissorRects.resize(1);
        viewportState.scissorRects[0] = { 0, 0, (int)resolution, (int)resolution };

        // Build binding set
        srrhi::ShadowDepthInputs inputs;
        inputs.SetShadowDepthCB(shadowDepthCB);
        inputs.SetInstances(g_Renderer.m_Scene.m_InstanceDataBuffer);
        inputs.SetMaterials(g_Renderer.m_Scene.m_MaterialConstantsBuffer);
        inputs.SetVertices(g_Renderer.m_Scene.m_VertexBufferQuantized);
        inputs.SetMeshlets(g_Renderer.m_Scene.m_MeshletBuffer);
        inputs.SetMeshletVertices(g_Renderer.m_Scene.m_MeshletVerticesBuffer);
        inputs.SetMeshletTriangles(g_Renderer.m_Scene.m_MeshletTrianglesBuffer);
        inputs.SetMeshletJobs(meshletJobs);
        inputs.SetMeshData(g_Renderer.m_Scene.m_MeshDataBuffer);

        nvrhi::BindingSetDesc bset = Renderer::CreateBindingSetDesc(inputs);
        const nvrhi::BindingLayoutHandle layout = g_Renderer.GetOrCreateBindingLayoutFromBindingSetDesc(bset);
        const nvrhi::BindingSetHandle bindingSet = device->createBindingSet(bset, layout);

        // Pipeline state — depth-only, standard depth, back-face cull
        nvrhi::RenderState renderState;
        renderState.rasterState       = CommonResources::GetInstance().RasterCullBack;
        renderState.depthStencilState = CommonResources::GetInstance().DepthReadWrite;
        renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual; // standard depth: keep closest

        const uint32_t msID = bAlphaTest
            ? ShaderID::SHADOWDEPTH_SHADOWDEPTH_MSMAIN_ALPHATEST_SHADOW_ALPHA_TEST_1
            : ShaderID::SHADOWDEPTH_SHADOWDEPTH_MSMAIN;
        const uint32_t asID = ShaderID::SHADOWDEPTH_SHADOWDEPTH_ASMAIN;
        const uint32_t psID = bAlphaTest
            ? ShaderID::SHADOWDEPTH_SHADOWDEPTH_ALPHATEST_PSMAIN_ALPHATEST_PS_SHADOW_ALPHA_TEST_1
            : UINT32_MAX; // null PS for opaque
        nvrhi::MeshletPipelineDesc meshPipelineDesc;
        meshPipelineDesc.AS = g_Renderer.GetShaderHandle(asID);
        meshPipelineDesc.MS = g_Renderer.GetShaderHandle(msID);
        meshPipelineDesc.PS = (psID != UINT32_MAX) ? g_Renderer.GetShaderHandle(psID) : nullptr;
        meshPipelineDesc.renderState    = renderState;
        meshPipelineDesc.bindingLayouts = { layout, g_Renderer.GetStaticTextureBindingLayout(), g_Renderer.GetStaticSamplerBindingLayout() };
        meshPipelineDesc.useDrawIndex = true;

        const nvrhi::MeshletPipelineHandle meshPipeline = g_Renderer.GetOrCreateMeshletPipeline(meshPipelineDesc, fbInfo);

        nvrhi::MeshletState meshState;
        meshState.framebuffer         = framebuffer;
        meshState.pipeline            = meshPipeline;
        meshState.bindings            = { bindingSet, g_Renderer.GetStaticTextureDescriptorTable(), g_Renderer.GetStaticSamplerDescriptorTable() };
        meshState.viewport            = viewportState;
        meshState.indirectParams      = meshletIndirect;
        meshState.indirectCountBuffer = meshletJobCount;

        commandList->setMeshletState(meshState);
        commandList->dispatchMeshIndirectCount(0, 0, (uint32_t)g_Renderer.m_Scene.m_InstanceData.size());
    }
};

REGISTER_RENDERER(ShadowRenderer);
