#include "pch.h"
#include "Utilities.h"

// ─── MemoryMappedDataReader ──────────────────────────────────────────────────

MemoryMappedDataReader::MemoryMappedDataReader(std::string_view filePath)
{
#ifdef _WIN32
    m_File = CreateFileA(std::string(filePath).c_str(), GENERIC_READ, FILE_SHARE_READ,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (m_File == INVALID_HANDLE_VALUE)
    {
        SDL_Log("[MemoryMappedDataReader] CreateFileA failed for %.*s (Error: %lu)",
                (int)filePath.size(), filePath.data(), GetLastError());
        return;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(m_File, &size))
    {
        SDL_Log("[MemoryMappedDataReader] GetFileSizeEx failed for %.*s (Error: %lu)",
                (int)filePath.size(), filePath.data(), GetLastError());
        return;
    }
    m_Size = static_cast<size_t>(size.QuadPart);
    if (m_Size == 0) return;

    m_Mapping = CreateFileMappingA(m_File, NULL, PAGE_READONLY, 0, 0, NULL);
    if (m_Mapping == NULL)
    {
        SDL_Log("[MemoryMappedDataReader] CreateFileMappingA failed for %.*s (Error: %lu)",
                (int)filePath.size(), filePath.data(), GetLastError());
        return;
    }

    m_Data = MapViewOfFile(m_Mapping, FILE_MAP_READ, 0, 0, 0);
    if (m_Data == NULL)
    {
        SDL_Log("[MemoryMappedDataReader] MapViewOfFile failed for %.*s (Error: %lu)",
                (int)filePath.size(), filePath.data(), GetLastError());
    }
#endif
}

MemoryMappedDataReader::MemoryMappedDataReader(void* data, size_t size, void (*deleter)(void*))
    : m_Data(data), m_Size(size), m_Deleter(deleter)
{
}

MemoryMappedDataReader::~MemoryMappedDataReader()
{
    if (m_Deleter)
    {
        if (m_Data) m_Deleter(m_Data);
    }
    else
    {
#ifdef _WIN32
        if (m_Data)    UnmapViewOfFile(m_Data);
        if (m_Mapping) CloseHandle(m_Mapping);
        if (m_File != INVALID_HANDLE_VALUE) CloseHandle(m_File);
#endif
    }
}

// ─────────────────────────────────────────────────────────────────────────────

float Halton(uint32_t index, uint32_t base)
{
    float result = 0.0f;
    float f = 1.0f / (float)base;
    uint32_t i = index;
    while (i > 0)
    {
        result += f * (float)(i % base);
        i /= base;
        f /= (float)base;
    }
    return result;
}

Vector3 CalculateGridZParams(float NearPlane, float FarPlane, float DepthDistributionScale, uint32_t GridSizeZ)
{
    // S = distribution scale
    // B, O are solved for given the z distances of the first+last slice, and the # of slices.
    //
    // slice = log2(z*B + O) * S

    // Don't spend lots of resolution right in front of the near plane
    float NearOffset = .095 * 100;

    // Space out the slices so they aren't all clustered at the near plane
    float S = DepthDistributionScale * GridSizeZ / log2(FarPlane / NearPlane);

    float N = NearPlane;// + NearOffset;
    float F = FarPlane;

    float O = (F - N * exp2(GridSizeZ / S)) / (F - N);
    float B = (1 - O) / N;

    return Vector3{ B, O, S };
}

Vector2 CreateInvDeviceZToWorldZTransform(const Matrix& ProjMatrix)
{
	// The perspective depth projection comes from the the following projection matrix:
	//
	// | 1  0  0  0 |
	// | 0  1  0  0 |
	// | 0  0  A  1 |
	// | 0  0  B  0 |
	//
	// Z' = (Z * A + B) / Z
	// Z' = A + B / Z
	//
	// So to get Z from Z' is just:
	// Z = B / (Z' - A)
	//
	// Note a reversed Z projection matrix will have A=0.
	//
	// Done in shader as:
	// Z = 1 / (Z' * C1 - C2)   --- Where C1 = 1/B, C2 = A/B
	//

	float DepthMul = ProjMatrix.m[2][2];
	float DepthAdd = ProjMatrix.m[3][2];

	if (DepthAdd == 0.f)
	{
		// Avoid dividing by 0 in this case
		DepthAdd = 0.00000001f;
	}

	// SceneDepth = 1.0f / (DeviceZ / ProjMatrix.M[3][2] - ProjMatrix.M[2][2] / ProjMatrix.M[3][2])

	// combined equation in shader to handle either
	// SceneDepth = DeviceZ * View.InvDeviceZToWorldZTransform[0] + View.InvDeviceZToWorldZTransform[1] + 1.0f / (DeviceZ * View.InvDeviceZToWorldZTransform[2] - View.InvDeviceZToWorldZTransform[3]);

	// therefore perspective needs
	// InvDeviceZToWorldZTransform[0] = 1.0f / ProjMatrix.M[3][2]
	// InvDeviceZToWorldZTransform[1] = ProjMatrix.M[2][2] / ProjMatrix.M[3][2]

    float SubtractValue = DepthMul / DepthAdd;

    // Subtract a tiny number to avoid divide by 0 errors in the shader when a very far distance is decided from the depth buffer.
    // This fixes fog not being applied to the black background in the editor.
    SubtractValue -= 0.00000001f;

    return Vector2{ 1.0f / DepthAdd, SubtractValue };
}

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file.is_open())
    {
        SDL_Log("[Shader] Failed to open file: %s", path.string().c_str());
        return {};
    }

    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size))
    {
        SDL_Log("[Shader] Failed to read file: %s", path.string().c_str());
        return {};
    }

    SDL_Log("[Shader] Loaded %zu bytes from %s", buffer.size(), path.string().c_str());
    return buffer;
}

uint32_t HashToUint(size_t hash)
{
    return uint32_t(hash ^ (hash >> 32));
}

void ChooseWindowSize(int* outWidth, int* outHeight)
{
    int windowW = 1280;
    int windowH = 720;

    const SDL_DisplayID primaryDisplay = SDL_GetPrimaryDisplay();
    SDL_Rect usableBounds{};
    if (!SDL_GetDisplayUsableBounds(primaryDisplay, &usableBounds))
    {
        SDL_LOG_ASSERT_FAIL("SDL_GetDisplayUsableBounds failed", "SDL_GetDisplayUsableBounds failed: %s", SDL_GetError());
        *outWidth = windowW;
        *outHeight = windowH;
        return;
    }

    int maxFitW = usableBounds.w;
    int maxFitH = usableBounds.h;
    if (static_cast<int64_t>(maxFitW) * 9 > static_cast<int64_t>(maxFitH) * 16)
    {
        maxFitW = (maxFitH * 16) / 9;
    }
    else
    {
        maxFitH = (maxFitW * 9) / 16;
    }

    constexpr int kStandard16x9[][2] = {
        {3840, 2160},
        {2560, 1440},
        {1920, 1080},
        {1600, 900},
        {1280, 720},
    };
    constexpr int kStandard16x9Count = static_cast<int>(sizeof(kStandard16x9) / sizeof(kStandard16x9[0]));

    windowW = maxFitW;
    windowH = maxFitH;
    int firstFitIndex = -1;
    for (int i = 0; i < kStandard16x9Count; ++i)
    {
        if (kStandard16x9[i][0] <= maxFitW && kStandard16x9[i][1] <= maxFitH)
        {
            firstFitIndex = i;
            break;
        }
    }

    if (firstFitIndex >= 0)
    {
        int chosenIndex = firstFitIndex;
        const bool fillsUsableWidth  = kStandard16x9[firstFitIndex][0] == maxFitW;
        const bool fillsUsableHeight = kStandard16x9[firstFitIndex][1] == maxFitH;
        if (fillsUsableWidth && fillsUsableHeight && firstFitIndex + 1 < kStandard16x9Count)
        {
            chosenIndex = firstFitIndex + 1;
        }

        windowW = kStandard16x9[chosenIndex][0];
        windowH = kStandard16x9[chosenIndex][1];
    }

    SDL_Log("[Init] Usable bounds: %dx%d, max 16:9 fit: %dx%d, chosen: %dx%d", usableBounds.w, usableBounds.h, maxFitW, maxFitH, windowW, windowH);
    *outWidth = windowW;
    *outHeight = windowH;
}
