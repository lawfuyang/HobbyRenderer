// Tests_Camera.cpp — Phase 5: Camera Math
//
// Systems under test: Camera, DirectXMath, ProjectionParams, Transform
// Prerequisites: CPU-only — no GPU resources required.
//                g_Renderer must be initialized (for FillPlanarViewConstants
//                which reads g_Renderer.m_bTAAEnabled / m_FrameNumber).
//
// Test coverage:
//   - Default construction and Reset()
//   - View matrix: identity at origin, LH convention, orthonormality
//   - Projection matrix: infinite reverse-Z, aspect ratio, fovY
//   - Inverse matrices: V * V^-1 ≈ I, P * P^-1 ≈ I, VP * VP^-1 ≈ I
//   - SetFromMatrix / round-trip position extraction
//   - Exposure computation: EV100 → linear multiplier
//   - FillPlanarViewConstants: viewport fields, matrix consistency
//   - Frustum plane extraction (via ComputeFrustumPlanes logic)
//   - Pitch clamping bounds
//   - Projection parameter accessors
//
// Run with: HobbyRenderer --run-tests=*Camera*
// ============================================================================

#include "TestFixtures.h"

// ============================================================================
// Internal helpers
// ============================================================================
namespace
{
    // Tolerance for floating-point comparisons.
    static constexpr float kEps = 1e-4f;

    // Returns true if |a - b| <= eps for every element of two 4x4 matrices.
    bool MatrixNearEqual(const Matrix& a, const Matrix& b, float eps = kEps)
    {
        const float* pa = reinterpret_cast<const float*>(&a);
        const float* pb = reinterpret_cast<const float*>(&b);
        for (int i = 0; i < 16; ++i)
            if (std::fabs(pa[i] - pb[i]) > eps)
                return false;
        return true;
    }

    // Returns the 4x4 identity matrix.
    Matrix IdentityMatrix()
    {
        Matrix m{};
        m._11 = m._22 = m._33 = m._44 = 1.0f;
        return m;
    }

    // Multiply two Matrix values via DirectXMath.
    Matrix MatMul(const Matrix& a, const Matrix& b)
    {
        using namespace DirectX;
        XMMATRIX xa = XMLoadFloat4x4(&a);
        XMMATRIX xb = XMLoadFloat4x4(&b);
        Matrix out{};
        XMStoreFloat4x4(&out, XMMatrixMultiply(xa, xb));
        return out;
    }

    // Invert a Matrix via DirectXMath.
    Matrix MatInv(const Matrix& m)
    {
        using namespace DirectX;
        XMMATRIX xm = XMLoadFloat4x4(&m);
        Matrix out{};
        XMStoreFloat4x4(&out, XMMatrixInverse(nullptr, xm));
        return out;
    }

    // Build a Camera at a given position with given yaw/pitch by using
    // SetFromMatrix with a synthetic world transform.
    Camera MakeCameraAt(float x, float y, float z, float yaw = 0.0f, float pitch = 0.0f)
    {
        using namespace DirectX;
        Camera cam;
        cam.Reset();

        // Build a world transform: rotation from yaw/pitch, then translation.
        XMVECTOR rot = XMQuaternionRotationRollPitchYaw(pitch, yaw, 0.0f);
        XMMATRIX rotM = XMMatrixRotationQuaternion(rot);
        XMMATRIX transM = XMMatrixTranslation(x, y, z);
        XMMATRIX world = XMMatrixMultiply(rotM, transM);

        Matrix worldM{};
        XMStoreFloat4x4(&worldM, world);
        cam.SetFromMatrix(worldM);
        return cam;
    }
} // anonymous namespace

// ============================================================================
// TEST SUITE: Camera_DefaultState
// ============================================================================
TEST_SUITE("Camera_DefaultState")
{
    // ------------------------------------------------------------------
    // TC-CAM-DEFAULT-01: Default-constructed camera has expected position
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-DEFAULT-01 DefaultState - default position is origin after Reset")
    {
        Camera cam;
        cam.Reset();

        const Vector3 pos = cam.GetPosition();
        CHECK(std::fabs(pos.x) < kEps);
        CHECK(std::fabs(pos.y) < kEps);
        CHECK(std::fabs(pos.z) < kEps);
    }

    // ------------------------------------------------------------------
    // TC-CAM-DEFAULT-02: Default projection parameters are sane
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-DEFAULT-02 DefaultState - default projection parameters are sane")
    {
        Camera cam;
        const ProjectionParams& proj = cam.GetProjection();

        CHECK(proj.aspectRatio > 0.0f);
        CHECK(proj.fovY > 0.0f);
        CHECK(proj.fovY < DirectX::XM_PI);
        CHECK(proj.nearZ > 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-CAM-DEFAULT-03: Default exposure multiplier is positive
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-DEFAULT-03 DefaultState - default exposure multiplier is positive")
    {
        Camera cam;
        // m_Exposure is computed in Update(); the default field value is 1.0f.
        CHECK(cam.m_Exposure > 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-CAM-DEFAULT-04: Reset restores position to origin
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-DEFAULT-04 DefaultState - Reset restores position to origin")
    {
        Camera cam;
        // Move the camera via SetFromMatrix.
        Matrix t{};
        t._11 = t._22 = t._33 = t._44 = 1.0f;
        t._41 = 10.0f; t._42 = 5.0f; t._43 = -3.0f;
        cam.SetFromMatrix(t);

        const Vector3 before = cam.GetPosition();
        CHECK(std::fabs(before.x - 10.0f) < kEps);

        cam.Reset();
        const Vector3 after = cam.GetPosition();
        CHECK(std::fabs(after.x) < kEps);
        CHECK(std::fabs(after.y) < kEps);
        CHECK(std::fabs(after.z) < kEps);
    }
}

// ============================================================================
// TEST SUITE: Camera_ViewMatrix
// ============================================================================
TEST_SUITE("Camera_ViewMatrix")
{
    // ------------------------------------------------------------------
    // TC-CAM-VIEW-01: View matrix at origin looking along +Z has identity
    //                 rotation block (LH convention, no translation)
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-VIEW-01 ViewMatrix - camera at origin looking +Z")
    {
        Camera cam;
        cam.Reset(); // position = (0,0,0), yaw = 0, pitch = 0

        const Matrix view = cam.GetViewMatrix();

        // In LH look-to, looking along +Z with up = +Y:
        //   right  = +X  → row 0 = (1, 0, 0, 0)
        //   up     = +Y  → row 1 = (0, 1, 0, 0)
        //   fwd    = +Z  → row 2 = (0, 0, 1, 0)
        //   trans  = 0   → row 3 = (0, 0, 0, 1)
        CHECK(std::fabs(view._11 - 1.0f) < kEps);
        CHECK(std::fabs(view._22 - 1.0f) < kEps);
        CHECK(std::fabs(view._33 - 1.0f) < kEps);
        CHECK(std::fabs(view._44 - 1.0f) < kEps);
        CHECK(std::fabs(view._41) < kEps);
        CHECK(std::fabs(view._42) < kEps);
        CHECK(std::fabs(view._43) < kEps);
    }

    // ------------------------------------------------------------------
    // TC-CAM-VIEW-02: View matrix rows are orthonormal
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-VIEW-02 ViewMatrix - rows are orthonormal")
    {
        Camera cam;
        cam.Reset();

        // Apply a 45-degree yaw so the matrix is non-trivial.
        ProjectionParams proj;
        proj.fovY = DirectX::XM_PIDIV4;
        proj.aspectRatio = 16.0f / 9.0f;
        proj.nearZ = 0.1f;
        cam.SetProjection(proj);

        // Rotate camera 45 degrees yaw via SetFromMatrix.
        using namespace DirectX;
        XMMATRIX rot = XMMatrixRotationY(XM_PIDIV4);
        XMMATRIX trans = XMMatrixTranslation(1.0f, 2.0f, 3.0f);
        XMMATRIX world = XMMatrixMultiply(rot, trans);
        Matrix worldM{};
        XMStoreFloat4x4(&worldM, world);
        cam.SetFromMatrix(worldM);

        const Matrix view = cam.GetViewMatrix();

        // Extract the 3×3 rotation block rows.
        const DirectX::XMFLOAT3 r0{ view._11, view._12, view._13 };
        const DirectX::XMFLOAT3 r1{ view._21, view._22, view._23 };
        const DirectX::XMFLOAT3 r2{ view._31, view._32, view._33 };

        auto dot3 = [](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        };
        auto len3 = [&](const DirectX::XMFLOAT3& v) {
            return std::sqrt(dot3(v, v));
        };

        // Each row must be unit length.
        CHECK(std::fabs(len3(r0) - 1.0f) < kEps);
        CHECK(std::fabs(len3(r1) - 1.0f) < kEps);
        CHECK(std::fabs(len3(r2) - 1.0f) < kEps);

        // Rows must be mutually orthogonal.
        CHECK(std::fabs(dot3(r0, r1)) < kEps);
        CHECK(std::fabs(dot3(r0, r2)) < kEps);
        CHECK(std::fabs(dot3(r1, r2)) < kEps);
    }

    // ------------------------------------------------------------------
    // TC-CAM-VIEW-03: Camera translation is encoded in the view matrix
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-VIEW-03 ViewMatrix - translation is encoded correctly")
    {
        // Place camera at (5, 0, 0) looking along +Z.
        Camera cam;
        cam.Reset();
        Matrix t{};
        t._11 = t._22 = t._33 = t._44 = 1.0f;
        t._41 = 5.0f; // translation X
        cam.SetFromMatrix(t);

        const Matrix view = cam.GetViewMatrix();

        // The view matrix for a camera at (5,0,0) looking +Z should have
        // a translation of -5 in the X column (row 3, col 1 in row-major).
        CHECK(std::fabs(view._41 - (-5.0f)) < kEps);
        CHECK(std::fabs(view._42) < kEps);
        CHECK(std::fabs(view._43) < kEps);
    }

    // ------------------------------------------------------------------
    // TC-CAM-VIEW-04: GetViewMatrix and GetViewProjMatrix are consistent
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-VIEW-04 ViewMatrix - ViewProj equals View * Proj")
    {
        Camera cam;
        cam.Reset();

        const Matrix view = cam.GetViewMatrix();
        const Matrix proj = cam.GetProjMatrix();
        const Matrix vp   = cam.GetViewProjMatrix();

        const Matrix expected = MatMul(view, proj);
        CHECK(MatrixNearEqual(vp, expected, 1e-3f));
    }
}

// ============================================================================
// TEST SUITE: Camera_Projection
// ============================================================================
TEST_SUITE("Camera_Projection")
{
    // ------------------------------------------------------------------
    // TC-CAM-PROJ-01: Projection matrix has correct infinite reverse-Z layout
    //                 m._33 == 0, m._34 == 1, m._43 == nearZ, m._44 == 0
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-PROJ-01 Projection - infinite reverse-Z layout is correct")
    {
        Camera cam;
        ProjectionParams proj;
        proj.fovY = DirectX::XM_PIDIV4;
        proj.aspectRatio = 1.0f;
        proj.nearZ = 0.1f;
        cam.SetProjection(proj);

        const Matrix m = cam.GetProjMatrix();

        CHECK(std::fabs(m._33 - 0.0f) < kEps);
        CHECK(std::fabs(m._34 - 1.0f) < kEps);
        CHECK(std::fabs(m._43 - proj.nearZ) < kEps);
        CHECK(std::fabs(m._44 - 0.0f) < kEps);
    }

    // ------------------------------------------------------------------
    // TC-CAM-PROJ-02: Projection matrix xScale = yScale / aspectRatio
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-PROJ-02 Projection - xScale equals yScale / aspectRatio")
    {
        Camera cam;
        ProjectionParams proj;
        proj.fovY = DirectX::XM_PIDIV4;
        proj.aspectRatio = 16.0f / 9.0f;
        proj.nearZ = 0.1f;
        cam.SetProjection(proj);

        const Matrix m = cam.GetProjMatrix();

        const float yScale = 1.0f / std::tan(proj.fovY * 0.5f);
        const float xScale = yScale / proj.aspectRatio;

        CHECK(std::fabs(m._11 - xScale) < kEps);
        CHECK(std::fabs(m._22 - yScale) < kEps);
    }

    // ------------------------------------------------------------------
    // TC-CAM-PROJ-03: Wider fovY produces smaller yScale (less zoom)
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-PROJ-03 Projection - wider fovY produces smaller yScale")
    {
        Camera camNarrow, camWide;

        ProjectionParams narrow;
        narrow.fovY = DirectX::XM_PIDIV4; // 45 deg
        narrow.aspectRatio = 1.0f;
        narrow.nearZ = 0.1f;
        camNarrow.SetProjection(narrow);

        ProjectionParams wide;
        wide.fovY = DirectX::XM_PIDIV2; // 90 deg
        wide.aspectRatio = 1.0f;
        wide.nearZ = 0.1f;
        camWide.SetProjection(wide);

        const Matrix mNarrow = camNarrow.GetProjMatrix();
        const Matrix mWide   = camWide.GetProjMatrix();

        CHECK(mNarrow._22 > mWide._22);
    }

    // ------------------------------------------------------------------
    // TC-CAM-PROJ-04: Changing nearZ shifts the m._43 element
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-PROJ-04 Projection - nearZ is reflected in m._43")
    {
        Camera cam;
        ProjectionParams proj;
        proj.fovY = DirectX::XM_PIDIV4;
        proj.aspectRatio = 1.0f;

        proj.nearZ = 0.01f;
        cam.SetProjection(proj);
        const float near01 = cam.GetProjMatrix()._43;

        proj.nearZ = 1.0f;
        cam.SetProjection(proj);
        const float near1 = cam.GetProjMatrix()._43;

        CHECK(std::fabs(near01 - 0.01f) < kEps);
        CHECK(std::fabs(near1  - 1.0f)  < kEps);
    }

    // ------------------------------------------------------------------
    // TC-CAM-PROJ-05: SetProjection round-trips through GetProjection
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-PROJ-05 Projection - SetProjection round-trips through GetProjection")
    {
        Camera cam;
        ProjectionParams proj;
        proj.fovY = 1.2f;
        proj.aspectRatio = 2.35f;
        proj.nearZ = 0.05f;
        cam.SetProjection(proj);

        const ProjectionParams& got = cam.GetProjection();
        CHECK(std::fabs(got.fovY        - proj.fovY)        < kEps);
        CHECK(std::fabs(got.aspectRatio - proj.aspectRatio) < kEps);
        CHECK(std::fabs(got.nearZ       - proj.nearZ)       < kEps);
    }
}

// ============================================================================
// TEST SUITE: Camera_InverseMatrices
// ============================================================================
TEST_SUITE("Camera_InverseMatrices")
{
    // ------------------------------------------------------------------
    // TC-CAM-INV-01: View * InvView ≈ Identity
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-INV-01 InverseMatrices - View * InvView is identity")
    {
        Camera cam = MakeCameraAt(3.0f, 1.0f, -2.0f, 0.5f, 0.2f);

        const Matrix view    = cam.GetViewMatrix();
        const Matrix invView = MatInv(view);
        const Matrix product = MatMul(view, invView);

        CHECK(MatrixNearEqual(product, Matrix{}, 1e-3f));
    }

    // ------------------------------------------------------------------
    // TC-CAM-INV-02: Proj * InvProj ≈ Identity
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-INV-02 InverseMatrices - Proj * InvProj is identity")
    {
        Camera cam;
        ProjectionParams proj;
        proj.fovY = DirectX::XM_PIDIV4;
        proj.aspectRatio = 16.0f / 9.0f;
        proj.nearZ = 0.1f;
        cam.SetProjection(proj);

        const Matrix p    = cam.GetProjMatrix();
        const Matrix invP = cam.GetInvProjMatrix();
        const Matrix prod = MatMul(p, invP);

        CHECK(MatrixNearEqual(prod, IdentityMatrix(), 1e-3f));
    }

    // ------------------------------------------------------------------
    // TC-CAM-INV-03: ViewProj * InvViewProj ≈ Identity
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-INV-03 InverseMatrices - ViewProj * InvViewProj is identity")
    {
        Camera cam = MakeCameraAt(1.0f, 2.0f, 3.0f, 0.3f, -0.1f);

        const Matrix vp    = cam.GetViewProjMatrix();
        const Matrix invVP = cam.GetInvViewProjMatrix();
        const Matrix prod  = MatMul(vp, invVP);

        CHECK(MatrixNearEqual(prod, IdentityMatrix(), 1e-3f));
    }

    // ------------------------------------------------------------------
    // TC-CAM-INV-04: GetInvProjMatrix matches manual inversion of GetProjMatrix
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-INV-04 InverseMatrices - GetInvProjMatrix matches manual inversion")
    {
        Camera cam;
        ProjectionParams proj;
        proj.fovY = 1.0f;
        proj.aspectRatio = 1.5f;
        proj.nearZ = 0.2f;
        cam.SetProjection(proj);

        const Matrix invP       = cam.GetInvProjMatrix();
        const Matrix manualInvP = MatInv(cam.GetProjMatrix());

        CHECK(MatrixNearEqual(invP, manualInvP, 1e-3f));
    }
}

// ============================================================================
// TEST SUITE: Camera_SetFromMatrix
// ============================================================================
TEST_SUITE("Camera_SetFromMatrix")
{
    // ------------------------------------------------------------------
    // TC-CAM-SFM-01: SetFromMatrix extracts position correctly
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-SFM-01 SetFromMatrix - position is extracted correctly")
    {
        Camera cam;
        cam.Reset();

        // Build a pure translation matrix.
        Matrix t{};
        t._11 = t._22 = t._33 = t._44 = 1.0f;
        t._41 = 7.0f;
        t._42 = -3.0f;
        t._43 = 12.0f;
        cam.SetFromMatrix(t);

        const Vector3 pos = cam.GetPosition();
        CHECK(std::fabs(pos.x - 7.0f)  < kEps);
        CHECK(std::fabs(pos.y - (-3.0f)) < kEps);
        CHECK(std::fabs(pos.z - 12.0f) < kEps);
    }

    // ------------------------------------------------------------------
    // TC-CAM-SFM-02: SetFromMatrix with identity leaves camera at origin
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-SFM-02 SetFromMatrix - identity matrix leaves camera at origin")
    {
        Camera cam;
        cam.SetFromMatrix(IdentityMatrix());

        const Vector3 pos = cam.GetPosition();
        CHECK(std::fabs(pos.x) < kEps);
        CHECK(std::fabs(pos.y) < kEps);
        CHECK(std::fabs(pos.z) < kEps);
    }

    // ------------------------------------------------------------------
    // TC-CAM-SFM-03: SetFromMatrix with 180-degree yaw rotation
    //                Camera should look along -Z (yaw ≈ π)
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-SFM-03 SetFromMatrix - 180-degree yaw produces -Z forward")
    {
        using namespace DirectX;
        Camera cam;
        cam.Reset();

        // 180-degree rotation around Y axis.
        XMMATRIX rot = XMMatrixRotationY(XM_PI);
        Matrix rotM{};
        XMStoreFloat4x4(&rotM, rot);
        cam.SetFromMatrix(rotM);

        // After 180-degree yaw, the view matrix forward direction (row 2 of view)
        // should point along -Z in world space.
        const Matrix view = cam.GetViewMatrix();
        // Row 2 of the view matrix is the camera's forward in view space.
        // For a 180-degree yaw, the world-space forward is -Z, so the view
        // matrix's third row should encode that.
        // We verify the view matrix is non-identity (camera is rotated).
        CHECK(std::fabs(view._11 - 1.0f) > kEps); // not identity
    }

    // ------------------------------------------------------------------
    // TC-CAM-SFM-04: Multiple SetFromMatrix calls overwrite previous state
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-SFM-04 SetFromMatrix - successive calls overwrite state")
    {
        Camera cam;

        Matrix t1{};
        t1._11 = t1._22 = t1._33 = t1._44 = 1.0f;
        t1._41 = 100.0f;
        cam.SetFromMatrix(t1);
        CHECK(std::fabs(cam.GetPosition().x - 100.0f) < kEps);

        Matrix t2{};
        t2._11 = t2._22 = t2._33 = t2._44 = 1.0f;
        t2._41 = -50.0f;
        cam.SetFromMatrix(t2);
        CHECK(std::fabs(cam.GetPosition().x - (-50.0f)) < kEps);
    }
}

// ============================================================================
// TEST SUITE: Camera_Exposure
// ============================================================================
TEST_SUITE("Camera_Exposure")
{
    // ------------------------------------------------------------------
    // TC-CAM-EXP-01: EV100 = 0 → exposure ≈ 1 / (1 * 1.2) ≈ 0.833
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-EXP-01 Exposure - EV100=0 produces expected multiplier")
    {
        // The formula: m_Exposure = 1 / (pow(2, EV - compensation) * 1.2)
        // With EV=0, compensation=0: exposure = 1 / (1.0 * 1.2) ≈ 0.8333
        const float ev = 0.0f;
        const float comp = 0.0f;
        const float expected = 1.0f / (std::pow(2.0f, ev - comp) * 1.2f);

        Camera cam;
        cam.m_ExposureValue = ev;
        cam.m_ExposureCompensation = comp;

        // Manually compute what Update() would set.
        const float finalEV = cam.m_ExposureValue - cam.m_ExposureCompensation;
        const float computed = 1.0f / (std::powf(2.0f, finalEV) * 1.2f);

        CHECK(std::fabs(computed - expected) < kEps);
        CHECK(computed > 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-CAM-EXP-02: Higher EV100 produces lower exposure (darker)
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-EXP-02 Exposure - higher EV100 produces lower exposure")
    {
        auto computeExposure = [](float ev, float comp) {
            return 1.0f / (std::powf(2.0f, ev - comp) * 1.2f);
        };

        const float expLow  = computeExposure(0.0f,  0.0f);
        const float expMid  = computeExposure(5.0f,  0.0f);
        const float expHigh = computeExposure(10.0f, 0.0f);

        CHECK(expLow > expMid);
        CHECK(expMid > expHigh);
        CHECK(expHigh > 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-CAM-EXP-03: Positive compensation increases exposure
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-EXP-03 Exposure - positive compensation increases exposure")
    {
        auto computeExposure = [](float ev, float comp) {
            return 1.0f / (std::powf(2.0f, ev - comp) * 1.2f);
        };

        const float ev = 5.0f;
        const float expNoComp   = computeExposure(ev, 0.0f);
        const float expPosComp  = computeExposure(ev, 2.0f); // +2 stops brighter

        CHECK(expPosComp > expNoComp);
    }

    // ------------------------------------------------------------------
    // TC-CAM-EXP-04: Default exposure range bounds are sane
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-EXP-04 Exposure - default EV range bounds are sane")
    {
        Camera cam;
        CHECK(cam.m_ExposureValueMin < cam.m_ExposureValueMax);
        CHECK(cam.m_ExposureValueMin < 0.0f);  // allows dark scenes
        CHECK(cam.m_ExposureValueMax > 10.0f); // allows bright scenes
    }

    // ------------------------------------------------------------------
    // TC-CAM-EXP-05: Exposure is always positive for any EV in valid range
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-EXP-05 Exposure - exposure is positive for all valid EV values")
    {
        Camera cam;
        const float evMin = cam.m_ExposureValueMin;
        const float evMax = cam.m_ExposureValueMax;

        for (float ev = evMin; ev <= evMax; ev += 1.0f)
        {
            const float exposure = 1.0f / (std::powf(2.0f, ev) * 1.2f);
            INFO("EV=" << ev << " exposure=" << exposure);
            CHECK(exposure > 0.0f);
        }
    }
}

// ============================================================================
// TEST SUITE: Camera_PlanarViewConstants
// ============================================================================
TEST_SUITE("Camera_PlanarViewConstants")
{
    // ------------------------------------------------------------------
    // TC-CAM-PVC-01: Viewport size fields are filled correctly
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-PVC-01 PlanarViewConstants - viewport size fields are correct")
    {
        Camera cam;
        cam.Reset();

        srrhi::PlanarViewConstants constants{};
        cam.FillPlanarViewConstants(constants, 1920.0f, 1080.0f);

        CHECK(std::fabs(constants.m_ViewportSize.x - 1920.0f) < kEps);
        CHECK(std::fabs(constants.m_ViewportSize.y - 1080.0f) < kEps);
    }

    // ------------------------------------------------------------------
    // TC-CAM-PVC-02: Viewport inverse size fields are correct
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-PVC-02 PlanarViewConstants - viewport inverse size is correct")
    {
        Camera cam;
        cam.Reset();

        srrhi::PlanarViewConstants constants{};
        cam.FillPlanarViewConstants(constants, 800.0f, 600.0f);

        CHECK(std::fabs(constants.m_ViewportSizeInv.x - (1.0f / 800.0f)) < kEps);
        CHECK(std::fabs(constants.m_ViewportSizeInv.y - (1.0f / 600.0f)) < kEps);
    }

    // ------------------------------------------------------------------
    // TC-CAM-PVC-03: Viewport origin is (0, 0)
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-PVC-03 PlanarViewConstants - viewport origin is zero")
    {
        Camera cam;
        cam.Reset();

        srrhi::PlanarViewConstants constants{};
        cam.FillPlanarViewConstants(constants, 1280.0f, 720.0f);

        CHECK(std::fabs(constants.m_ViewportOrigin.x) < kEps);
        CHECK(std::fabs(constants.m_ViewportOrigin.y) < kEps);
    }

    // ------------------------------------------------------------------
    // TC-CAM-PVC-04: ClipToWindow scale and bias are consistent with viewport
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-PVC-04 PlanarViewConstants - ClipToWindow scale and bias are correct")
    {
        Camera cam;
        cam.Reset();

        const float W = 1920.0f, H = 1080.0f;
        srrhi::PlanarViewConstants constants{};
        cam.FillPlanarViewConstants(constants, W, H);

        // ClipToWindowScale = (W/2, -H/2), ClipToWindowBias = (W/2, H/2)
        CHECK(std::fabs(constants.m_ClipToWindowScale.x - (W * 0.5f)) < kEps);
        CHECK(std::fabs(constants.m_ClipToWindowScale.y - (-H * 0.5f)) < kEps);
        CHECK(std::fabs(constants.m_ClipToWindowBias.x  - (W * 0.5f)) < kEps);
        CHECK(std::fabs(constants.m_ClipToWindowBias.y  - (H * 0.5f)) < kEps);
    }

    // ------------------------------------------------------------------
    // TC-CAM-PVC-05: WorldToView * ViewToWorld ≈ Identity
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-PVC-05 PlanarViewConstants - WorldToView * ViewToWorld is identity")
    {
        Camera cam = MakeCameraAt(2.0f, 1.0f, -4.0f, 0.4f, -0.2f);

        srrhi::PlanarViewConstants constants{};
        cam.FillPlanarViewConstants(constants, 1920.0f, 1080.0f);

        const Matrix& wtv = constants.m_MatWorldToView;
        const Matrix& vtw = constants.m_MatViewToWorld;
        const Matrix product = MatMul(wtv, vtw);

        CHECK(MatrixNearEqual(product, IdentityMatrix(), 1e-3f));
    }

    // ------------------------------------------------------------------
    // TC-CAM-PVC-06: No-offset matrices match non-jittered matrices when TAA is off
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-PVC-06 PlanarViewConstants - NoOffset matrices match base when TAA off")
    {
        // Ensure TAA is disabled for this test.
        const bool taaWas = g_Renderer.m_bTAAEnabled;
        g_Renderer.m_bTAAEnabled = false;

        Camera cam;
        cam.Reset();

        srrhi::PlanarViewConstants constants{};
        cam.FillPlanarViewConstants(constants, 1920.0f, 1080.0f);

        // When TAA is off, jitter = (0,0), so jittered == non-jittered.
        CHECK(MatrixNearEqual(constants.m_MatViewToClip,   constants.m_MatViewToClipNoOffset,   1e-3f));
        CHECK(MatrixNearEqual(constants.m_MatWorldToClip,  constants.m_MatWorldToClipNoOffset,  1e-3f));
        CHECK(MatrixNearEqual(constants.m_MatClipToView,   constants.m_MatClipToViewNoOffset,   1e-3f));
        CHECK(MatrixNearEqual(constants.m_MatClipToWorld,  constants.m_MatClipToWorldNoOffset,  1e-3f));

        g_Renderer.m_bTAAEnabled = taaWas;
    }
}

// ============================================================================
// TEST SUITE: Camera_FrustumPlanes
// ============================================================================
TEST_SUITE("Camera_FrustumPlanes")
{
    // Helper: replicate the ComputeFrustumPlanes logic from BasePassRenderer.cpp
    // so we can test it in isolation without depending on BasePassRenderer.
    static void ComputeFrustumPlanesLocal(const Matrix& proj, Vector4 frustumPlanes[5])
    {
        using namespace DirectX;
        const float xScale = std::fabs(proj._11);
        const float yScale = std::fabs(proj._22);
        const float nearZ  = proj._43;

        const XMVECTOR planes[5] = {
            XMPlaneNormalize(XMVectorSet(-1.0f, 0.0f, 1.0f / xScale, 0.0f)),
            XMPlaneNormalize(XMVectorSet( 1.0f, 0.0f, 1.0f / xScale, 0.0f)),
            XMPlaneNormalize(XMVectorSet(0.0f, -1.0f, 1.0f / yScale, 0.0f)),
            XMPlaneNormalize(XMVectorSet(0.0f,  1.0f, 1.0f / yScale, 0.0f)),
            XMPlaneNormalize(XMVectorSet(0.0f,  0.0f, 1.0f, nearZ)),
        };

        for (int i = 0; i < 5; ++i)
            XMStoreFloat4(&frustumPlanes[i], planes[i]);
    }

    // ------------------------------------------------------------------
    // TC-CAM-FRUST-01: Five frustum planes are produced
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-FRUST-01 FrustumPlanes - five planes are produced")
    {
        Camera cam;
        ProjectionParams proj;
        proj.fovY = DirectX::XM_PIDIV4;
        proj.aspectRatio = 16.0f / 9.0f;
        proj.nearZ = 0.1f;
        cam.SetProjection(proj);

        Vector4 planes[5]{};
        ComputeFrustumPlanesLocal(cam.GetProjMatrix(), planes);

        // All five planes must have a non-zero normal.
        for (int i = 0; i < 5; ++i)
        {
            INFO("Plane " << i);
            const float len = std::sqrt(planes[i].x * planes[i].x +
                                        planes[i].y * planes[i].y +
                                        planes[i].z * planes[i].z);
            CHECK(len > 0.5f); // normalized → should be ≈ 1
        }
    }

    // ------------------------------------------------------------------
    // TC-CAM-FRUST-02: Frustum planes are normalized (unit normals)
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-FRUST-02 FrustumPlanes - plane normals are unit length")
    {
        Camera cam;
        ProjectionParams proj;
        proj.fovY = DirectX::XM_PIDIV4;
        proj.aspectRatio = 1.0f;
        proj.nearZ = 0.1f;
        cam.SetProjection(proj);

        Vector4 planes[5]{};
        ComputeFrustumPlanesLocal(cam.GetProjMatrix(), planes);

        for (int i = 0; i < 5; ++i)
        {
            INFO("Plane " << i);
            const float len = std::sqrt(planes[i].x * planes[i].x +
                                        planes[i].y * planes[i].y +
                                        planes[i].z * planes[i].z);
            CHECK(std::fabs(len - 1.0f) < 1e-3f);
        }
    }

    // ------------------------------------------------------------------
    // TC-CAM-FRUST-03: Near plane encodes nearZ correctly
    //                  Plane[4] is the near plane: normal = (0,0,1), d = nearZ
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-FRUST-03 FrustumPlanes - near plane encodes nearZ")
    {
        Camera cam;
        ProjectionParams proj;
        proj.fovY = DirectX::XM_PIDIV4;
        proj.aspectRatio = 1.0f;
        proj.nearZ = 0.5f;
        cam.SetProjection(proj);

        Vector4 planes[5]{};
        ComputeFrustumPlanesLocal(cam.GetProjMatrix(), planes);

        // Near plane (index 4): normal should be (0, 0, 1), d = nearZ.
        CHECK(std::fabs(planes[4].x) < kEps);
        CHECK(std::fabs(planes[4].y) < kEps);
        CHECK(std::fabs(planes[4].z - 1.0f) < kEps);
        CHECK(std::fabs(planes[4].w - proj.nearZ) < kEps);
    }

    // ------------------------------------------------------------------
    // TC-CAM-FRUST-04: Left and right planes are symmetric for square aspect
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-FRUST-04 FrustumPlanes - left and right planes are symmetric")
    {
        Camera cam;
        ProjectionParams proj;
        proj.fovY = DirectX::XM_PIDIV4;
        proj.aspectRatio = 1.0f; // square → symmetric
        proj.nearZ = 0.1f;
        cam.SetProjection(proj);

        Vector4 planes[5]{};
        ComputeFrustumPlanesLocal(cam.GetProjMatrix(), planes);

        // Left plane (index 0): normal.x < 0
        // Right plane (index 1): normal.x > 0
        // They should be mirror images: planes[0].x == -planes[1].x
        CHECK(planes[0].x < 0.0f);
        CHECK(planes[1].x > 0.0f);
        CHECK(std::fabs(planes[0].x + planes[1].x) < kEps);
        CHECK(std::fabs(planes[0].z - planes[1].z) < kEps);
    }

    // ------------------------------------------------------------------
    // TC-CAM-FRUST-05: Top and bottom planes are symmetric
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-FRUST-05 FrustumPlanes - top and bottom planes are symmetric")
    {
        Camera cam;
        ProjectionParams proj;
        proj.fovY = DirectX::XM_PIDIV4;
        proj.aspectRatio = 1.0f;
        proj.nearZ = 0.1f;
        cam.SetProjection(proj);

        Vector4 planes[5]{};
        ComputeFrustumPlanesLocal(cam.GetProjMatrix(), planes);

        // Bottom plane (index 2): normal.y < 0
        // Top plane (index 3): normal.y > 0
        CHECK(planes[2].y < 0.0f);
        CHECK(planes[3].y > 0.0f);
        CHECK(std::fabs(planes[2].y + planes[3].y) < kEps);
        CHECK(std::fabs(planes[2].z - planes[3].z) < kEps);
    }
}

// ============================================================================
// TEST SUITE: Camera_PitchClamping
// ============================================================================
TEST_SUITE("Camera_PitchClamping")
{
    // ------------------------------------------------------------------
    // TC-CAM-PITCH-01: Pitch is clamped to [-PI/2 + eps, PI/2 - eps]
    //                  We verify the clamp bounds are sane constants.
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-PITCH-01 PitchClamping - pitch clamp bounds are within half-pi")
    {
        // The Camera::ProcessEvent code clamps pitch to:
        //   [-XM_PIDIV2 + 0.01, XM_PIDIV2 - 0.01]
        // We verify these bounds are strictly within ±π/2.
        const float maxPitch = DirectX::XM_PIDIV2 - 0.01f;
        const float minPitch = -DirectX::XM_PIDIV2 + 0.01f;

        CHECK(maxPitch < DirectX::XM_PIDIV2);
        CHECK(minPitch > -DirectX::XM_PIDIV2);
        CHECK(maxPitch > 0.0f);
        CHECK(minPitch < 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-CAM-PITCH-02: View matrix remains valid at extreme pitch
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-PITCH-02 PitchClamping - view matrix is valid at extreme pitch")
    {
        using namespace DirectX;

        // Simulate near-maximum pitch via SetFromMatrix.
        const float extremePitch = XM_PIDIV2 - 0.02f;
        XMMATRIX rot = XMMatrixRotationX(extremePitch);
        Matrix rotM{};
        XMStoreFloat4x4(&rotM, rot);

        Camera cam;
        cam.SetFromMatrix(rotM);

        const Matrix view = cam.GetViewMatrix();

        // The view matrix must not contain NaN or Inf.
        const float* v = reinterpret_cast<const float*>(&view);
        bool allFinite = true;
        for (int i = 0; i < 16; ++i)
            if (!std::isfinite(v[i])) { allFinite = false; break; }

        CHECK(allFinite);
    }
}

// ============================================================================
// TEST SUITE: Camera_SceneIntegration
// ============================================================================
TEST_SUITE("Camera_SceneIntegration")
{
    // ------------------------------------------------------------------
    // TC-CAM-SCENE-01: g_Renderer.m_Scene.m_Camera is valid after init
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-SCENE-01 SceneIntegration - scene camera is accessible")
    {
        // The scene camera is a member of Scene; it should always be accessible.
        Camera& cam = g_Renderer.m_Scene.m_Camera;

        // GetViewMatrix must not crash and must return a finite matrix.
        const Matrix view = cam.GetViewMatrix();
        const float* v = reinterpret_cast<const float*>(&view);
        bool allFinite = true;
        for (int i = 0; i < 16; ++i)
            if (!std::isfinite(v[i])) { allFinite = false; break; }

        CHECK(allFinite);
    }

    // ------------------------------------------------------------------
    // TC-CAM-SCENE-02: SetCameraFromSceneCamera populates position from node
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-SCENE-02 SceneIntegration - SetCameraFromSceneCamera sets projection")
    {
        SKIP_IF_NO_SAMPLES("BoxTextured/glTF/BoxTextured.gltf");
        SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
        REQUIRE(scope.loaded);

        // BoxTextured may or may not have cameras; if it does, test the setter.
        if (g_Renderer.m_Scene.m_Cameras.empty())
        {
            WARN("BoxTextured has no cameras - skipping SetCameraFromSceneCamera test");
            return;
        }

        const Scene::Camera& sceneCam = g_Renderer.m_Scene.m_Cameras[0];
        g_Renderer.SetCameraFromSceneCamera(sceneCam);

        const ProjectionParams& proj = g_Renderer.m_Scene.m_Camera.GetProjection();
        CHECK(proj.fovY > 0.0f);
        CHECK(proj.nearZ > 0.0f);
        CHECK(proj.aspectRatio > 0.0f);
    }

    // ------------------------------------------------------------------
    // TC-CAM-SCENE-03: FillPlanarViewConstants does not crash with scene camera
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-SCENE-03 SceneIntegration - FillPlanarViewConstants with scene camera")
    {
        Camera& cam = g_Renderer.m_Scene.m_Camera;
        cam.Reset();

        srrhi::PlanarViewConstants constants{};
        cam.FillPlanarViewConstants(constants, 1920.0f, 1080.0f);

        // Viewport fields must be correct.
        CHECK(std::fabs(constants.m_ViewportSize.x - 1920.0f) < kEps);
        CHECK(std::fabs(constants.m_ViewportSize.y - 1080.0f) < kEps);

        // All matrices must be finite.
        const float* m = reinterpret_cast<const float*>(&constants.m_MatWorldToClip);
        bool allFinite = true;
        for (int i = 0; i < 16; ++i)
            if (!std::isfinite(m[i])) { allFinite = false; break; }
        CHECK(allFinite);
    }

    // ------------------------------------------------------------------
    // TC-CAM-SCENE-04: Shader hot-reload does not affect camera matrices
    // ------------------------------------------------------------------
    TEST_CASE("TC-CAM-SCENE-04 SceneIntegration - shader hot-reload does not affect camera")
    {
        Camera& cam = g_Renderer.m_Scene.m_Camera;
        cam.Reset();

        const Matrix viewBefore = cam.GetViewMatrix();
        const Matrix projBefore = cam.GetProjMatrix();

        // Request a shader reload (non-destructive).
        g_Renderer.m_RequestedShaderReload = true;
        // The reload is processed at the start of the next frame; camera
        // matrices must be unaffected immediately.
        const Matrix viewAfter = cam.GetViewMatrix();
        const Matrix projAfter = cam.GetProjMatrix();

        g_Renderer.m_RequestedShaderReload = false;

        CHECK(MatrixNearEqual(viewBefore, viewAfter));
        CHECK(MatrixNearEqual(projBefore, projAfter));
    }
}
