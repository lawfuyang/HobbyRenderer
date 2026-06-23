#pragma once

struct CameraSavedState
{
    Vector3 position;
    float   yaw = 0.0f;
    float   pitch = 0.0f;
};

// Manages per-scene camera-state persistence via camera_state.json in the binary directory.
// Owned by Renderer.  Periodic save (1 s throttled) + on-demand load.
class CameraStateManager
{
public:
    CameraStateManager() = default;

    // Resolves the binary directory via GetModuleFileNameA and sets the JSON file path.
    void Initialize();

    // Set the current scene path so periodic saves know which key to update.
    void SetScenePath(std::string_view scenePath);

    // Call once per frame with delta time (seconds) and current camera state.
    // Saves automatically every 1 s if the camera has moved since the last write.
    void Update(float dt, const CameraSavedState& currentState);

    // Atomically persist the camera state for the given (normalised) scene path.
    void SaveCamera(std::string_view scenePath, const CameraSavedState& state);

    // Look up a previously-saved camera state for the given scene path.
    // Returns false if the file doesn't exist or the path isn't present.
    bool LoadCamera(std::string_view scenePath, CameraSavedState& outState) const;

private:
    std::string       m_JsonPath;
    CameraSavedState  m_LastSavedState;
    bool              m_HasLastSaved   = false;
    float             m_AccumulatedDt  = 0.0f;
    std::string       m_CurrentScenePath;
};
