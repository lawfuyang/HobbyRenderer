/*
	The compute software raster (docs/visibility_buffer_implementation_plan.md, phase 7), in two passes: a kernel
	that finds the samples the routed clusters cover and appends a stamp for each, and a replay that hands those
	stamps back to the hardware as one-pixel points.

	Off by default, on purpose. It is correct - routing 78 % of an animation scene's clusters still matches the mesh
	path to one pixel in two million - but it is slower: +5.7 % GPU time at a 2 px routing threshold, +15 % at 8 px,
	a wash on the textured scene. The waste it removes is helper lanes, and the id raster has almost none: its pixel
	shader is a single uint store (31 M of Bistro's 8,192 M instructions), while this path adds a dispatch, a barrier
	and a per-sample atomic append. See Config::m_bComputeRaster for the numbers.
*/

#include "Renderer.h"
#include "CommonResources.h"
#include "BasePassCommon.h"
#include "Config.h"
#include "Utilities.h"
#include "shaders/srrhi/cpp/BasePass.h"
#include "shaders/srrhi/cpp/ClusterComputeRaster.h"
#include "shaders/srrhi/cpp/StampRaster.h"

extern RGTextureHandle g_RG_VisibilityBuffer;
extern RGTextureHandle g_RG_DepthTexture;

// The compute software raster's two shared transients: the stamp list the kernel appends and the indirect draw
// arguments that replay it. The list is declared by the dispatch pass and read by the replay, so the render graph
// puts the barrier between them.
RGBufferHandle g_RG_ComputeRasterStamps;
RGBufferHandle g_RG_ComputeRasterStampArgs;

namespace
{
    // Must match kMaxStamps in ClusterComputeRaster.hlsl.
    constexpr uint32_t kMaxStamps = 1u << 20;

    // The indirect argument block: four DRAW arguments, then the kernel's counters (dropped stamps and padding).
    constexpr uint32_t kStampArgWords = 8;
    constexpr uint32_t kVisListIndexCompute = 3;

    // The per-frame block the base pass writes, so the software raster decodes the same view and names its ids with
    // the same list index the resolve will read them back through.
    nvrhi::BufferHandle WritePerFrameConstants(nvrhi::CommandListHandle commandList)
    {
        const nvrhi::BufferHandle perFrameCB = g_Renderer.m_RHI->m_NvrhiDevice->createBuffer(
            nvrhi::utils::CreateVolatileConstantBufferDesc((uint32_t)sizeof(srrhi::BasePassConstants), "ComputeRasterCB", 1));

        srrhi::BasePassConstants cb{};
        cb.SetView(g_Renderer.m_Scene.m_View);
        cb.SetPrevView(g_Renderer.m_Scene.m_ViewPrev);
        cb.SetDebugMode(uint32_t(Config::Get().m_DebugMode));
        cb.SetForcedTextureMip(g_Renderer.m_ForcedTextureMip);
        cb.SetWidth(g_Renderer.m_RHI->m_SwapchainExtent.x);
        cb.SetHeight(g_Renderer.m_RHI->m_SwapchainExtent.y);
        cb.SetVisListIndex(kVisListIndexCompute);
        commandList->writeBuffer(perFrameCB, &cb, sizeof(cb), 0);
        return perFrameCB;
    }

    // The base pass' binding set with the compute bucket's job list in the job slot. Everything the decode does not
    // read is a placeholder, exactly as in the resolve.
    srrhi::BasePassInputs MakeGeometryInputs(nvrhi::BufferHandle perFrameCB, nvrhi::BufferHandle clusterJobs)
    {
        const CommonResources& common = CommonResources::GetInstance();
        const Scene& scene = g_Renderer.m_Scene;

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
        inputs.SetClusterJobsSecond(common.GetDummyStructuredSRV(sizeof(srrhi::ClusterJob)));
        inputs.SetClusterJobsMasked(common.GetDummyStructuredSRV(sizeof(srrhi::ClusterJob)));
        return inputs;
    }
}

// The dispatch: one thread group per cluster the cull routed here, each walking its cluster's triangles and
// appending the samples it covers. The grid is the traversal's own argument slot, so nothing about the count
// reaches the CPU.
class ComputeRasterRenderer : public IRenderer
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        if (!g_Renderer.IsComputeRasterActive())
            return false;

        renderGraph.ReadBuffer(g_RG_VisClusterJobLists[kVisListIndexCompute]);
        renderGraph.ReadBuffer(g_RG_ClusterIndirectArgs);

        RGBufferDesc stamps;
        stamps.m_NvrhiDesc.setByteSize(kMaxStamps * sizeof(uint32_t) * 4u)
            .setStructStride(sizeof(uint32_t) * 4u)
            .setCanHaveUAVs(true)
            .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
            .setKeepInitialState(true)
            .setDebugName("ComputeRasterStamps_RG");
        renderGraph.DeclareBuffer(stamps, g_RG_ComputeRasterStamps);

        RGBufferDesc args;
        args.m_NvrhiDesc.setByteSize(kStampArgWords * sizeof(uint32_t))
            .setStructStride(sizeof(uint32_t))
            .setIsDrawIndirectArgs(true)
            .setCanHaveUAVs(true)
            .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
            .setKeepInitialState(true)
            .setDebugName("ComputeRasterStampArgs_RG");
        renderGraph.DeclareBuffer(args, g_RG_ComputeRasterStampArgs);
        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_GPU_SCOPED("Compute Raster", commandList);

        const nvrhi::BufferHandle clusterJobs = renderGraph.GetBuffer(g_RG_VisClusterJobLists[kVisListIndexCompute],
                                                                     RGResourceAccessMode::Read);
        const nvrhi::BufferHandle indirectArgs = renderGraph.GetBuffer(g_RG_ClusterIndirectArgs, RGResourceAccessMode::Read);
        const nvrhi::BufferHandle stamps = renderGraph.GetBuffer(g_RG_ComputeRasterStamps, RGResourceAccessMode::Write);
        const nvrhi::BufferHandle stampArgs = renderGraph.GetBuffer(g_RG_ComputeRasterStampArgs, RGResourceAccessMode::Write);

        // The draw the replay will issue: one vertex per instance, and an instance count the kernel appends to.
        // Writing the whole block also resets last frame's counts, which is why there is no separate clear.
        const uint32_t stampedArgs[kStampArgWords] = { 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
        commandList->writeBuffer(stampArgs, stampedArgs, sizeof(stampedArgs), 0);

        const nvrhi::BufferHandle perFrameCB = WritePerFrameConstants(commandList);
        srrhi::BasePassInputs inputs = MakeGeometryInputs(perFrameCB, clusterJobs);

        srrhi::ClusterComputeRasterInputs rasterInputs;
        rasterInputs.SetStamps(stamps);
        rasterInputs.SetStampArgs(stampArgs);

        Renderer::RenderPassParams params{
            .commandList = commandList,
            .shaderID = ShaderID::CLUSTERCOMPUTERASTER_CSMAIN,
            .bindingSetDesc = Renderer::CreateBindingSetDesc(inputs),
            .dispatchParams = Renderer::ComputeDispatchParams{
                .indirectBuffer = indirectArgs,
                .indirectOffsetBytes = kClusterIndirectSlotComputeRaster * uint32_t(sizeof(srrhi::DispatchIndirectArguments))
            }
        };
        params.additionalBindingSets.push_back({ Renderer::CreateBindingSetDesc(rasterInputs), 1 });

        g_Renderer.AddComputePass(params);
    }

    const char* GetName() const override { return "Compute Raster"; }
};

// The replay: every stamp becomes a one-pixel point, depth-tested and depth-writing, so the hardware decides the
// final order against everything the mesh shader drew - and against other stamps, which it can resolve in any
// order. The pixel shader writes the id, so the resolve cannot tell the two rasterisers apart.
class ComputeStampRenderer : public IRenderer
{
public:
    bool Setup(RenderGraph& renderGraph) override
    {
        if (!g_Renderer.IsComputeRasterActive())
            return false;

        renderGraph.ReadBuffer(g_RG_ComputeRasterStamps);
        renderGraph.ReadBuffer(g_RG_ComputeRasterStampArgs);
        renderGraph.ReadTexture(g_RG_DepthTexture);
        renderGraph.WriteTexture(g_RG_DepthTexture);
        renderGraph.WriteTexture(g_RG_VisibilityBuffer);
        return true;
    }

    void Render(nvrhi::CommandListHandle commandList, const RenderGraph& renderGraph) override
    {
        PROFILE_GPU_SCOPED("Compute Stamp", commandList);

        const nvrhi::BufferHandle stamps = renderGraph.GetBuffer(g_RG_ComputeRasterStamps, RGResourceAccessMode::Read);
        const nvrhi::BufferHandle stampArgs = renderGraph.GetBuffer(g_RG_ComputeRasterStampArgs, RGResourceAccessMode::Read);
        const nvrhi::TextureHandle visibility = renderGraph.GetTexture(g_RG_VisibilityBuffer, RGResourceAccessMode::Write);
        const nvrhi::TextureHandle depth = renderGraph.GetTexture(g_RG_DepthTexture, RGResourceAccessMode::Write);

        nvrhi::FramebufferDesc fbDesc;
        fbDesc.addColorAttachment(visibility);
        fbDesc.setDepthAttachment(depth);
        const nvrhi::FramebufferHandle framebuffer = g_Renderer.m_RHI->m_NvrhiDevice->createFramebuffer(fbDesc);

        srrhi::StampRasterInputs inputs;
        inputs.m_Constants.SetWidth(g_Renderer.m_RHI->m_SwapchainExtent.x);
        inputs.m_Constants.SetHeight(g_Renderer.m_RHI->m_SwapchainExtent.y);
        inputs.SetStamps(stamps);

        const nvrhi::BindingSetDesc bindingSetDesc = Renderer::CreateBindingSetDesc(inputs);
        const nvrhi::BindingLayoutHandle layout =
            g_Renderer.GetOrCreateBindingLayoutFromBindingSetDesc(bindingSetDesc);
        const nvrhi::BindingSetHandle bindingSet = g_Renderer.m_RHI->m_NvrhiDevice->createBindingSet(bindingSetDesc, layout);

        // The base pass' own depth state for opaque geometry, so a stamp leaves the stencil coverage mask and the
        // depth buffer exactly as a rasterised fragment would have.
        nvrhi::RenderState renderState;
        renderState.rasterState = CommonResources::GetInstance().RasterCullNone;
        renderState.blendState.targets[0] = CommonResources::GetInstance().BlendTargetOpaque;
        renderState.depthStencilState = CommonResources::GetInstance().DepthReadWrite;
        renderState.depthStencilState.stencilEnable = true;
        renderState.depthStencilState.frontFaceStencil.passOp = nvrhi::StencilOp::Replace;
        renderState.depthStencilState.frontFaceStencil.stencilFunc = nvrhi::ComparisonFunc::Always;
        renderState.depthStencilState.backFaceStencil = renderState.depthStencilState.frontFaceStencil;
        renderState.depthStencilState.stencilRefValue = 1;

        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.VS = g_Renderer.GetShaderHandle(ShaderID::STAMPRASTER_VSMAIN);
        pipelineDesc.PS = g_Renderer.GetShaderHandle(ShaderID::STAMPRASTER_PSMAIN);
        pipelineDesc.primType = nvrhi::PrimitiveType::PointList;
        pipelineDesc.renderState = renderState;
        pipelineDesc.bindingLayouts = { layout };

        nvrhi::GraphicsState state;
        state.framebuffer = framebuffer;
        state.bindings = { bindingSet };
        state.pipeline = g_Renderer.GetOrCreateGraphicsPipeline(pipelineDesc, framebuffer->getFramebufferInfo());
        state.indirectParams = stampArgs;

        const nvrhi::TextureDesc& idDesc = framebuffer->getDesc().colorAttachments[0].texture->getDesc();
        const nvrhi::Viewport viewport(0.0f, float(idDesc.width), 0.0f, float(idDesc.height), 0.0f, 1.0f);
        state.viewport.viewports.push_back(viewport);
        state.viewport.scissorRects.push_back(nvrhi::Rect((int)viewport.minX, (int)viewport.maxX,
                                                          (int)viewport.minY, (int)viewport.maxY));

        commandList->setGraphicsState(state);
        commandList->setPushConstants(&inputs.m_Constants, srrhi::StampRasterInputs::PushConstantBytes);
        // One draw read from the argument block: the instance count is the number of stamps the kernel appended.
        commandList->drawIndirect(0, 1);
    }

    const char* GetName() const override { return "Compute Stamp"; }
};

REGISTER_RENDERER(ComputeRasterRenderer);
REGISTER_RENDERER(ComputeStampRenderer);
