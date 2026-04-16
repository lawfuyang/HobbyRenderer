
#include "Camera.h"
#include "Renderer.h"
#include "Utilities.h"

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

            m_Yaw += dx * m_MouseSensitivity;
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
    case SDL_EVENT_WINDOW_MINIMIZED:
    {
        // Prevent window minimization by immediately restoring it
        SDL_RestoreWindow(SDL_GetWindowFromID(event.window.windowID));
        SDL_Log("[Window] Prevented window minimization");
    }
    break;
    default:
        break;
    }
}

void Camera::Update()
{
    // Retrieve frame time from renderer (ms -> seconds)
    
    float dt = static_cast<float>(g_Renderer.GetFrameTimeMs() * 0.001);

    // Compute exposure multiplier (manual mode)
    float finalEV = m_ExposureValue - m_ExposureCompensation;
    m_Exposure = 1.0f / (powf(2.0f, finalEV) * 1.2f);

    if (dt <= 0.0f)
        return;

    // Movement in local space
    Vector forward = DirectX::XMVectorSet(0, 0, 1, 0);
    Vector right = DirectX::XMVectorSet(1, 0, 0, 0);

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
    m._22 = yScale;
    m._33 = 0.0f;
    m._34 = 1.0f;
    m._43 = m_Proj.nearZ;
    m._44 = 0.0f;

    return m;
}

Matrix Camera::GetInvProjMatrix() const
{
    using namespace DirectX;
    Matrix projM = GetProjMatrix();
    XMMATRIX proj = XMLoadFloat4x4(&projM);
    XMMATRIX invProj = XMMatrixInverse(nullptr, proj);
    Matrix out{};
    XMStoreFloat4x4(&out, invProj);
    return out;
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

Matrix Camera::GetInvViewProjMatrix() const
{
    using namespace DirectX;
    Matrix vpM = GetViewProjMatrix();
    XMMATRIX vp = XMLoadFloat4x4(&vpM);
    XMMATRIX invVp = XMMatrixInverse(nullptr, vp);
    Matrix out{};
    XMStoreFloat4x4(&out, invVp);
    return out;
}

void Camera::FillPlanarViewConstants(srrhi::PlanarViewConstants& constants, float viewportWidth, float viewportHeight) const
{
    using namespace DirectX;
    Matrix viewM = GetViewMatrix();
    Matrix projM = GetProjMatrix();
    XMMATRIX view = XMLoadFloat4x4(&viewM);
    XMMATRIX proj = XMLoadFloat4x4(&projM);
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);

    XMVECTOR det;
    XMMATRIX invView = XMMatrixInverse(&det, view);
    XMMATRIX invProj = XMMatrixInverse(&det, proj);
    XMMATRIX invViewProj = XMMatrixInverse(&det, viewProj);

    XMStoreFloat4x4(&constants.m_MatWorldToView, view);
    XMStoreFloat4x4(&constants.m_MatViewToWorld, invView);

    // Jittered versions — only apply sub-pixel jitter when TAA is enabled.
    // When TAA is off the "jittered" matrices are identical to the non-jittered
    // ones so that nothing on screen jitters.
    
    Vector2 jitter = { 0.0f, 0.0f };

    if (g_Renderer.m_bTAAEnabled)
    {
        uint32_t frameIndex = g_Renderer.m_FrameNumber;
        jitter = { Halton(frameIndex % 16 + 1, 2) - 0.5f, Halton(frameIndex % 16 + 1, 3) - 0.5f };
    }

    XMMATRIX jitterMatrix = XMMatrixTranslation(2.0f * jitter.x / viewportWidth, -2.0f * jitter.y / viewportHeight, 0.0f);
    XMMATRIX viewProjJittered = XMMatrixMultiply(viewProj, jitterMatrix);
    XMMATRIX invViewProjJittered = XMMatrixInverse(&det, viewProjJittered);
    XMMATRIX projJittered = XMMatrixMultiply(proj, jitterMatrix);
    XMMATRIX invProjJittered = XMMatrixInverse(&det, projJittered);

    XMStoreFloat4x4(&constants.m_MatViewToClip, projJittered);
    XMStoreFloat4x4(&constants.m_MatWorldToClip, viewProjJittered);
    XMStoreFloat4x4(&constants.m_MatClipToView, invProjJittered);
    XMStoreFloat4x4(&constants.m_MatClipToWorld, invViewProjJittered);

    XMStoreFloat4x4(&constants.m_MatViewToClipNoOffset, proj);
    XMStoreFloat4x4(&constants.m_MatWorldToClipNoOffset, viewProj);
    XMStoreFloat4x4(&constants.m_MatClipToViewNoOffset, invProj);
    XMStoreFloat4x4(&constants.m_MatClipToWorldNoOffset, invViewProj);

    constants.m_ViewportOrigin = Vector2(0, 0);
    constants.m_ViewportSize = Vector2(viewportWidth, viewportHeight);
    constants.m_ViewportSizeInv = Vector2(1.0f / viewportWidth, 1.0f / viewportHeight);
    constants.m_PixelOffset = jitter;

    constants.m_ClipToWindowScale = Vector2(0.5f * viewportWidth, -0.5f * viewportHeight);
    constants.m_ClipToWindowBias = Vector2(0.5f * viewportWidth, 0.5f * viewportHeight);
}

void Camera::SetFromMatrix(const Matrix& worldTransform)
{
    using namespace DirectX;
    XMMATRIX m = XMLoadFloat4x4(&worldTransform);

    // Extract position
    XMVECTOR pos = m.r[3];
    XMStoreFloat3(reinterpret_cast<Vector3*>(&m_Position), pos);

    // Forward direction in world space
    XMVECTOR worldForward = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), m);
    worldForward = XMVector3Normalize(worldForward);

    // Compute yaw and pitch from forward direction
    XMFLOAT3 fwd;
    XMStoreFloat3(&fwd, worldForward);
    m_Yaw = atan2f(fwd.x, fwd.z);
    m_Pitch = -asinf(fwd.y);
}

void Camera::Reset()
{
    m_Position = { 0.0f, 0.0f, 0.0f };
    m_Yaw = 0.0f;
    m_Pitch = 0.0f;
}
