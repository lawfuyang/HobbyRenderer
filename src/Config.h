#pragma once

#include "pch.h"

struct Config
{
    // Graphics settings
    nvrhi::GraphicsAPI m_GraphicsAPI = nvrhi::GraphicsAPI::D3D12;
    bool m_EnableValidation = false;
    bool m_EnableGPUAssistedValidation = false;
    // Path to a scene to load (empty = none)
    std::string m_ScenePath = "";
    // Skip loading textures from scene
    bool m_SkipTextures = false;
    // Skip loading/saving scene cache
    bool m_SkipCache = false;

    bool ExecutePerPass = false;
    bool ExecutePerPassAndWait = false;

    // Add more configuration options here as needed
    // int renderWidth = 1920;
    // int renderHeight = 1080;
    // float renderScale = 1.0f;

    static const Config& Get() { return s_Instance; }
    static void ParseCommandLine(int argc, char* argv[]);

private:
    static Config s_Instance;
};

inline Config Config::s_Instance{};
