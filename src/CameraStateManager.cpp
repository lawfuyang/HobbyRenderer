#include "CameraStateManager.h"

namespace
{
    // ── JSON helpers ────────────────────────────────────────────────────────

    void SkipWhitespace(std::string_view s, size_t& i)
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }

    void ExpectChar(std::string_view s, size_t& i, char c)
    {
        SkipWhitespace(s, i);
        SDL_assert(i < s.size() && s[i] == c);
        ++i;
    }

    // Read a JSON string (no escape decoding — our keys never contain escapes).
    std::string ReadString(std::string_view s, size_t& i)
    {
        SkipWhitespace(s, i);
        SDL_assert(i < s.size() && s[i] == '"');
        ++i;
        size_t start = i;
        while (i < s.size() && s[i] != '"')
            ++i;
        SDL_assert(i < s.size());
        std::string result = std::string(s.substr(start, i - start));
        ++i;
        return result;
    }

    float ReadNumber(std::string_view s, size_t& i)
    {
        SkipWhitespace(s, i);
        size_t end;
        float v = std::stof(std::string(s.substr(i)), &end);
        i += end;
        return v;
    }

    // Escape a string for JSON output (handles " and \).
    std::string EscapeJson(std::string_view str)
    {
        std::string out;
        out.reserve(str.size() + 2);
        for (char c : str)
        {
            if (c == '"')      out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else                out += c;
        }
        return out;
    }

    // Read the entire contents of a file. Returns empty string on failure.
    std::string ReadWholeFile(std::string_view path)
    {
        std::ifstream f(std::string(path), std::ios::binary | std::ios::ate);
        if (!f.is_open())
            return {};
        size_t size = static_cast<size_t>(f.tellg());
        if (size == 0)
            return {};
        std::string content(size, '\0');
        f.seekg(0);
        f.read(content.data(), static_cast<std::streamsize>(size));
        return content;
    }

    // Canonical path key — every Save / Load / SetScenePath calls this inline.
    std::string NormalizePath(std::string_view path)
    {
        return std::filesystem::absolute(std::filesystem::path(path))
            .lexically_normal()
            .generic_string();
    }

    // Parse all entries from the JSON string into a map.
    // Asserts on parse errors (corrupted JSON).
    std::map<std::string, CameraSavedState> ParseAllEntries(std::string_view json)
    {
        std::map<std::string, CameraSavedState> result;

        if (json.empty())
            return result;

        size_t i = 0;
        SkipWhitespace(json, i);
        if (i >= json.size())
            return result;

        // Root object
        SDL_assert(json[i] == '{' && "camera_state.json: expected '{' at root");
        ++i;

        // Expect "scenes" key
        std::string rootKey = ReadString(json, i);
        SDL_assert(rootKey == "scenes");
        ExpectChar(json, i, ':');
        ExpectChar(json, i, '{');

        // Parse scene entries
        for (;;)
        {
            SkipWhitespace(json, i);
            if (i >= json.size() || json[i] == '}')
            {
                if (i < json.size() && json[i] == '}')
                    ++i;
                break;
            }

            // Read entry key (scene path)
            std::string entryKey = ReadString(json, i);
            ExpectChar(json, i, ':');
            ExpectChar(json, i, '{');

            CameraSavedState state;
            int fieldsFound = 0;

            // Parse fields: "position", "yaw", "pitch" (any order)
            for (;;)
            {
                SkipWhitespace(json, i);
                if (i >= json.size() || json[i] == '}')
                {
                    if (i < json.size() && json[i] == '}')
                        ++i;
                    break;
                }

                std::string fieldName = ReadString(json, i);
                ExpectChar(json, i, ':');

                if (fieldName == "position")
                {
                    ExpectChar(json, i, '[');
                    state.position.x = ReadNumber(json, i);
                    ExpectChar(json, i, ',');
                    state.position.y = ReadNumber(json, i);
                    ExpectChar(json, i, ',');
                    state.position.z = ReadNumber(json, i);
                    ExpectChar(json, i, ']');
                    fieldsFound |= 1;
                }
                else if (fieldName == "yaw")
                {
                    state.yaw = ReadNumber(json, i);
                    fieldsFound |= 2;
                }
                else if (fieldName == "pitch")
                {
                    state.pitch = ReadNumber(json, i);
                    fieldsFound |= 4;
                }
                else
                {
                    SDL_LOG_ASSERT_FAIL("Unknown field in camera_state.json",
                        "[CameraState] Unknown field: %s", fieldName.c_str());
                }

                SkipWhitespace(json, i);
                if (i < json.size() && json[i] == ',')
                    ++i;
            }

            SDL_assert(fieldsFound == 7 && "camera_state.json: entry missing required fields");

            result[entryKey] = state;

            SkipWhitespace(json, i);
            if (i < json.size() && json[i] == ',')
                ++i;
        }

        return result;
    }

    // Write all entries to a file as pretty-printed JSON.
    void WriteAllEntries(std::string_view path, const std::map<std::string, CameraSavedState>& allStates)
    {
        std::string tmpPath = std::string(path) + ".tmp";

        FILE* f = fopen(tmpPath.c_str(), "w");
        if (!f)
        {
            SDL_Log("[CameraState] Failed to open %s for writing", tmpPath.c_str());
            return;
        }

        fprintf(f, "{\n");
        fprintf(f, "\t\"scenes\": {\n");

        bool first = true;
        for (const auto& [scenePath, state] : allStates)
        {
            if (!first)
                fprintf(f, ",\n");
            first = false;

            fprintf(f, "\t\t\"%s\": {\n", EscapeJson(scenePath).c_str());
            fprintf(f, "\t\t\t\"position\": [\n");
            fprintf(f, "\t\t\t\t%.6g,\n", state.position.x);
            fprintf(f, "\t\t\t\t%.6g,\n", state.position.y);
            fprintf(f, "\t\t\t\t%.6g\n", state.position.z);
            fprintf(f, "\t\t\t],\n");
            fprintf(f, "\t\t\t\"yaw\": %.6g,\n", state.yaw);
            fprintf(f, "\t\t\t\"pitch\": %.6g\n", state.pitch);
            fprintf(f, "\t\t}");
        }

        fprintf(f, "\n\t}\n");
        fprintf(f, "}\n");
        fclose(f);

        std::error_code ec;
        std::filesystem::rename(tmpPath, std::string(path), ec);
        if (ec)
        {
            SDL_Log("[CameraState] Failed to rename %s to %s: %s", tmpPath.c_str(), std::string(path).c_str(), ec.message().c_str());
        }
    }
} // anonymous namespace

// ── CameraStateManager ────────────────────────────────────────────────────────

void CameraStateManager::Initialize()
{
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    SDL_assert(len > 0 && len < MAX_PATH);

    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    m_JsonPath = (exeDir / "camera_state.json").string();

    SDL_Log("[CameraState] Initialized, JSON path: %s", m_JsonPath.c_str());

    StartAsyncWorker();
}

void CameraStateManager::SetScenePath(std::string_view scenePath)
{
    m_CurrentScenePath = NormalizePath(scenePath);
}

void CameraStateManager::Update(const CameraSavedState& currentState)
{
    if (m_CurrentScenePath.empty())
        return;

    // ── Minimal critical section (held for ~10-15 ns) ──────────────────
    // The worker holds this spinlock for nanoseconds once per second, so the
    // render thread will almost never spin.
    while (m_PendingLock.test_and_set(std::memory_order_acquire))
    {
        // Spin — rare in practice (worker lock hold time <1 µs).
    }

    m_PendingState = currentState;
    m_PendingDirty = true;

    m_PendingLock.clear(std::memory_order_release);
    // ── End critical section ────────────────────────────────────────────
}

void CameraStateManager::SaveCamera(std::string_view scenePath, const CameraSavedState& state)
{
    std::string key = NormalizePath(scenePath);

    std::string json = ReadWholeFile(m_JsonPath);
    std::map<std::string, CameraSavedState> allStates = ParseAllEntries(json);

    allStates[key] = state;

    WriteAllEntries(m_JsonPath, allStates);
}

bool CameraStateManager::LoadCamera(std::string_view scenePath, CameraSavedState& outState) const
{
    std::string key = NormalizePath(scenePath);

    std::string json = ReadWholeFile(m_JsonPath);
    std::map<std::string, CameraSavedState> allStates = ParseAllEntries(json);

    auto it = allStates.find(key);
    if (it != allStates.end())
    {
        outState = it->second;
        return true;
    }

    return false;
}

// ── Async worker thread ─────────────────────────────────────────────────────

CameraStateManager::~CameraStateManager()
{
    StopAsyncWorker();
}

void CameraStateManager::StartAsyncWorker()
{
    if (m_WorkerThread.joinable())
        return; // already running

    m_StopRequested.store(false, std::memory_order_relaxed);
    m_WorkerThread = std::thread(&CameraStateManager::WorkerLoop, this);
}

void CameraStateManager::StopAsyncWorker()
{
    m_StopRequested.store(true, std::memory_order_release);

    if (m_WorkerThread.joinable())
    {
        m_WorkerThread.join();
    }
}

void CameraStateManager::WorkerLoop()
{
    SDL_Log("[CameraState] Async worker thread started");

    while (!m_StopRequested.load(std::memory_order_relaxed))
    {
        // Sleep for the throttle interval (1 s).
        // Using a simple sleep avoids condition-variable overhead on the
        // render thread — the render side only does a spinlock + memcpy.
        std::this_thread::sleep_for(std::chrono::seconds(1));

        if (m_StopRequested.load(std::memory_order_relaxed))
            break;

        CameraSavedState state;
        bool             hasWork = false;

        // ── Grab pending state under lock ───────────────────────────────
        {
            while (m_PendingLock.test_and_set(std::memory_order_acquire))
            {
                // Spin — render holds this for nanoseconds.
            }

            if (m_PendingDirty)
            {
                state          = m_PendingState;
                m_PendingDirty = false;
                hasWork        = true;
            }

            m_PendingLock.clear(std::memory_order_release);
        }
        // ── Lock released; I/O work begins ──────────────────────────────

        if (!hasWork)
            continue;

        // ── Skip write if camera hasn't moved significantly ────────────
        if (m_HasLastSaved)
        {
            float dx = state.position.x - m_LastSavedState.position.x;
            float dy = state.position.y - m_LastSavedState.position.y;
            float dz = state.position.z - m_LastSavedState.position.z;
            float distSq = dx * dx + dy * dy + dz * dz;

            if (distSq < 1e-6f &&
                std::abs(state.yaw   - m_LastSavedState.yaw)   < 1e-4f &&
                std::abs(state.pitch - m_LastSavedState.pitch) < 1e-4f)
                continue;
        }

        // ── Perform I/O (read-merge-write, no lock held) ───────────────
        SaveCamera(m_CurrentScenePath, state);
        m_LastSavedState = state;
        m_HasLastSaved   = true;
    }

    SDL_Log("[CameraState] Async worker thread stopped");
}
