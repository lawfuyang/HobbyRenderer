#pragma once

#include "pch.h"

struct ProjectionParams
{
    // Perspective only
    float aspectRatio = 16.0f / 9.0f;
    float fovY = DirectX::XM_PIDIV4; // 45deg
    float nearZ = 0.1f;
    // farZ is always infinite
};

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
    Matrix GetInvViewProjMatrix() const;

    // Set camera from world transform matrix (for GLTF cameras)
    void SetFromMatrix(const Matrix& worldTransform);

    // Set projection parameters
    void SetProjection(const ProjectionParams& proj) { m_Proj = proj; }

    // Reset camera to default position and orientation
    void Reset();

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

    ProjectionParams m_Proj{};
};
