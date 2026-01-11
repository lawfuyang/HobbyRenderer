#pragma once

#include "pch.h"
#include "GraphicRHI.h"
#include "ImGuiLayer.h"

struct Renderer
{
    // ShaderMake default SPIRV register shifts (from external/ShaderMake/ShaderMake/ShaderMake.cpp Options)
    static constexpr uint32_t SPIRV_SAMPLER_SHIFT = 100;  // sRegShift (s#)
    static constexpr uint32_t SPIRV_TEXTURE_SHIFT = 200;  // tRegShift (t#)
    static constexpr uint32_t SPIRV_CBUFFER_SHIFT = 300;  // bRegShift (b#)
    static constexpr uint32_t SPIRV_UAV_SHIFT = 400;      // uRegShift (u#)

    static void SetInstance(Renderer* instance);
    static Renderer* GetInstance();
    nvrhi::CommandListHandle AcquireCommandList();
    void ReleaseCommandList(const nvrhi::CommandListHandle& commandList);
    void SubmitCommandList(const nvrhi::CommandListHandle& commandList);
    void ExecutePendingCommandLists();

    SDL_Window* m_Window = nullptr;
    GraphicRHI m_RHI{};
    nvrhi::DeviceHandle m_NvrhiDevice;
    nvrhi::TextureHandle m_SwapchainTextures[GraphicRHI::SwapchainImageCount] = {};
    std::vector<nvrhi::CommandListHandle> m_CommandListFreeList;
    std::vector<nvrhi::CommandListHandle> m_PendingCommandLists;

    // Cached shader handles loaded from compiled SPIR-V binaries (keyed by output stem, e.g., "imgui_VSMain")
    std::unordered_map<std::string, nvrhi::ShaderHandle> m_ShaderCache;

    // ImGui state
    double m_FrameTime = 0.0;
    double m_FPS = 0.0;

    // ImGui layer
    ImGuiLayer m_ImGuiLayer;

    bool Initialize();
    void Run();
    void Shutdown();

    // Retrieve a shader handle by name (output stem), e.g., "imgui_VSMain" or "imgui_PSMain"
    nvrhi::ShaderHandle GetShaderHandle(std::string_view name) const;

private:
    bool CreateNvrhiDevice();
    void DestroyNvrhiDevice();
    bool CreateSwapchainTextures();
    void DestroySwapchainTextures();
    
    bool LoadShaders();
    void UnloadShaders();

    static Renderer* s_Instance;
    
};
