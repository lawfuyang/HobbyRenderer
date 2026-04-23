#pragma once

// ============================================================================
// TestFixtures.h — Shared test infrastructure for HobbyRenderer test suite
//
// This header is included by ALL test files (Tests_CoreBoot.cpp, etc.).
// It does NOT define DOCTEST_CONFIG_IMPLEMENT — that is done exactly once
// in TestMain.cpp.
//
// Usage:
//   #include "TestFixtures.h"
//   TEST_CASE("MyTest") { CHECK(1 + 1 == 2); }
// ============================================================================

// doctest.h without the implementation macro — just the test macros.
#include "../external/doctest.h"

#include "../CommonResources.h"
#include "../Config.h"
#include "../GraphicRHI.h"
#include "../Renderer.h"
#include "../TaskScheduler.h"
#include "../Utilities.h"
#include "../SceneLoader.h"
#include "../TextureLoader.h"

// Convenience alias
static CommonResources& CR() { return CommonResources::GetInstance(); }
static nvrhi::IDevice*  DEV() { return g_Renderer.m_RHI->m_NvrhiDevice; }

// ============================================================================
// Helper: tiny RAII wrapper that resets Config to defaults after a test
// ============================================================================
struct ConfigGuard
{
    // Snapshot the current singleton state
    Config snapshot = Config::Get();

    ~ConfigGuard()
    {
        // Restore via the private s_Instance — we access it through ParseCommandLine
        // with a no-op argv so we just re-assign the snapshot directly.
        // Since Config::s_Instance is inline and accessible via the header we
        // reach it through the public Get() reference cast.
        const_cast<Config&>(Config::Get()) = snapshot;
    }
};

// Returns the glTF-Sample-Assets root path from Config, or "" if not set.
inline std::string GltfSamplesRoot()
{
    return Config::Get().m_GltfSamplesPath;
}

// Build a full path to a model inside the glTF-Sample-Assets repo.
// e.g. GltfSampleModel("BoxTextured/glTF/BoxTextured.gltf")
std::string GltfSampleModel(const char* relPath);

// Returns true if the glTF-Sample-Assets path is configured AND the given
// model file actually exists on disk.
bool SampleModelExists(const char* relPath);

// ============================================================================
// RAII helper: loads a scene into g_Renderer.m_Scene for the duration of a
// test, then shuts it down cleanly on destruction.
//
// Usage:
//   SceneScope scope("BoxTextured/glTF/BoxTextured.gltf");
//   REQUIRE(scope.loaded);
//   CHECK(g_Renderer.m_Scene.m_Meshes.size() > 0);
// ============================================================================
struct SceneScope
{
    bool loaded = false;

    explicit SceneScope(const char* modelRelPath, bool skipCache = true);
    ~SceneScope();

    // Non-copyable
    SceneScope(const SceneScope&) = delete;
    SceneScope& operator=(const SceneScope&) = delete;
};

// Macro: skip the entire test if glTF-Sample-Assets path is not configured
// or the specific model file is missing.
#define SKIP_IF_NO_SAMPLES(modelRelPath)                                         \
    do {                                                                        \
        if (GltfSamplesRoot().empty())                                          \
        {                                                                       \
            WARN("Skipping: --gltf-samples not provided");                      \
            return;                                                             \
        }                                                                       \
        if (!SampleModelExists(modelRelPath))                                   \
        {                                                                       \
            WARN("Skipping: model not found");                   \
            return;                                                             \
        }                                                                       \
    } while (0)

// Returns the absolute path to a file inside src/Tests/ReferenceImages/.
// Uses SDL_GetBasePath() to locate the executable directory, then walks up
// to find the source tree.  Falls back to a path relative to the CWD.
std::filesystem::path ReferenceImagePath(const char* filename);

// Helper: create a minimal GPU texture from raw RGBA8 pixel data (CPU-only path).
// Returns a valid nvrhi::TextureHandle or nullptr on failure.
nvrhi::TextureHandle CreateTestTexture2D(uint32_t width, uint32_t height, nvrhi::Format format, const void* initialData, size_t rowPitch, const char* debugName = "TestTexture");

// Find a renderer by name (returns nullptr if not found).
IRenderer* FindRendererByName(const char* name);

// -----------------------------------------------------------------------
// RunOneFrame — executes a single full rendering frame using the actual
// Renderer pipeline against the hidden test swapchain.
//
// The caller is responsible for having a valid scene loaded (or not, for
// empty-scene frames).  The function:
//   1. Resets all renderer pass-enabled flags
//   2. Resets the RenderGraph
//   3. Schedules the standard Normal-mode renderer chain
//   4. Compiles the RenderGraph
//   5. Executes all scheduled tasks (records command lists)
//   6. Flushes pending command lists to the GPU
//   7. Waits for GPU idle
//
// Returns true if no exception was thrown.
// -----------------------------------------------------------------------
bool RunOneFrame();

// -----------------------------------------------------------------------
// ReadbackTexelFloat — reads a single texel from a GPU texture via a
// staging texture.  Returns the R-channel value as a float.
// Only valid for single-channel float formats (R32_FLOAT, R16_FLOAT, …).
// -----------------------------------------------------------------------
float ReadbackTexelFloat(nvrhi::TextureHandle tex, uint32_t x, uint32_t y);

// -----------------------------------------------------------------------
// ReadbackTexelRGBA8 — reads a single RGBA8_UNORM texel.
// Returns the packed uint32 (R in bits 0-7, G in 8-15, B in 16-23, A in 24-31).
// -----------------------------------------------------------------------
uint32_t ReadbackTexelRGBA8(nvrhi::TextureHandle tex, uint32_t x, uint32_t y);

// ============================================================================
// MinimalSceneFixture — doctest fixture for tests that call RunOneFrame.
//
// Sets up a minimal, valid Scene + Graphic state before each test and tears
// it down cleanly afterwards.  Use with TEST_CASE_FIXTURE:
//
//   TEST_CASE_FIXTURE(MinimalSceneFixture, "TC-FF-01 ...")
//   {
//       CHECK_NOTHROW(RunOneFrame());
//   }
//
// What the fixture guarantees:
//   - Any previously loaded scene is shut down (GPU idle + Shutdown()).
//   - g_Renderer.m_Scene is in a clean, empty state.
//   - g_Renderer.m_RenderGraph is cleared before and after each test so no
//     transient or persistent RG resources leak across doctest cases.
//   - Exactly one directional light exists in the scene (via
//     Scene::EnsureDefaultDirectionalLight) so renderers that read
//     m_Lights.back() do not crash on an empty light list.
//   - On destruction the scene is shut down again (GPU idle + Shutdown()).
// ============================================================================
struct MinimalSceneFixture
{
    MinimalSceneFixture();
    ~MinimalSceneFixture();

    // Non-copyable
    MinimalSceneFixture(const MinimalSceneFixture&) = delete;
    MinimalSceneFixture& operator=(const MinimalSceneFixture&) = delete;
};
