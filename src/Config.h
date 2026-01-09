#pragma once

#include "pch.h"

struct Config
{
    // Graphics settings
    bool m_EnableGPUValidation = false;

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
