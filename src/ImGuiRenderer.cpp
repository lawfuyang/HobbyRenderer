#include "Renderer.h"

// Request the shared PushConstants definition for C++ and include it.
#define IMGUI_DEFINE_PUSH_CONSTANTS
#include "shaders/ShaderShared.hlsl"

#include <imgui.h>
#include "CommonResources.h"

class ImGuiRenderer : public IRenderer
{
public:
    bool Initialize() override;
    void Render(nvrhi::CommandListHandle commandList) override;
    const char* GetName() const override { return "ImGui"; }

private:
    bool CreateDeviceObjects();
    void DestroyDeviceObjects();

    // GPU Resources (moved from ImGuiLayer)
    nvrhi::TextureHandle m_FontTexture;
    nvrhi::BufferHandle m_VertexBuffer;
    nvrhi::BufferHandle m_IndexBuffer;
    
    nvrhi::InputLayoutHandle m_InputLayout;

    uint32_t m_VertexBufferSize = 0;
    uint32_t m_IndexBufferSize = 0;
};

REGISTER_RENDERER(ImGuiRenderer);

bool ImGuiRenderer::Initialize()
{
    return CreateDeviceObjects();
}

void ImGuiRenderer::Render(nvrhi::CommandListHandle commandList)
{
    Renderer* renderer = Renderer::GetInstance();
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data)
    {
        // Replicate ImGui_ImplVulkan_RenderDrawData behavior using nvrhi

        int fb_width = (int)(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
        int fb_height = (int)(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
        if (fb_width <= 0 || fb_height <= 0)
            return;

        // Create framebuffer on demand
        nvrhi::TextureHandle renderTarget = renderer->GetCurrentBackBufferTexture();
        nvrhi::FramebufferHandle framebuffer = renderer->m_NvrhiDevice->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(renderTarget));

        // Create or resize vertex/index buffers
        size_t vertex_size = draw_data->TotalVtxCount * sizeof(ImDrawVert);
        size_t index_size = draw_data->TotalIdxCount * sizeof(ImDrawIdx);

        if (!m_VertexBuffer || m_VertexBufferSize < vertex_size)
        {
            if (m_VertexBufferSize < vertex_size)
            {
                SDL_Log("[ImGui] Vertex buffer size increased from %u to %zu bytes", m_VertexBufferSize, vertex_size);
            }
            m_VertexBuffer = nullptr;
            nvrhi::BufferDesc bufferDesc;
            bufferDesc.byteSize = vertex_size;
            bufferDesc.isVertexBuffer = true;
            bufferDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
            bufferDesc.keepInitialState = true;
            m_VertexBuffer = renderer->m_NvrhiDevice->createBuffer(bufferDesc);
            m_VertexBufferSize = (uint32_t)vertex_size;
        }

        if (!m_IndexBuffer || m_IndexBufferSize < index_size)
        {
            if (m_IndexBufferSize < index_size)
            {
                SDL_Log("[ImGui] Index buffer size increased from %u to %zu bytes", m_IndexBufferSize, index_size);
            }
            m_IndexBuffer = nullptr;
            nvrhi::BufferDesc bufferDesc;
            bufferDesc.byteSize = index_size;
            bufferDesc.isIndexBuffer = true;
            bufferDesc.initialState = nvrhi::ResourceStates::IndexBuffer;
            bufferDesc.keepInitialState = true;
            m_IndexBuffer = renderer->m_NvrhiDevice->createBuffer(bufferDesc);
            m_IndexBufferSize = (uint32_t)index_size;
        }

        // Collect vertex/index data
        std::vector<ImDrawVert> vertices;
        std::vector<ImDrawIdx> indices;
        for (const ImDrawList* draw_list : draw_data->CmdLists)
        {
            vertices.insert(vertices.end(), draw_list->VtxBuffer.Data, draw_list->VtxBuffer.Data + draw_list->VtxBuffer.Size);
            indices.insert(indices.end(), draw_list->IdxBuffer.Data, draw_list->IdxBuffer.Data + draw_list->IdxBuffer.Size);
        }

        // Upload data
        commandList->writeBuffer(m_VertexBuffer, vertices.data(), vertex_size, 0);
        commandList->writeBuffer(m_IndexBuffer, indices.data(), index_size, 0);

        // Setup render state
        nvrhi::GraphicsState state;
        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.VS = renderer->GetShaderHandle("imgui_VSMain");
        pipelineDesc.PS = renderer->GetShaderHandle("imgui_PSMain");
        pipelineDesc.inputLayout = m_InputLayout;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
        pipelineDesc.renderState.rasterState = CommonResources::GetInstance().RasterCullNone;
        pipelineDesc.renderState.blendState.targets[0] = CommonResources::GetInstance().BlendTargetImGui;
        pipelineDesc.renderState.depthStencilState = CommonResources::GetInstance().DepthDisabled;

        nvrhi::FramebufferInfoEx fbInfo;
        fbInfo.colorFormats = { renderer->m_RHI.VkFormatToNvrhiFormat(renderer->m_RHI.m_SwapchainFormat) };
        state.framebuffer = framebuffer;
        
        // Create binding set description and query the renderer for its layout so
        // the pipeline layout can include push-constant ranges and descriptor sets.
        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::PushConstants(0, sizeof(PushConstants)),
            nvrhi::BindingSetItem::Texture_SRV(0, m_FontTexture),
            nvrhi::BindingSetItem::Sampler(0, CommonResources::GetInstance().LinearClamp)
        };

        // Query the renderer for the binding layout cached for this binding-set description.
        nvrhi::BindingLayoutHandle layoutForSet = renderer->GetOrCreateBindingLayoutFromBindingSetDesc(bindingSetDesc, nvrhi::ShaderType::All);
        pipelineDesc.bindingLayouts = { layoutForSet };

        nvrhi::BindingSetHandle bindingSet = renderer->m_NvrhiDevice->createBindingSet(bindingSetDesc, layoutForSet);
        state.bindings = { bindingSet };
        state.vertexBuffers = { nvrhi::VertexBufferBinding{m_VertexBuffer, 0, 0} };
        state.indexBuffer = nvrhi::IndexBufferBinding{ m_IndexBuffer, sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT, 0 };

        // Set viewport to framebuffer size. Vulkan uses an inverted Y, so flip min/max Y for Vulkan.
        {
            // minX = 0, maxX = fb_width, minY = fb_height, maxY = 0 => inverted Y viewport for Vulkan
            state.viewport.viewports.push_back(nvrhi::Viewport(0.0f, (float)fb_width, (float)fb_height, 0.0f, 0.0f, 1.0f));
        }
        state.viewport.scissorRects.resize(1);

        nvrhi::GraphicsPipelineHandle pipeline = renderer->GetOrCreateGraphicsPipeline(pipelineDesc, fbInfo);
        if (!pipeline)
        {
            SDL_Log("[Error] Failed to obtain graphics pipeline from Renderer");
            SDL_assert(false && "Failed to obtain ImGui graphics pipeline");
            return;
        }
        state.pipeline = pipeline;

        commandList->setGraphicsState(state);

        PushConstants pushConstants{};

        // Push constants (scale and translate)
        pushConstants.uScale.x = 2.0f / draw_data->DisplaySize.x;
        pushConstants.uScale.y = 2.0f / draw_data->DisplaySize.y;
        pushConstants.uTranslate.x = -1.0f - draw_data->DisplayPos.x * pushConstants.uScale.x;
        pushConstants.uTranslate.y = -1.0f - draw_data->DisplayPos.y * pushConstants.uScale.y;
        commandList->setPushConstants(&pushConstants, sizeof(pushConstants));

        // Render command lists
        int global_vtx_offset = 0;
        int global_idx_offset = 0;
        for (const ImDrawList* draw_list : draw_data->CmdLists)
        {
            for (int cmd_i = 0; cmd_i < draw_list->CmdBuffer.Size; cmd_i++)
            {
                const ImDrawCmd* pcmd = &draw_list->CmdBuffer[cmd_i];

                // Project scissor/clipping rectangles into framebuffer space (match ImGui_ImplVulkan)
                ImVec2 clip_off = draw_data->DisplayPos;
                ImVec2 clip_scale = draw_data->FramebufferScale;

                ImVec2 clip_min((pcmd->ClipRect.x - clip_off.x) * clip_scale.x, (pcmd->ClipRect.y - clip_off.y) * clip_scale.y);
                ImVec2 clip_max((pcmd->ClipRect.z - clip_off.x) * clip_scale.x, (pcmd->ClipRect.w - clip_off.y) * clip_scale.y);

                // Clamp to viewport
                if (clip_min.x < 0.0f) clip_min.x = 0.0f;
                if (clip_min.y < 0.0f) clip_min.y = 0.0f;
                if (clip_max.x > (float)fb_width) clip_max.x = (float)fb_width;
                if (clip_max.y > (float)fb_height) clip_max.y = (float)fb_height;
                if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
                    continue;

                nvrhi::Rect& r = state.viewport.scissorRects[0];
                r.minX = (int)clip_min.x;
                r.minY = (int)clip_min.y;
                r.maxX = (int)clip_max.x;
                r.maxY = (int)clip_max.y;

                commandList->setGraphicsState(state);
                commandList->setPushConstants(&pushConstants, sizeof(pushConstants));

                // Draw
                nvrhi::DrawArguments args;
                args.vertexCount = pcmd->ElemCount;
                args.startIndexLocation = pcmd->IdxOffset + global_idx_offset;
                args.startVertexLocation = pcmd->VtxOffset + global_vtx_offset;
                commandList->drawIndexed(args);
            }
            global_idx_offset += draw_list->IdxBuffer.Size;
            global_vtx_offset += draw_list->VtxBuffer.Size;
        }
    }
}

bool ImGuiRenderer::CreateDeviceObjects()
{
    SDL_Log("[Init] Creating ImGui device objects");
    Renderer* renderer = Renderer::GetInstance();

    // Create input layout (vertex attributes)
    {
        nvrhi::VertexAttributeDesc attributes[] = {
        	{ "POSITION", nvrhi::Format::RG32_FLOAT,  1, 0, offsetof(ImDrawVert,pos), sizeof(ImDrawVert), false },
        	{ "TEXCOORD0", nvrhi::Format::RG32_FLOAT,  1, 0, offsetof(ImDrawVert,uv),  sizeof(ImDrawVert), false },
        	{ "COLOR0",    nvrhi::Format::RGBA8_UNORM, 1, 0, offsetof(ImDrawVert,col), sizeof(ImDrawVert), false },
        };

        // Note: vertexShader parameter is only used by DX11 backend, unused in Vulkan
        m_InputLayout = renderer->m_NvrhiDevice->createInputLayout(attributes, 3, nullptr);
        
        if (!m_InputLayout)
        {
            SDL_Log("[Error] Failed to create ImGui input layout");
            SDL_assert(false && "ImGui input layout creation failed");
            return false;
        }
    }

    // Create font texture
    {
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        nvrhi::TextureDesc textureDesc;
        textureDesc.width = width;
        textureDesc.height = height;
        textureDesc.format = nvrhi::Format::RGBA8_UNORM;
        textureDesc.dimension = nvrhi::TextureDimension::Texture2D;
        textureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        textureDesc.keepInitialState = true;
        textureDesc.isRenderTarget = false;
        textureDesc.isUAV = false;
        textureDesc.debugName = "ImGui Font Texture";

        m_FontTexture = renderer->m_NvrhiDevice->createTexture(textureDesc);
        
        if (!m_FontTexture)
        {
            SDL_Log("[Error] Failed to create ImGui font texture");
            SDL_assert(false && "ImGui font texture creation failed");
            return false;
        }

        // Upload font texture data
        nvrhi::CommandListHandle cmd = renderer->AcquireCommandList("ImGui Font Upload");
        cmd->writeTexture(m_FontTexture, 0, 0, pixels, width * 4);
        renderer->SubmitCommandList(cmd);
    }

    SDL_Log("[Init] ImGui device objects created successfully");
    return true;
}

void ImGuiRenderer::DestroyDeviceObjects()
{
    m_FontTexture = nullptr;
    m_VertexBuffer = nullptr;
    m_IndexBuffer = nullptr;
    m_InputLayout = nullptr;
    m_VertexBufferSize = 0;
    m_IndexBufferSize = 0;
}