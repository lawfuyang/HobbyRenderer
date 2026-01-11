#include "pch.h"
#include "ImGuiLayer.h"
#include "Renderer.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include "CommonResources.h"

bool ImGuiLayer::Initialize(SDL_Window* window)
{
    m_Window = window;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForVulkan(m_Window))
    {
        SDL_Log("[Init] Failed to initialize ImGui SDL3 backend");
        SDL_assert(false && "ImGui_ImplSDL3_InitForVulkan failed");
        return false;
    }

    if (!CreateDeviceObjects())
    {
        SDL_Log("[Init] Failed to create ImGui device objects");
        SDL_assert(false && "CreateDeviceObjects failed");
        return false;
    }

    SDL_Log("[Init] ImGui initialized successfully");
    return true;
}

void ImGuiLayer::Shutdown()
{
    SDL_Log("[Shutdown] Shutting down ImGui");
    DestroyDeviceObjects();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

bool ImGuiLayer::CreateDeviceObjects()
{
    SDL_Log("[Init] Creating ImGui device objects");
    Renderer* renderer = Renderer::GetInstance();

    // Create input layout (vertex attributes)
    {
        nvrhi::VertexAttributeDesc attributes[] = {
	        { "POSITION", nvrhi::Format::RG32_FLOAT,  1, 0, offsetof(ImDrawVert,pos), sizeof(ImDrawVert), false },
	        { "TEXCOORD", nvrhi::Format::RG32_FLOAT,  1, 0, offsetof(ImDrawVert,uv),  sizeof(ImDrawVert), false },
	        { "COLOR",    nvrhi::Format::RGBA8_UNORM, 1, 0, offsetof(ImDrawVert,col), sizeof(ImDrawVert), false },
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

    // Create a single binding layout that includes push constants (VS) and texture/sampler (PS)
    {
        nvrhi::BindingLayoutDesc descriptorLayoutDesc;
        descriptorLayoutDesc.visibility = nvrhi::ShaderType::All; // covers VS (push constants) and PS (texture/sampler)
        descriptorLayoutDesc.bindingOffsets.shaderResource = Renderer::SPIRV_TEXTURE_SHIFT; // matches ShaderMake tRegShift default
        descriptorLayoutDesc.bindingOffsets.sampler = Renderer::SPIRV_SAMPLER_SHIFT;        // matches ShaderMake sRegShift default
        descriptorLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::PushConstants(0, sizeof(float) * 4), // push constants for VS
            nvrhi::BindingLayoutItem::Texture_SRV(0),   // logical slot -> SPIR-V binding shift
            nvrhi::BindingLayoutItem::Sampler(0)        // logical slot -> SPIR-V binding shift
        };

        m_BindingLayout = renderer->m_NvrhiDevice->createBindingLayout(descriptorLayoutDesc);
        
        if (!m_BindingLayout)
        {
            SDL_Log("[Error] Failed to create ImGui binding layout");
            SDL_assert(false && "ImGui binding layout creation failed");
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
        nvrhi::CommandListHandle commandList = renderer->AcquireCommandList("ImGuiFont");
        commandList->writeTexture(m_FontTexture, 0, 0, pixels, width * 4);
        renderer->SubmitCommandList(commandList);
        renderer->ExecutePendingCommandLists();
    }

    // Create graphics pipeline
    {
        Renderer* renderer = Renderer::GetInstance();
        
        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.VS = renderer->GetShaderHandle("imgui_VSMain");
        pipelineDesc.PS = renderer->GetShaderHandle("imgui_PSMain");
        pipelineDesc.inputLayout = m_InputLayout;
        pipelineDesc.bindingLayouts = { m_BindingLayout };
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;

        // Render state
        pipelineDesc.renderState.rasterState = CommonResources::GetInstance().RasterCullNone;

        // Blend state - build from common ImGui alpha blend target
        pipelineDesc.renderState.blendState.targets[0] = CommonResources::GetInstance().BlendTargetImGui;

        // Depth stencil state - use common disabled depth state
        pipelineDesc.renderState.depthStencilState = CommonResources::GetInstance().DepthDisabled;

        // Framebuffer info - rendering to swapchain
        nvrhi::FramebufferInfoEx fbInfo;
        fbInfo.colorFormats = { renderer->m_RHI.VkFormatToNvrhiFormat(renderer->m_RHI.m_SwapchainFormat) };

        m_Pipeline = renderer->m_NvrhiDevice->createGraphicsPipeline(pipelineDesc, fbInfo);
        
        if (!m_Pipeline)
        {
            SDL_Log("[Error] Failed to create ImGui graphics pipeline");
            SDL_assert(false && "ImGui pipeline creation failed");
            return false;
        }
    }

    SDL_Log("[Init] ImGui device objects created successfully");
    return true;
}

void ImGuiLayer::DestroyDeviceObjects()
{
    // Release all GPU resources
    m_Pipeline = nullptr;
    m_BindingLayout = nullptr;
    m_InputLayout = nullptr;
    m_IndexBuffer = nullptr;
    m_VertexBuffer = nullptr;
    m_FontTexture = nullptr;

    m_VertexBufferSize = 0;
    m_IndexBufferSize = 0;
}

void ImGuiLayer::ProcessEvent(const SDL_Event& event)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiLayer::RenderFrame(nvrhi::CommandListHandle commandList)
{
    Renderer* renderer = Renderer::GetInstance();

    const double fps = renderer->m_FPS;
    const double frameTime = renderer->m_FrameTime;

    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    if (ImGui::BeginMainMenuBar())
    {
        ImGui::Text("FPS: %.1f", fps);
        ImGui::Text("Frame Time: %.3f ms", frameTime);
        ImGui::EndMainMenuBar();
    }

    if (ImGui::Begin("Property Grid"))
    {
        static bool s_ShowDemoWindow = false;
        if (ImGui::Checkbox("Show Demo Window", &s_ShowDemoWindow))
        {
            ImGui::ShowDemoWindow();
        }

        ImGui::End();
    }

    ImGui::Render();

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
        state.pipeline = m_Pipeline;
        state.framebuffer = framebuffer;
        
        // Create binding set on demand
        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::PushConstants(0, sizeof(float) * 4),
            nvrhi::BindingSetItem::Texture_SRV(0, m_FontTexture),
            nvrhi::BindingSetItem::Sampler(0, CommonResources::GetInstance().LinearClamp)
        };
        nvrhi::BindingSetHandle bindingSet = renderer->m_NvrhiDevice->createBindingSet(bindingSetDesc, m_BindingLayout);

        const ImGuiIO& io = ImGui::GetIO();
        
        state.bindings = { bindingSet };
        state.vertexBuffers = { nvrhi::VertexBufferBinding{m_VertexBuffer, 0, 0} };
        state.indexBuffer = nvrhi::IndexBufferBinding{ m_IndexBuffer, sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT, 0 };
        state.viewport.viewports.push_back(nvrhi::Viewport{ io.DisplaySize.x * io.DisplayFramebufferScale.x, -io.DisplaySize.y * io.DisplayFramebufferScale.y });
        state.viewport.scissorRects.resize(1);  // updated below
        commandList->setGraphicsState(state);

        struct PushConstants
        {
            float uScale[2];
            float uTranslate[2];
        } pushConstants;

        // Push constants (scale and translate)
        pushConstants.uScale[0] = 2.0f / draw_data->DisplaySize.x;
        pushConstants.uScale[1] = 2.0f / draw_data->DisplaySize.y;
        pushConstants.uTranslate[0] = -1.0f - draw_data->DisplayPos.x * pushConstants.uScale[0];
        pushConstants.uTranslate[1] = -1.0f - draw_data->DisplayPos.y * pushConstants.uScale[1];
        commandList->setPushConstants(&pushConstants, sizeof(pushConstants));

        // Render command lists
        int global_vtx_offset = 0;
        int global_idx_offset = 0;
        for (const ImDrawList* draw_list : draw_data->CmdLists)
        {
            for (int cmd_i = 0; cmd_i < draw_list->CmdBuffer.Size; cmd_i++)
            {
                const ImDrawCmd* pcmd = &draw_list->CmdBuffer[cmd_i];

                nvrhi::Rect& r = state.viewport.scissorRects[0];
                r.minX = (int)pcmd->ClipRect.x;
                r.maxY = (int)pcmd->ClipRect.y;
                r.maxX = (int)pcmd->ClipRect.z;
                r.minY = (int)pcmd->ClipRect.w;

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


