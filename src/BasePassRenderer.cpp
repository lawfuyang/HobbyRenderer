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

// Register the BasePassRenderer
static bool s_BasePassRendererRegistered = []() {
    RendererRegistry::RegisterRenderer([]() {
        return std::make_shared<BasePassRenderer>();
    });
    return true;
}();

bool BasePassRenderer::Initialize()
{
    Renderer* renderer = Renderer::GetInstance();

    // Create input layout matching shared VertexInput (pos, normal, uv)
    nvrhi::VertexAttributeDesc attributes[] = {
        { "POSITION", nvrhi::Format::RGB32_FLOAT,  1, 0, offsetof(VertexInput, pos),    sizeof(VertexInput), false },
        { "NORMAL",   nvrhi::Format::RGB32_FLOAT,  1, 0, offsetof(VertexInput, normal), sizeof(VertexInput), false },
        { "TEXCOORD0",nvrhi::Format::RG32_FLOAT,   1, 0, offsetof(VertexInput, uv),     sizeof(VertexInput), false },
    };

    // Create input layout (vertexShader parameter unused for Vulkan backend)
    m_InputLayout = renderer->m_NvrhiDevice->createInputLayout(attributes, 3, nullptr);
    if (!m_InputLayout)
    {
        SDL_Log("[BasePass] Failed to create input layout");
        SDL_assert(false);
        return false;
    }

    return true;
}

void BasePassRenderer::Render(nvrhi::CommandListHandle commandList)
{
    Renderer* renderer = Renderer::GetInstance();
    
    // Framebuffer
    nvrhi::TextureHandle rt = renderer->GetCurrentBackBufferTexture();
    nvrhi::TextureHandle depth = renderer->m_DepthTexture;
    nvrhi::FramebufferHandle framebuffer = renderer->m_NvrhiDevice->createFramebuffer(
        nvrhi::FramebufferDesc().addColorAttachment(rt).setDepthAttachment(depth)
    );

    // Prepare graphics state
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

    // Create a volatile constant buffer for this frame (nvrhi will suballocate versions as needed)
    nvrhi::BufferDesc cbd = nvrhi::utils::CreateVolatileConstantBufferDesc((uint32_t)sizeof(PerObjectData), "PerObjectCB", 8);
    nvrhi::BufferHandle perFrameCB = renderer->m_NvrhiDevice->createBuffer(cbd);
    renderer->m_RHI.SetDebugName(perFrameCB, "PerObjectCB_frame");

    // Create a single BindingSetDesc that references the volatile CB and derive the binding layout from it
    nvrhi::BindingSetDesc bset;
    bset.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, perFrameCB) };
    nvrhi::BindingLayoutHandle layout = renderer->GetOrCreateBindingLayoutFromBindingSetDesc(bset, nvrhi::ShaderType::All);
    pipelineDesc.bindingLayouts = { layout };

    nvrhi::BindingSetHandle bindingSet = renderer->m_NvrhiDevice->createBindingSet(bset, layout);
    state.bindings = { bindingSet };

    nvrhi::GraphicsPipelineHandle pipeline = renderer->GetOrCreateGraphicsPipeline(pipelineDesc, fbInfo);
    state.pipeline = pipeline;

    // Set index/vertex buffers common for all draws
    state.vertexBuffers = { nvrhi::VertexBufferBinding{ renderer->m_Scene.m_VertexBuffer, 0, 0 } };
    state.indexBuffer = nvrhi::IndexBufferBinding{ renderer->m_Scene.m_IndexBuffer, nvrhi::Format::R32_UINT, 0 };

    // Viewport
    uint32_t w = renderer->m_RHI.m_SwapchainExtent.width;
    uint32_t h = renderer->m_RHI.m_SwapchainExtent.height;
    nvrhi::GraphicsAPI api = renderer->m_NvrhiDevice->getGraphicsAPI();
    if (api == nvrhi::GraphicsAPI::VULKAN)
        state.viewport.viewports.push_back(nvrhi::Viewport(0.0f, (float)w, (float)h, 0.0f, 0.0f, 1.0f));
    else
        state.viewport.viewports.push_back(nvrhi::Viewport(0.0f, (float)w, 0.0f, (float)h, 0.0f, 1.0f));
    state.viewport.scissorRects.resize(1);
    state.viewport.scissorRects[0].minX = 0; state.viewport.scissorRects[0].minY = 0;
    state.viewport.scissorRects[0].maxX = (int)w; state.viewport.scissorRects[0].maxY = (int)h;

    // Note: we'll set graphics state per-object after updating the volatile CB

    // Iterate nodes and draw primitives
    Camera* cam = &renderer->m_Camera;
    Matrix viewProj = cam->GetViewProjMatrix();

    for (size_t ni = 0; ni < renderer->m_Scene.m_Nodes.size(); ++ni)
    {
        const Scene::Node& node = renderer->m_Scene.m_Nodes[ni];
        if (node.m_MeshIndex < 0) continue;
        const Scene::Mesh& mesh = renderer->m_Scene.m_Meshes[node.m_MeshIndex];

        // Per-object world matrix (use Matrix for CPU-side math)
        PerObjectData cb{};
        cb.ViewProj = viewProj;
        cb.World = node.m_WorldTransform;

        // Draw each primitive with its material values (update volatile CB per-primitive)
        for (const Scene::Primitive& prim : mesh.m_Primitives)
        {
            // Default material values
            cb.BaseColor = Vector4{1.0f, 1.0f, 1.0f, 1.0f};
            cb.RoughnessMetallic = Vector2{1.0f, 0.0f};
            // Camera position for view vector (xyz), w unused
            Vector3 camPos = renderer->m_Camera.GetPosition();
            cb.CameraPos = Vector4{ camPos.x, camPos.y, camPos.z, 0.0f };

            // Override from primitive material when available
            if (prim.m_MaterialIndex >= 0 && prim.m_MaterialIndex < (int)renderer->m_Scene.m_Materials.size())
            {
                const Scene::Material& mat = renderer->m_Scene.m_Materials[prim.m_MaterialIndex];
                cb.BaseColor = mat.m_BaseColorFactor;
                cb.RoughnessMetallic = Vector2{ mat.m_RoughnessFactor, mat.m_MetallicFactor };
            }

            // Write PerObjectData directly into the per-frame volatile constant buffer for this primitive
            commandList->writeBuffer(perFrameCB, &cb, sizeof(cb), 0);

            // Re-set graphics state so the binding uses the latest volatile CB suballocation
            commandList->setGraphicsState(state);

            nvrhi::DrawArguments args{};
            args.vertexCount = prim.m_IndexCount;
            args.startIndexLocation = prim.m_IndexOffset;
            args.startVertexLocation = prim.m_VertexOffset;
            commandList->drawIndexed(args);
        }
    }
}
