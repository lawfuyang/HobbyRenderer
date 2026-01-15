#include "Renderer.h"
#include "CommonResources.h"
#include "Camera.h"

// Enable ForwardLighting shared definitions for C++ side
#define FORWARD_LIGHTING_DEFINE
#include "shaders/ShaderShared.hlsl"

class BasePassRenderer : public IRenderer
{
public:
    bool Initialize() override;
    void Render(nvrhi::CommandListHandle commandList) override;
    const char* GetName() const override { return "BasePass"; }

private:
    nvrhi::InputLayoutHandle m_InputLayout;
};

REGISTER_RENDERER(BasePassRenderer);

bool BasePassRenderer::Initialize()
{
    Renderer* renderer = Renderer::GetInstance();

    // Create input layout matching shared VertexInput (pos, normal, uv)
    nvrhi::VertexAttributeDesc attributes[] = {
        { "POSITION", nvrhi::Format::RGB32_FLOAT,  1, 0, offsetof(VertexInput, m_Pos),    sizeof(VertexInput), false },
        { "NORMAL",   nvrhi::Format::RGB32_FLOAT,  1, 0, offsetof(VertexInput, m_Normal), sizeof(VertexInput), false },
        { "TEXCOORD0",nvrhi::Format::RG32_FLOAT,   1, 0, offsetof(VertexInput, m_Uv),     sizeof(VertexInput), false },
    };

    // Create input layout (vertexShader parameter unused for Vulkan backend)
    m_InputLayout = renderer->m_NvrhiDevice->createInputLayout(attributes, 3, nullptr);
    if (!m_InputLayout)
    {
        SDL_LOG_ASSERT_FAIL("ImGui input layout creation failed", "[BasePass] Failed to create input layout");
        return false;
    }

    return true;
}

void BasePassRenderer::Render(nvrhi::CommandListHandle commandList)
{
    Renderer* renderer = Renderer::GetInstance();

    // ============================================================================
    // Framebuffer Setup
    // ============================================================================
    nvrhi::TextureHandle rt = renderer->GetCurrentBackBufferTexture();
    nvrhi::TextureHandle depth = renderer->m_DepthTexture;
    nvrhi::FramebufferHandle framebuffer = renderer->m_NvrhiDevice->createFramebuffer(
        nvrhi::FramebufferDesc().addColorAttachment(rt).setDepthAttachment(depth));

    // ============================================================================
    // Graphics Pipeline Setup
    // ============================================================================
    nvrhi::GraphicsState state;
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.VS = renderer->GetShaderHandle("ForwardLighting_VSMain");
    pipelineDesc.PS = renderer->GetShaderHandle("ForwardLighting_PSMain");
    pipelineDesc.inputLayout = m_InputLayout;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.renderState.rasterState = CommonResources::GetInstance().RasterCullBack;
    pipelineDesc.renderState.blendState.targets[0] = CommonResources::GetInstance().BlendTargetOpaque;
    pipelineDesc.renderState.depthStencilState = CommonResources::GetInstance().DepthReadWrite;

    nvrhi::FramebufferInfoEx fbInfo;
    fbInfo.colorFormats = { renderer->m_RHI.VkFormatToNvrhiFormat(renderer->m_RHI.m_SwapchainFormat) };
    fbInfo.setDepthFormat(nvrhi::Format::D32);
    state.framebuffer = framebuffer;

    // ============================================================================
    // Constant Buffer Setup
    // ============================================================================
    nvrhi::BufferDesc cbd = nvrhi::utils::CreateVolatileConstantBufferDesc(
        (uint32_t)sizeof(PerFrameData), "PerFrameCB", 8);
    nvrhi::BufferHandle perFrameCB = renderer->m_NvrhiDevice->createBuffer(cbd);
    renderer->m_RHI.SetDebugName(perFrameCB, "PerFrameCB_frame");

    // ============================================================================
    // Vertex/Index Buffer and Viewport Setup
    // ============================================================================
    state.vertexBuffers = { nvrhi::VertexBufferBinding{ renderer->m_Scene.m_VertexBuffer, 0, 0 } };
    state.indexBuffer = nvrhi::IndexBufferBinding{
        renderer->m_Scene.m_IndexBuffer, nvrhi::Format::R32_UINT, 0 };

    uint32_t w = renderer->m_RHI.m_SwapchainExtent.width;
    uint32_t h = renderer->m_RHI.m_SwapchainExtent.height;
    state.viewport.viewports.push_back(nvrhi::Viewport(0.0f, (float)w, (float)h, 0.0f, 0.0f, 1.0f));
    state.viewport.scissorRects.resize(1);
    state.viewport.scissorRects[0].minX = 0;
    state.viewport.scissorRects[0].minY = 0;
    state.viewport.scissorRects[0].maxX = (int)w;
    state.viewport.scissorRects[0].maxY = (int)h;

    // ============================================================================
    // Instance Data Collection
    // ============================================================================
    std::vector<PerInstanceData> instances;
    std::vector<nvrhi::DrawIndexedIndirectArguments> indirectArgs;

    Camera* cam = &renderer->m_Camera;
    Matrix viewProj = cam->GetViewProjMatrix();
    Vector3 camPos = renderer->m_Camera.GetPosition();

    for (size_t ni = 0; ni < renderer->m_Scene.m_Nodes.size(); ++ni)
    {
        const Scene::Node& node = renderer->m_Scene.m_Nodes[ni];
        if (node.m_MeshIndex < 0) continue;

        const Scene::Mesh& mesh = renderer->m_Scene.m_Meshes[node.m_MeshIndex];

        for (const Scene::Primitive& prim : mesh.m_Primitives)
        {
            PerInstanceData inst{};
            inst.m_World = node.m_WorldTransform;
            inst.m_MaterialIndex = prim.m_MaterialIndex;

            instances.push_back(inst);

            nvrhi::DrawIndexedIndirectArguments args{};
            args.indexCount = prim.m_IndexCount;
            args.instanceCount = 1;
            args.startIndexLocation = prim.m_IndexOffset;
            args.baseVertexLocation = 0;
            args.startInstanceLocation = (uint32_t)instances.size() - 1;

            indirectArgs.push_back(args);
        }
    }

    // ============================================================================
    // Instance and Indirect Buffer Creation
    // ============================================================================
    nvrhi::BufferDesc instBufDesc = nvrhi::BufferDesc()
        .setByteSize(instances.size() * sizeof(PerInstanceData))
        .setStructStride(sizeof(PerInstanceData))
        .setInitialState(nvrhi::ResourceStates::ShaderResource)
        .setKeepInitialState(true);
    nvrhi::BufferHandle instanceBuffer = renderer->m_NvrhiDevice->createBuffer(instBufDesc);
    renderer->m_RHI.SetDebugName(instanceBuffer, "InstanceBuffer");
    commandList->writeBuffer(instanceBuffer, instances.data(), instances.size() * sizeof(PerInstanceData));

    nvrhi::BufferDesc indirectBufDesc = nvrhi::BufferDesc()
        .setByteSize(indirectArgs.size() * sizeof(nvrhi::DrawIndexedIndirectArguments))
        .setStructStride(sizeof(nvrhi::DrawIndexedIndirectArguments))
        .setIsDrawIndirectArgs(true)
        .setInitialState(nvrhi::ResourceStates::IndirectArgument)
        .setKeepInitialState(true);
    nvrhi::BufferHandle indirectBuffer = renderer->m_NvrhiDevice->createBuffer(indirectBufDesc);
    renderer->m_RHI.SetDebugName(indirectBuffer, "IndirectBuffer");
    commandList->writeBuffer(indirectBuffer, indirectArgs.data(),
        indirectArgs.size() * sizeof(nvrhi::DrawIndexedIndirectArguments));

    // ============================================================================
    // Binding Set Setup
    // ============================================================================
    nvrhi::BindingSetDesc bset;
    bset.bindings =
    {
        nvrhi::BindingSetItem::ConstantBuffer(0, perFrameCB),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, instanceBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(1, renderer->m_Scene.m_MaterialConstantsBuffer),
        nvrhi::BindingSetItem::Sampler(0, CommonResources::GetInstance().LinearWrap)
    };
    nvrhi::BindingLayoutHandle layout = renderer->GetOrCreateBindingLayoutFromBindingSetDesc(bset, nvrhi::ShaderType::All);
    pipelineDesc.bindingLayouts = { renderer->GetGlobalTextureBindingLayout(), layout };

    nvrhi::BindingSetHandle bindingSet = renderer->m_NvrhiDevice->createBindingSet(bset, layout);
    state.bindings = { renderer->GetGlobalTextureDescriptorTable(), bindingSet };

    nvrhi::GraphicsPipelineHandle pipeline = renderer->GetOrCreateGraphicsPipeline(pipelineDesc, fbInfo);
    state.pipeline = pipeline;

    // ============================================================================
    // Per-Frame Constants and Rendering
    // ============================================================================
    PerFrameData cb{};
    cb.m_ViewProj = viewProj;
    cb.m_CameraPos = Vector4{ camPos.x, camPos.y, camPos.z, 0.0f };
    cb.m_LightDirection = renderer->GetDirectionalLightDirection();
    cb.m_LightIntensity = renderer->m_DirectionalLight.intensity / 10000.0f;
    commandList->writeBuffer(perFrameCB, &cb, sizeof(cb), 0);

    state.indirectParams = indirectBuffer;

    commandList->setGraphicsState(state);
    commandList->drawIndexedIndirect(0, (uint32_t)indirectArgs.size());
}
