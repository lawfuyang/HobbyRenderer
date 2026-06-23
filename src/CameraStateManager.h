#pragma once

struct CameraSavedState
{
    Vector3 position;
    float   yaw = 0.0f;
    float   pitch = 0.0f;
};

// Manages per-scene camera-state persistence via camera_state.json.
// Owned by Renderer.
//
// Architecture:
//   Render thread  ──Update()──▶  [pending buffer]  ──WorkerLoop()──▶  JSON I/O
//                     (spinlock,                   (1 s sleep,
//                      ~20 byte copy)               compare + write)
//
// The render thread never touches the filesystem.  Update() only copies the
// current camera state into a spinlock-protected buffer — ~20 bytes, held for
// nanoseconds.  The background worker wakes every 1 s, grabs the latest state,
// compares against the last-saved state, and performs I/O only when the camera
// has moved significantly.
//
// LoadCamera() remains synchronous (called once during scene initialisation).
// SaveCamera() is synchronous for explicit flush during shutdown.
class CameraStateManager
{
public:
    ~CameraStateManager();

    // Resolve binary directory path and start the async worker thread.
    void Initialize();

    // Start / stop the background I/O worker thread.
    // StartAsyncWorker is called by Initialize(); call it again only if the
    // worker was previously stopped.
    void StartAsyncWorker();
    void StopAsyncWorker();

    // Set the current scene path (render-thread safe, lock-free).
    void SetScenePath(std::string_view scenePath);

    // Fast-path update (render thread, every frame).
    // Copies the current camera state into the pending buffer under a spinlock.
    // No I/O, no parsing, no allocations.  Scene path is immutable at runtime
    // so the worker reads it lock-free.
    void Update(const CameraSavedState& currentState);

    // Synchronous save — flushes camera state to disk immediately.
    // Used during shutdown (Scene::Shutdown) when the worker is already stopped.
    void SaveCamera(std::string_view scenePath, const CameraSavedState& state);

    // Synchronous load — reads camera state from disk.
    // Called during scene initialisation (not in hot path).
    bool LoadCamera(std::string_view scenePath, CameraSavedState& outState) const;

private:
    void WorkerLoop();

    // ── Thread management ──────────────────────────────────────────────────
    std::thread m_WorkerThread;
    std::atomic<bool> m_StopRequested{false};

    // ── Render→Worker sync ─────────────────────────────────────────────────
    // Spinlock + pending camera state.  The render thread holds this lock for
    // only the duration of a memcpy (~10-15 ns).  Scene path is immutable at
    // runtime so the worker reads m_CurrentScenePath lock-free.
    // Cache-line aligned to avoid false sharing with worker-private data.
    alignas(64) std::atomic_flag m_PendingLock = ATOMIC_FLAG_INIT;
    CameraSavedState m_PendingState;
    bool             m_PendingDirty = false;

    // ── Worker-private state (never accessed by render thread) ─────────────
    alignas(64) std::string m_JsonPath;
    CameraSavedState m_LastSavedState;
    bool             m_HasLastSaved = false;

    // ── Render-thread-only state (lock-free) ───────────────────────────────
    std::string m_CurrentScenePath;
};
