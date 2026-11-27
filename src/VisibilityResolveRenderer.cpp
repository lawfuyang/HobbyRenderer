#include "Renderer.h"
#include "CommonResources.h"
#include "BasePassCommon.h"
#include "Config.h"
#include "Utilities.h"

#include "shaders/srrhi/cpp/BasePass.h"
#include "shaders/srrhi/cpp/VisibilityResolve.h"

extern RGTextureHandle g_RG_VisibilityBuffer;
extern RGTextureHandle g_RG_GBufferAlbedo;
extern RGTextureHandle g_RG_GBufferNormals;
extern RGTextureHandle g_RG_GBufferGeoNormals;
extern RGTextureHandle g_RG_GBufferORM;
extern RGTextureHandle g_RG_GBufferEmissive;
extern RGTextureHandle g_RG_GBufferMotionVectors;

// Reads the ids the opaque cluster bucket rasterised and writes the six G-buffer targets from them. It binds the base
// pass' binding set, because it runs the base pass' material evaluation: the ids are the only thing this pass adds,
// and they live in their own register space so the two sets can sit side by side.
class VisibilityResolveRenderer : public IRenderer
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        if (!g_Renderer.IsVisibilityBufferActive())
            return false;

        renderGraph.ReadTexture(g_RG_VisibilityBuffer);

        // An id names a slot in the list the raster drew from, and one frame can hold ids from both opaque lists.
        renderGraph.ReadBuffer(g_RG_VisClusterJobLists[0]);
        renderGraph.ReadBuffer(g_RG_VisClusterJobLists[1]);

        // The six writes the raster pass no longer performs. Everything else the pass binds is scene-owned, a
        // placeholder, or a buffer it fills itself (the per-frame block).
        renderGraph.WriteTexture(g_RG_GBufferAlbedo);
        renderGraph.WriteTexture(g_RG_GBufferNormals);
        renderGraph.WriteTexture(g_RG_GBufferGeoNormals);
        renderGraph.WriteTexture(g_RG_GBufferORM);
        renderGraph.WriteTexture(g_RG_GBufferEmissive);
        renderGraph.WriteTexture(g_RG_GBufferMotionVectors);
        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_GPU_SCOPED("VisibilityResolve", commandList);

        nvrhi::DeviceHandle device = g_Renderer.m_RHI->m_NvrhiDevice;
        const CommonResources& common = CommonResources::GetInstance();
        const Scene& scene = g_Renderer.m_Scene;

        nvrhi::TextureHandle visibility = renderGraph.GetTexture(g_RG_VisibilityBuffer, RGResourceAccessMode::Read);
        nvrhi::TextureHandle albedo = renderGraph.GetTexture(g_RG_GBufferAlbedo, RGResourceAccessMode::Write);
        nvrhi::TextureHandle normals = renderGraph.GetTexture(g_RG_GBufferNormals, RGResourceAccessMode::Write);
        nvrhi::TextureHandle geoNormals = renderGraph.GetTexture(g_RG_GBufferGeoNormals, RGResourceAccessMode::Write);
        nvrhi::TextureHandle orm = renderGraph.GetTexture(g_RG_GBufferORM, RGResourceAccessMode::Write);
        nvrhi::TextureHandle emissive = renderGraph.GetTexture(g_RG_GBufferEmissive, RGResourceAccessMode::Write);
        nvrhi::TextureHandle motion = renderGraph.GetTexture(g_RG_GBufferMotionVectors, RGResourceAccessMode::Write);

        nvrhi::BufferHandle clusterJobs = renderGraph.GetBuffer(g_RG_VisClusterJobLists[0], RGResourceAccessMode::Read);
        nvrhi::BufferHandle clusterJobsSecond = renderGraph.GetBuffer(g_RG_VisClusterJobLists[1], RGResourceAccessMode::Read);

        const uint32_t width = g_Renderer.m_RHI->m_SwapchainExtent.x;
        const uint32_t height = g_Renderer.m_RHI->m_SwapchainExtent.y;

        // The per-frame block the base pass writes, so the material evaluation sees the same view, jitter, forced mip
        // and debug mode it would in the raster path. Zeroed first: the evaluation reads a subset of the fields, and
        // the rest would otherwise be whatever the volatile buffer happened to hold.
        const nvrhi::BufferHandle perFrameCB = device->createBuffer(
            nvrhi::utils::CreateVolatileConstantBufferDesc((uint32_t)sizeof(srrhi::BasePassConstants), "VisibilityResolveCB", 1));

        srrhi::BasePassConstants cb{};
        cb.SetView(scene.m_View);
        cb.SetPrevView(scene.m_ViewPrev);
        cb.SetDebugMode(uint32_t(Config::Get().m_DebugMode));
        cb.SetForcedTextureMip(g_Renderer.m_ForcedTextureMip);
        cb.SetWidth(width);
        cb.SetHeight(height);
        commandList->writeBuffer(perFrameCB, &cb, sizeof(cb), 0);

        // The base pass' binding set. The slots the G-buffer evaluation never reads (the meshlet path's buffers, the
        // HZB, the opaque colour) are placeholders, which is also what keeps them out of the render graph's checks.
        srrhi::BasePassInputs inputs;
        inputs.SetPerFrameCB(perFrameCB);
        inputs.SetInstances(scene.m_InstanceDataBuffer);
        inputs.SetMaterials(scene.m_MaterialConstantsBuffer);
        inputs.SetVertices(scene.m_VertexBufferQuantized);
        inputs.SetMeshlets(common.GetDummyStructuredSRV(sizeof(srrhi::Meshlet)));
        inputs.SetMeshletVertices(common.GetDummyStructuredSRV(sizeof(uint32_t)));
        inputs.SetMeshletTriangles(common.GetDummyStructuredSRV(sizeof(uint32_t)));
        inputs.SetMeshletJobs(common.GetDummyStructuredSRV(sizeof(srrhi::MeshletJob)));
        inputs.SetMeshData(scene.m_MeshDataBuffer);
        inputs.SetHZB(common.DefaultTextureBlack);
        inputs.SetSceneAS(scene.m_TLAS);
        inputs.SetIndices(scene.m_IndexBuffer);
        inputs.SetOpaqueColor(common.DefaultTextureBlack);
        inputs.SetLights(scene.m_LightBuffer);
        inputs.SetClusterJobs(clusterJobs);
        inputs.SetClusterRecords(scene.m_ClusterRecordBuffer);
        inputs.SetClusterStreamData(g_Renderer.m_GeometryStreaming->GetStreamDataBuffer());
        inputs.SetClusterGroups(scene.m_ClusterGroupBuffer);
        inputs.SetClusterGroupStates(g_Renderer.m_GeometryStreaming->GetGroupStateBuffer());
        inputs.SetClusterJobsSecond(clusterJobsSecond);

        // The ids, in their own register space.
        srrhi::VisibilityResolveInputs visInputs;
        visInputs.SetVisibilityBuffer(visibility);

        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(albedo);
        fbDesc.addColorAttachment(normals);
        fbDesc.addColorAttachment(orm);
        fbDesc.addColorAttachment(emissive);
        fbDesc.addColorAttachment(motion);
        fbDesc.addColorAttachment(geoNormals);
        const nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(fbDesc);

        Renderer::RenderPassParams params{
            .commandList = commandList,
            .shaderID = ShaderID::VISIBILITYRESOLVE_PSMAIN,
            .bindingSetDesc = Renderer::CreateBindingSetDesc(inputs),
            .framebuffer = framebuffer
        };
        params.additionalBindingSets.push_back({ Renderer::CreateBindingSetDesc(visInputs), 1 });

        g_Renderer.AddFullScreenPass(params);
    }

    const char* GetName() const override { return "Visibility Resolve"; }
};

REGISTER_RENDERER(VisibilityResolveRenderer);
