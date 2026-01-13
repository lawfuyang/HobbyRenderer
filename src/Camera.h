#pragma once

#include "pch.h"

class Camera
{
public:
    Camera();

    Vector3 GetPosition() const { return m_Position; }

    void ProcessEvent(const SDL_Event& event);
    void Update();

    // Accessors
    Matrix GetViewMatrix() const;
    Matrix GetProjMatrix() const;
    Matrix GetViewProjMatrix() const;

    // Mark view dirty
    bool IsDirty() const { return m_Dirty; }
    void ClearDirty() { m_Dirty = false; }

    // Movement / input tuning (access directly)
    float m_MoveSpeed = 5.0f;
    float m_MouseSensitivity = 0.0025f;

private:
    Vector3 m_Position{0.0f, 0.0f, -5.0f};
    float m_Yaw = 0.0f; // radians
    float m_Pitch = 0.0f; // radians

    bool m_Forward = false;
    bool m_Back = false;
    bool m_Left = false;
    bool m_Right = false;

    bool m_Rotating = false;
    int m_LastMouseX = 0;
    int m_LastMouseY = 0;

    float m_Aspect = 16.0f/9.0f;
    float m_FovY = DirectX::XM_PIDIV4; // 45deg
    float m_Near = 0.1f;

    bool m_Dirty = true;
};
