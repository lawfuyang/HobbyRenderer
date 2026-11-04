#pragma once

// In-application RenderDoc captures.
//
// RenderDoc is normally injected from its own UI or from renderdoccmd. Loading renderdoc.dll from the
// application works just as well, but only if it happens BEFORE the D3D12 device exists: RenderDoc hooks
// D3D12CreateDevice through the importing module's import table, so a device created beforehand stays
// invisible to it. main() therefore loads the DLL from the command line, ahead of Renderer::Initialize.
//
// A capture is a bracket around exactly one frame: StartFrameCapture before the frame's first command
// list, EndFrameCapture after the present. Two captures must never overlap.

class RenderDocCapture
{
public:
    // dllPath empty means "look in the usual places" (see the .cpp). Returns false, with a log line, when
    // no usable renderdoc.dll is found; every other member then degrades to a no-op.
    [[nodiscard]] bool Load(std::string_view dllPath);
    void Unload();

    [[nodiscard]] bool IsAvailable() const { return m_API != nullptr; }

    // False when the binary was built without RenderDoc support at all, i.e. no renderdoc_app.h was found when
    // CMake ran. A runtime Load() failure (no DLL) is a different state: support is compiled in, but unavailable.
    static constexpr bool IsCompiledIn()
    {
#if defined(HOBBY_RENDERER_WITH_RENDERDOC)
        return true;
#else
        return false;
#endif
    }

    // "<dir>/<stem>"; RenderDoc appends "_frame<N>.rdc" and creates the directory when it is missing.
    void SetCapturePathTemplate(std::string_view pathTemplate);

    // The renderer has a single device and a single window, which is the case RenderDoc documents for
    // passing a wildcard (null) device/window pair, so neither is plumbed through here.
    void BeginFrameCapture();
    [[nodiscard]] bool EndFrameCapture();

    // Absolute path of the newest capture RenderDoc knows about; empty when it reports none.
    [[nodiscard]] std::string GetNewestCapturePath() const;

private:
    HMODULE m_Module = nullptr;
    void*   m_API = nullptr; // RENDERDOC_API_1_6_0*, see renderdoc::API in the .cpp
};

extern RenderDocCapture g_RenderDoc;
