#pragma once

#include "pch.h"

class ImGuiLayer
{
public:
    bool Initialize();
    void Shutdown();
    void ProcessEvent(const SDL_Event& event);
    void UpdateFrame();
};
