// PixProbe - reads data out of a PIX GPU capture through the PIX API (PixApi.h).
//
// pixtool.exe covers event lists, hardware counters, render targets and playback. This tool covers what the
// command line cannot reach, and it is the reference for "use the API instead of parsing CSVs":
//
//   PixProbe <capture.wpix> [--filter=<text>] [--limit=<n>] [--screenshot=<out.bmp>] [--pix-dir=<dir>]
//
//   file info        capture device, size, and every file-info string pair
//   queues/events    queue names, per-queue event counts, and the event tree (name, index, parent)
//   --screenshot     the capture's gold image, written as BMP (pixtool's save-screenshot fails here)
//
// The PIX installation is discovered at run time - never hardcoded:
//   1. --pix-dir=<dir>
//   2. the PIX_DIR environment variable
//   3. the newest "%ProgramFiles%\Microsoft PIX[\ Preview|\ Experimental]\<version>" that has include\PixApi.h,
//      compared by the numeric fields of YYMM.DD[.NNN] (a lexical sort puts 2606.9 after 2606.17).
//
// pixapi.dll is loaded by hand (PixCreateFactory is the only export needed), so linking does not require
// PixApi.lib and the install directory can be added to the DLL search path before the load.

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

// PixApi*.h reference D3D12 types (D3D12_FEATURE_DATA_DUMP_FILE, D3D12_DUMP_FILE_DRIVER_OPTIONS, ...) that the
// Windows SDK's d3d12.h does not define, so the Agility SDK headers go in first.
#include <d3d12.h>
#include <PixApi.h>

namespace
{
    // PIX reports this instead of a generic failure when the machine is not in Developer Mode.
    constexpr HRESULT kDeveloperModeNotEnabled = static_cast<HRESULT>(-1967390720);
    constexpr HRESULT kFeatureRequiresDeveloperMode = static_cast<HRESULT>(-1967390719);

    void PrintHresult(const char* what, HRESULT hr)
    {
        std::printf("%s failed: 0x%08X\n", what, static_cast<unsigned>(hr));
        if (hr == kDeveloperModeNotEnabled || hr == kFeatureRequiresDeveloperMode)
        {
            std::printf("Windows Developer Mode is required for this operation.\n"
                        "Enable it in Settings > Privacy & security > For developers.\n");
        }
    }

    std::vector<int> ParseVersion(const std::wstring& name)
    {
        // "2606.17", "2602.24.004-main", "2606.17-preview": numeric fields before the flavor suffix.
        std::wstring version = name;
        const size_t dash = version.find(L'-');
        if (dash != std::wstring::npos)
            version.resize(dash);

        std::vector<int> fields;
        int current = 0;
        bool bInField = false;
        for (const wchar_t c : version)
        {
            if (c >= L'0' && c <= L'9')
            {
                current = current * 10 + int(c - L'0');
                bInField = true;
            }
            else if (c == L'.')
            {
                if (!bInField)
                    return {};
                fields.push_back(current);
                current = 0;
                bInField = false;
            }
            else
            {
                return {};
            }
        }
        if (bInField)
            fields.push_back(current);
        return fields;
    }

    bool VersionLess(const std::vector<int>& a, const std::vector<int>& b)
    {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
    }

    std::wstring NewestInstallIn(const std::wstring& root)
    {
        std::wstring newest;
        std::vector<int> newestVersion;

        const std::wstring pattern = root + L"\\*";
        WIN32_FIND_DATAW findData{};
        HANDLE search = FindFirstFileW(pattern.c_str(), &findData);
        if (search == INVALID_HANDLE_VALUE)
            return {};

        do
        {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                continue;

            const std::wstring candidate = root + L"\\" + findData.cFileName;
            if (GetFileAttributesW((candidate + L"\\include\\PixApi.h").c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;

            const std::vector<int> version = ParseVersion(findData.cFileName);
            if (version.empty())
                continue;

            if (newest.empty() || VersionLess(newestVersion, version))
            {
                newest = candidate;
                newestVersion = version;
            }
        } while (FindNextFileW(search, &findData));

        FindClose(search);
        return newest;
    }

    std::wstring DiscoverPixDir(const std::wstring& overrideDir)
    {
        if (!overrideDir.empty())
            return overrideDir;

        wchar_t buffer[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"PIX_DIR", buffer, MAX_PATH) > 0)
            return buffer;

        const wchar_t* programFilesRoots[] = { L"ProgramW6432", L"ProgramFiles" };
        const wchar_t* flavors[] = { L"Microsoft PIX", L"Microsoft PIX Preview", L"Microsoft PIX Experimental" };

        std::wstring best;
        std::vector<int> bestVersion;
        for (const wchar_t* variable : programFilesRoots)
        {
            wchar_t programFiles[MAX_PATH]{};
            if (GetEnvironmentVariableW(variable, programFiles, MAX_PATH) == 0)
                continue;

            for (const wchar_t* flavor : flavors)
            {
                const std::wstring root = std::wstring(programFiles) + L"\\" + flavor;
                const std::wstring candidate = NewestInstallIn(root);
                if (candidate.empty())
                    continue;

                const std::wstring leaf = candidate.substr(candidate.find_last_of(L'\\') + 1);
                const std::vector<int> version = ParseVersion(leaf);
                if (best.empty() || VersionLess(bestVersion, version))
                {
                    best = candidate;
                    bestVersion = version;
                }
            }
        }
        return best;
    }

    struct PixApiLibrary
    {
        HMODULE m_Module = nullptr;
        HRESULT(WINAPI* m_CreateFactory)(REFIID, void**) = nullptr;

        ~PixApiLibrary()
        {
            if (m_Module)
                FreeLibrary(m_Module);
        }
    };

    // The document hands PIX-thread callbacks through this; a null pointer is not accepted by
    // OpenGpuCaptureDocument even though the parameter is documented as optional.
    class CaptureNotifications final : public IPixGpuCaptureNotifications
    {
    public:
        ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_RefCount); }

        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG count = InterlockedDecrement(&m_RefCount);
            if (count == 0)
                delete this;
            return count;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
        {
            if (!object)
                return E_POINTER;

            if (riid == __uuidof(IUnknown) || riid == __uuidof(IPixDocumentNotifications) ||
                riid == __uuidof(IPixGpuCaptureNotifications))
            {
                *object = static_cast<IPixGpuCaptureNotifications*>(this);
                AddRef();
                return S_OK;
            }

            *object = nullptr;
            return E_NOINTERFACE;
        }

        void STDMETHODCALLTYPE FatalError(HRESULT reason) override
        {
            std::printf("PIX reported a fatal document error: 0x%08X\n", static_cast<unsigned>(reason));
        }

    private:
        LONG m_RefCount = 1;
    };

    bool LoadPixApi(const std::wstring& pixDir, PixApiLibrary& out)
    {
        // pixapi.dll has ~15 sibling dependencies, so its own folder has to be searchable before the load.
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
        AddDllDirectory(pixDir.c_str());

        const std::wstring dllPath = pixDir + L"\\pixapi.dll";
        out.m_Module = LoadLibraryExW(dllPath.c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
        if (!out.m_Module)
        {
            std::printf("cannot load %ls (error %lu)\n", dllPath.c_str(), GetLastError());
            return false;
        }

        out.m_CreateFactory = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(
            GetProcAddress(out.m_Module, "PixCreateFactory"));
        if (!out.m_CreateFactory)
        {
            std::printf("pixapi.dll has no PixCreateFactory export\n");
            return false;
        }
        return true;
    }

    std::string Narrow(const wchar_t* text)
    {
        if (!text)
            return {};

        const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        std::string narrow(size > 0 ? size_t(size - 1) : 0, '\0');
        if (size > 1)
            WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow.data(), size, nullptr, nullptr);
        return narrow;
    }

    bool WriteBmp(const char* path, const PIX_CAPTURED_SCREENSHOT_INFO& info, const PIX_PIXEL_DATA& pixels)
    {
        // Accept the common 32/8-bit layouts; write a 24-bit bottom-up BMP, which needs no encoder.
        UINT32 bytesPerPixel = 0;
        switch (info.Format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            bytesPerPixel = 4;
            break;
        default:
            std::printf("screenshot format %u is not handled; writing the raw bytes instead\n", UINT32(info.Format));
            FILE* raw = nullptr;
            if (fopen_s(&raw, path, "wb") != 0 || !raw)
                return false;
            fwrite(pixels.Data, 1, pixels.SizeInBytes, raw);
            fclose(raw);
            return true;
        }

        const UINT32 rowSize = (info.Width * 3 + 3) & ~UINT32(3);
        const UINT32 imageSize = rowSize * info.Height;
        const UINT32 fileSize = 54 + imageSize;

        FILE* file = nullptr;
        if (fopen_s(&file, path, "wb") != 0 || !file)
            return false;

        BYTE header[54]{};
        header[0] = 'B';
        header[1] = 'M';
        std::memcpy(header + 2, &fileSize, 4);
        const UINT32 pixelOffset = 54;
        std::memcpy(header + 10, &pixelOffset, 4);
        const UINT32 headerSize = 40;
        std::memcpy(header + 14, &headerSize, 4);
        std::memcpy(header + 18, &info.Width, 4);
        std::memcpy(header + 22, &info.Height, 4);
        const UINT16 planes = 1;
        std::memcpy(header + 26, &planes, 2);
        const UINT16 bitCount = 24;
        std::memcpy(header + 28, &bitCount, 2);
        std::memcpy(header + 34, &imageSize, 4);
        fwrite(header, 1, sizeof(header), file);

        std::vector<BYTE> row(rowSize, 0);
        for (UINT32 y = 0; y < info.Height; ++y)
        {
            const BYTE* source = pixels.Data + size_t(info.Height - 1 - y) * info.RowPitchBytes;
            for (UINT32 x = 0; x < info.Width; ++x)
            {
                const BYTE* texel = source + size_t(x) * bytesPerPixel;
                // Both DXGI layouts keep R first for the formats above except the BGRA ones; PIX hands out
                // BGRA, so byte 0 is blue there.
                row[x * 3 + 0] = texel[0];
                row[x * 3 + 1] = texel[1];
                row[x * 3 + 2] = texel[2];
            }
            fwrite(row.data(), 1, rowSize, file);
        }

        fclose(file);
        return true;
    }
}

int main(int argc, char** argv)
{
    std::wstring capturePath;
    std::wstring pixDirOverride;
    std::string filter;
    std::string screenshotPath;
    UINT32 limit = 40;

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument.rfind("--pix-dir=", 0) == 0)
        {
            const std::string value = argument.substr(10);
            pixDirOverride.assign(value.begin(), value.end());
        }
        else if (argument.rfind("--filter=", 0) == 0)
        {
            filter = argument.substr(9);
        }
        else if (argument.rfind("--limit=", 0) == 0)
        {
            limit = UINT32(std::atoi(argument.c_str() + 8));
        }
        else if (argument.rfind("--screenshot=", 0) == 0)
        {
            screenshotPath = argument.substr(13);
        }
        else if (argument.rfind("--", 0) == 0)
        {
            std::printf("unknown option %s\n", argument.c_str());
            return 2;
        }
        else
        {
            capturePath.assign(argument.begin(), argument.end());
        }
    }

    if (capturePath.empty())
    {
        std::printf("usage: PixProbe <capture.wpix> [--filter=<text>] [--limit=<n>] "
                    "[--screenshot=<out.bmp>] [--pix-dir=<dir>]\n");
        return 2;
    }

    const std::wstring pixDir = DiscoverPixDir(pixDirOverride);
    if (pixDir.empty())
    {
        std::printf("no PIX installation found (looked for include\\PixApi.h under %%ProgramFiles%%\\Microsoft PIX*, "
                    "and PIX_DIR)\n");
        return 3;
    }
    std::printf("PIX install: %ls\n", pixDir.c_str());

    PixApiLibrary library;
    if (!LoadPixApi(pixDir, library))
        return 3;

    IPixFactory* factory = nullptr;
    HRESULT hr = library.m_CreateFactory(__uuidof(IPixFactory), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory)
    {
        PrintHresult("PixCreateFactory", hr);
        return 4;
    }

    IPixGpuCaptureDocument* document = nullptr;
    CaptureNotifications* notifications = new CaptureNotifications();
    hr = factory->OpenGpuCaptureDocument(capturePath.c_str(), notifications, nullptr,
                                         __uuidof(IPixGpuCaptureDocument), reinterpret_cast<void**>(&document));
    notifications->Release();
    if (FAILED(hr) || !document)
    {
        PrintHresult("OpenGpuCaptureDocument", hr);
        factory->Release();
        return 4;
    }

    IPixGpuCaptureFileInfo* fileInfo = nullptr;
    if (SUCCEEDED(document->GetFileInfo(__uuidof(IPixGpuCaptureFileInfo), reinterpret_cast<void**>(&fileInfo))) &&
        fileInfo)
    {
        std::printf("\ncapture: %s\n", Narrow(capturePath.c_str()).c_str());
        std::printf("  device: %s\n", Narrow(fileInfo->GetCaptureDevice()).c_str());
        std::printf("  size:   %.1f MiB\n", double(fileInfo->GetFileSize()) / (1024.0 * 1024.0));

        const UINT64 pairCount = fileInfo->GetNumFileInfoStringPairs();
        for (UINT64 index = 0; index < pairCount; ++index)
        {
            PIX_STRING_PAIR pair{};
            if (SUCCEEDED(fileInfo->GetFileInfoStringPair(index, &pair)))
                std::printf("  %s = %s\n", Narrow(pair.Name).c_str(), Narrow(pair.Value).c_str());
        }
        fileInfo->Release();
    }

    IPixCollection* queues = nullptr;
    hr = document->GetQueues(__uuidof(IPixCollection), reinterpret_cast<void**>(&queues));
    if (FAILED(hr) || !queues)
    {
        PrintHresult("GetQueues", hr);
    }
    else
    {
        const UINT64 queueCount = queues->GetCount();
        std::printf("\nqueues: %llu\n", static_cast<unsigned long long>(queueCount));
        for (UINT64 queueIndex = 0; queueIndex < queueCount; ++queueIndex)
        {
            IPixGpuCaptureQueueInfo* queue = nullptr;
            if (FAILED(queues->Get(queueIndex, __uuidof(IPixGpuCaptureQueueInfo), reinterpret_cast<void**>(&queue))) ||
                !queue)
                continue;

            const UINT32 eventCount = queue->GetEventCount();
            std::printf("  [%u] %s: %u events\n", queue->GetId(), Narrow(queue->GetName()).c_str(), eventCount);

            UINT32 printed = 0;
            for (UINT32 eventIndex = 0; eventIndex < eventCount; ++eventIndex)
            {
                PIX_EVENT_INFO event{};
                if (FAILED(queue->GetEvent(eventIndex, &event)) || !event.Name)
                    continue;

                const std::string name = event.Name;
                if (!filter.empty() && name.find(filter) == std::string::npos)
                    continue;

                if (printed < limit)
                {
                    std::printf("      %5u parent=%5u %s%s\n", event.Index, event.ParentIndex, name.c_str(),
                                event.ApiCallData && *event.ApiCallData ? (" :: " + std::string(event.ApiCallData)).c_str()
                                                                       : "");
                    ++printed;
                }
                else if (printed == limit)
                {
                    std::printf("      ... (raise --limit to see more)\n");
                    ++printed;
                }
            }
            queue->Release();
        }
        queues->Release();
    }

    if (!screenshotPath.empty())
    {
        IPixCapturedScreenshot* screenshot = nullptr;
        hr = document->GetCapturedScreenshot(__uuidof(IPixCapturedScreenshot), reinterpret_cast<void**>(&screenshot));
        if (FAILED(hr) || !screenshot)
        {
            PrintHresult("GetCapturedScreenshot", hr);
        }
        else
        {
            const PIX_CAPTURED_SCREENSHOT_INFO info = screenshot->GetInfo();
            const PIX_PIXEL_DATA pixels = screenshot->GetPixels();
            std::printf("\nscreenshot: %ux%u format=%u (%u bytes)\n", info.Width, info.Height, UINT32(info.Format),
                        pixels.SizeInBytes);
            if (WriteBmp(screenshotPath.c_str(), info, pixels))
                std::printf("  written to %s\n", screenshotPath.c_str());
            else
                std::printf("  could not write %s\n", screenshotPath.c_str());
            screenshot->Release();
        }
    }

    document->Release();
    factory->Release();
    return 0;
}
