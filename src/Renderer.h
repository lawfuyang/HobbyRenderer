#pragma once

#include "pch.h"
#include "GraphicRHI.h"
#include "ImGuiLayer.h"

struct Renderer
{
    SDL_Window* m_Window = nullptr;
    GraphicRHI m_RHI{};
    nvrhi::DeviceHandle m_NvrhiDevice;
    nvrhi::TextureHandle m_SwapchainTextures[GraphicRHI::SwapchainImageCount] = {};

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
    
};
