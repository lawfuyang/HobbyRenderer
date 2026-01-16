#include "pch.h"
#include "Camera.h"
#include "Renderer.h"
#include <cmath>

Camera::Camera()
{
}

void Camera::ProcessEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    {
        bool down = (event.type == SDL_EVENT_KEY_DOWN);
        // SDL3: scancode is available directly on SDL_KeyboardEvent as 'scancode'
        int sc = event.key.scancode;
        // W S A D
        if (sc == SDL_SCANCODE_W) m_Forward = down;
        if (sc == SDL_SCANCODE_S) m_Back = down;
        if (sc == SDL_SCANCODE_A) m_Left = down;
        if (sc == SDL_SCANCODE_D) m_Right = down;
    }
    break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    {
            if (event.button.button == SDL_BUTTON_RIGHT)
            {
                m_Rotating = true;
                m_LastMouseX = event.button.x;
                m_LastMouseY = event.button.y;
            }
    }
    break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
            if (event.button.button == SDL_BUTTON_RIGHT)
            {
                m_Rotating = false;
            }
    }
    break;
    case SDL_EVENT_MOUSE_MOTION:
    {
        if (m_Rotating)
        {
            int dx = event.motion.x - m_LastMouseX;
            int dy = event.motion.y - m_LastMouseY;
            // When relative mouse is set, SDL provides motion deltas directly
            if (event.motion.which == SDL_TOUCH_MOUSEID || event.motion.which == 0)
            {
                // Use event.motion.xrel/xrel if available
                dx = event.motion.xrel;
                dy = event.motion.yrel;
            }
            m_LastMouseX = event.motion.x;
            m_LastMouseY = event.motion.y;

            Renderer* renderer = Renderer::GetInstance();
            float yawDelta = dx * m_MouseSensitivity;
            yawDelta = -yawDelta; // Negate yaw for Vulkan coordinate system
            m_Yaw += yawDelta;
            m_Pitch += dy * m_MouseSensitivity;  // Positive dy (mouse down) should increase pitch (look down)
            // Clamp pitch
            if (m_Pitch > DirectX::XM_PIDIV2 - 0.01f) m_Pitch = DirectX::XM_PIDIV2 - 0.01f;
            if (m_Pitch < -DirectX::XM_PIDIV2 + 0.01f) m_Pitch = -DirectX::XM_PIDIV2 + 0.01f;
        }
    }
    break;
    case SDL_EVENT_MOUSE_WHEEL:
    {
        if (m_Rotating)
        {
            float multiplier = (event.wheel.y > 0) ? 1.1f : 0.9f;
            m_MoveSpeed *= multiplier;
            // Clamp speed
            if (m_MoveSpeed < 0.1f) m_MoveSpeed = 0.1f;
            if (m_MoveSpeed > 100.0f) m_MoveSpeed = 100.0f;
        }
    }
    break;
    case SDL_EVENT_WINDOW_RESIZED:
    {
        int w = event.window.data1;
        int h = event.window.data2;
        if (h > 0) m_Proj.aspectRatio = float(w) / float(h);
    }
    break;
    default:
        break;
    }
}

void Camera::Update()
{
    // Retrieve frame time from renderer (ms -> seconds)
    Renderer* renderer = Renderer::GetInstance();
    float dt = static_cast<float>(renderer->GetFrameTimeMs() * 0.001);
    if (dt <= 0.0f)
        return;

    // Movement in local space
    Vector forward = DirectX::XMVectorSet(0, 0, 1, 0);
    Vector right = DirectX::XMVectorSet(-1, 0, 0, 0); // Negate X for Vulkan right-handed coordinate system

    // Construct rotation from yaw/pitch
    Vector rot = DirectX::XMQuaternionRotationRollPitchYaw(m_Pitch, m_Yaw, 0.0f);
    Vector fw = DirectX::XMVector3Rotate(forward, rot);
    Vector rt = DirectX::XMVector3Rotate(right, rot);

    Vector pos = DirectX::XMLoadFloat3(reinterpret_cast<const Vector3*>(&m_Position));
    Vector move = DirectX::XMVectorZero();
    if (m_Forward) move = DirectX::XMVectorAdd(move, fw);
    if (m_Back) move = DirectX::XMVectorSubtract(move, fw);
    if (m_Left) move = DirectX::XMVectorSubtract(move, rt);
    if (m_Right) move = DirectX::XMVectorAdd(move, rt);

    if (DirectX::XMVector3NearEqual(move, DirectX::XMVectorZero(), DirectX::XMVectorReplicate(1e-6f)) == 0)
    {
        move = DirectX::XMVector3Normalize(move);
        move = DirectX::XMVectorScale(move, m_MoveSpeed * dt);
        pos = DirectX::XMVectorAdd(pos, move);
        DirectX::XMStoreFloat3(reinterpret_cast<Vector3*>(&m_Position), pos);
    }
}

Matrix Camera::GetViewMatrix() const
{
    using namespace DirectX;
    XMVECTOR pos = XMLoadFloat3(reinterpret_cast<const Vector3*>(&m_Position));
    XMVECTOR rot = XMQuaternionRotationRollPitchYaw(m_Pitch, m_Yaw, 0.0f);
    XMVECTOR forward = XMVector3Rotate(XMVectorSet(0,0,1,0), rot);
    XMVECTOR up = XMVector3Rotate(XMVectorSet(0,1,0,0), rot);
    XMMATRIX m = XMMatrixLookToLH(pos, forward, up);
    Matrix out{};
    XMStoreFloat4x4(&out, m);
    return out;
}

Matrix Camera::GetProjMatrix() const
{
    // Create an infinite projection matrix (no far plane) for better depth precision.
    float yScale = 1.0f / std::tan(m_Proj.fovY * 0.5f);
    float xScale = yScale / m_Proj.aspectRatio;

    Matrix m{};
    m._11 = xScale;
    // Negate X and Y scales for Vulkan to match DirectX Y-up convention and right-handed coordinates
    Renderer* renderer = Renderer::GetInstance();
    m._11 = -xScale;
    m._22 = -yScale;
    m._33 = 0.0f;
    m._34 = 1.0f;
    m._43 = m_Proj.nearZ;
    m._44 = 0.0f;

    return m;
}

Matrix Camera::GetViewProjMatrix() const
{
    using namespace DirectX;
    // Reuse view and projection getters to avoid code duplication
    Matrix viewM = GetViewMatrix();
    Matrix projM = GetProjMatrix();
    XMMATRIX view = XMLoadFloat4x4(&viewM);
    XMMATRIX proj = XMLoadFloat4x4(&projM);
    XMMATRIX vp = XMMatrixMultiply(view, proj);
    Matrix out{};
    XMStoreFloat4x4(&out, vp);
    return out;
}

void Camera::SetFromMatrix(const Matrix& worldTransform)
{
    using namespace DirectX;
    XMMATRIX m = XMLoadFloat4x4(&worldTransform);

    // Extract position
    XMVECTOR pos = m.r[3];
    XMStoreFloat3(reinterpret_cast<Vector3*>(&m_Position), pos);

    // Extract rotation matrix (3x3 part)
    XMMATRIX rotationM = m;
    rotationM.r[3] = XMVectorSet(0, 0, 0, 1); // remove translation

    // GLTF cameras look down -Z, our camera looks down +Z, so adjust by 180 degrees around Y
    XMMATRIX rotY180 = XMMatrixRotationY(XM_PI);
    XMMATRIX effectiveRot = XMMatrixMultiply(rotY180, rotationM);

    // Forward direction in world space
    XMVECTOR worldForward = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), effectiveRot);
    worldForward = XMVector3Normalize(worldForward);

    // Compute yaw and pitch from forward direction
    XMFLOAT3 fwd;
    XMStoreFloat3(&fwd, worldForward);
    m_Yaw = atan2f(fwd.x, fwd.z);
    Renderer* renderer = Renderer::GetInstance();
    m_Pitch = -asinf(fwd.y); // Negate pitch for Vulkan coordinate system
}

void Camera::Reset()
{
    m_Position = { 0.0f, 0.0f, 0.0f };
    m_Yaw = 0.0f;
    m_Pitch = 0.0f;
}
