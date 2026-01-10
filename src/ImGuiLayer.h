#pragma once

#include "pch.h"

class ImGuiLayer
{
public:
    bool Initialize(SDL_Window* window);
    void Shutdown();
    void ProcessEvent(const SDL_Event& event);
    void RenderFrame(double fps, double frameTime);

private:
    SDL_Window* m_Window = nullptr;
};
