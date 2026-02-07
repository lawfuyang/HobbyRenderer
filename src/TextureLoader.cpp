#include "TextureLoader.h"

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "../external/stb_image.h"

class MemoryMappedDataReader : public ITextureDataReader
{
public:
    MemoryMappedDataReader(std::string_view filePath)
    {
#ifdef _WIN32
        m_File = CreateFileA(std::string(filePath).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (m_File == INVALID_HANDLE_VALUE)
        {
            SDL_Log("[TextureLoader] CreateFileA failed for %*.s (Error: %lu)", (int)filePath.size(), filePath.data(), GetLastError());
            return;
        }

        LARGE_INTEGER size;
        if (!GetFileSizeEx(m_File, &size))
        {
            SDL_Log("[TextureLoader] GetFileSizeEx failed for %*.s (Error: %lu)", (int)filePath.size(), filePath.data(), GetLastError());
            return;
        }
        m_Size = static_cast<size_t>(size.QuadPart);

        if (m_Size == 0) return;

        m_Mapping = CreateFileMappingA(m_File, NULL, PAGE_READONLY, 0, 0, NULL);
        if (m_Mapping == NULL)
        {
            SDL_Log("[TextureLoader] CreateFileMappingA failed for %*.s (Error: %lu)", (int)filePath.size(), filePath.data(), GetLastError());
            return;
        }

        m_Data = MapViewOfFile(m_Mapping, FILE_MAP_READ, 0, 0, 0);
        if (m_Data == NULL)
        {
            SDL_Log("[TextureLoader] MapViewOfFile failed for %*.s (Error: %lu)", (int)filePath.size(), filePath.data(), GetLastError());
        }
#endif
    }

    ~MemoryMappedDataReader() override
    {
#ifdef _WIN32
        if (m_Data) UnmapViewOfFile(m_Data);
        if (m_Mapping) CloseHandle(m_Mapping);
        if (m_File != INVALID_HANDLE_VALUE) CloseHandle(m_File);
#endif
    }

    const void* GetData() const override { return static_cast<const uint8_t*>(m_Data) + m_Offset; }
    size_t GetSize() const override { return m_Size - m_Offset; }

    bool IsValid() const { return m_Data != nullptr; }

    void SetOffset(size_t offset) { m_Offset = offset; }

private:
    void* m_Data = nullptr;
    size_t m_Size = 0;
    size_t m_Offset = 0;
#ifdef _WIN32
    HANDLE m_File = INVALID_HANDLE_VALUE;
    HANDLE m_Mapping = NULL;
#endif
};

class StbiDataReader : public ITextureDataReader
{
public:
    StbiDataReader(unsigned char* data, size_t size) : m_Data(data), m_Size(size) {}
    ~StbiDataReader() override
    {
        if (m_Data) stbi_image_free(m_Data);
    }

    const void* GetData() const override { return m_Data; }
    size_t GetSize() const override { return m_Size; }

private:
    unsigned char* m_Data = nullptr;
    size_t m_Size = 0;
};

const uint32_t DDS_MAGIC = 0x20534444; // 'DDS '
const uint32_t DDS_FOURCC_DX10 = 0x30315844; // 'DX10'
const uint32_t DDS_FOURCC_DXT1 = 0x31545844; // 'DXT1'
const uint32_t DDS_FOURCC_DXT3 = 0x33545844; // 'DXT3'
const uint32_t DDS_FOURCC_DXT5 = 0x35545844; // 'DXT5'
const uint32_t DDS_FOURCC_ATI1 = 0x55344342; // 'ATI1' (BC4)
const uint32_t DDS_FOURCC_ATI2 = 0x55354342; // 'ATI2' (BC5)

const uint32_t DDS_DDPF_FOURCC = 0x4;
const uint32_t DDS_DDPF_RGB = 0x40;

const uint32_t DDS_RESOURCE_MISC_TEXTURECUBE = 0x4;

const uint32_t DDS_RESOURCE_DIMENSION_TEXTURE1D = 2;
const uint32_t DDS_RESOURCE_DIMENSION_TEXTURE2D = 3;
const uint32_t DDS_RESOURCE_DIMENSION_TEXTURE3D = 4;

struct DDS_PIXELFORMAT
{
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwFourCC;
    uint32_t dwRGBBitCount;
    uint32_t dwRBitMask;
    uint32_t dwGBitMask;
    uint32_t dwBBitMask;
    uint32_t dwABitMask;
};

struct DDS_HEADER
{
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwHeight;
    uint32_t dwWidth;
    uint32_t dwPitchOrLinearSize;
    uint32_t dwDepth;
    uint32_t dwMipMapCount;
    uint32_t dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t dwCaps;
    uint32_t dwCaps2;
    uint32_t dwCaps3;
    uint32_t dwCaps4;
    uint32_t dwReserved2;
};

struct DDS_HEADER_DXT10
{
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
};

static nvrhi::Format GetFormatFromDDS(const DDS_PIXELFORMAT& pf, bool hasDX10, const DDS_HEADER_DXT10& dx10)
{
    if (hasDX10)
    {
        switch (dx10.dxgiFormat)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return nvrhi::Format::RGBA8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return nvrhi::Format::SRGBA8_UNORM;
        case DXGI_FORMAT_R16G16_FLOAT: return nvrhi::Format::RG16_FLOAT;
        case DXGI_FORMAT_R16G16_UNORM: return nvrhi::Format::RG16_UNORM;
        case DXGI_FORMAT_BC1_UNORM: return nvrhi::Format::BC1_UNORM;
        case DXGI_FORMAT_BC1_UNORM_SRGB: return nvrhi::Format::BC1_UNORM_SRGB;
        case DXGI_FORMAT_BC2_UNORM: return nvrhi::Format::BC2_UNORM;
        case DXGI_FORMAT_BC2_UNORM_SRGB: return nvrhi::Format::BC2_UNORM_SRGB;
        case DXGI_FORMAT_BC3_UNORM: return nvrhi::Format::BC3_UNORM;
        case DXGI_FORMAT_BC3_UNORM_SRGB: return nvrhi::Format::BC3_UNORM_SRGB;
        case DXGI_FORMAT_BC4_UNORM: return nvrhi::Format::BC4_UNORM;
        case DXGI_FORMAT_BC4_SNORM: return nvrhi::Format::BC4_SNORM;
        case DXGI_FORMAT_BC5_UNORM: return nvrhi::Format::BC5_UNORM;
        case DXGI_FORMAT_BC5_SNORM: return nvrhi::Format::BC5_SNORM;
        case DXGI_FORMAT_BC6H_UF16: return nvrhi::Format::BC6H_UFLOAT;
        case DXGI_FORMAT_BC6H_SF16: return nvrhi::Format::BC6H_SFLOAT;
        case DXGI_FORMAT_BC7_UNORM: return nvrhi::Format::BC7_UNORM;
        case DXGI_FORMAT_BC7_UNORM_SRGB: return nvrhi::Format::BC7_UNORM_SRGB;
        default: SDL_LOG_ASSERT_FAIL("Unsupported DXGI format", "Unsupported DXGI format"); return nvrhi::Format::UNKNOWN;
        }
    }
    else
    {
        if (pf.dwFlags & DDS_DDPF_FOURCC)
        { // DDPF_FOURCC
            uint32_t fourCC = pf.dwFourCC;
            if (fourCC == DDS_FOURCC_DXT1) return nvrhi::Format::BC1_UNORM; // DXT1
            if (fourCC == DDS_FOURCC_DXT3) return nvrhi::Format::BC2_UNORM; // DXT3
            if (fourCC == DDS_FOURCC_DXT5) return nvrhi::Format::BC3_UNORM; // DXT5
            if (fourCC == DDS_FOURCC_ATI1) return nvrhi::Format::BC4_UNORM; // ATI1
            if (fourCC == DDS_FOURCC_ATI2) return nvrhi::Format::BC5_UNORM; // ATI2

            // Legacy D3DFMT values
            if (fourCC == 34)  return nvrhi::Format::RG16_UNORM;
            if (fourCC == 36)  return nvrhi::Format::RGBA16_UNORM;
            if (fourCC == 111) return nvrhi::Format::R16_FLOAT;
            if (fourCC == 112) return nvrhi::Format::RG16_FLOAT;
            if (fourCC == 113) return nvrhi::Format::RGBA16_FLOAT;
            if (fourCC == 114) return nvrhi::Format::R32_FLOAT;
            if (fourCC == 115) return nvrhi::Format::RG32_FLOAT;
            if (fourCC == 116) return nvrhi::Format::RGBA32_FLOAT;

            SDL_LOG_ASSERT_FAIL("Unsupported FourCC", "Unsupported FourCC: %u", fourCC);
            return nvrhi::Format::UNKNOWN;
        }
        else if (pf.dwFlags & DDS_DDPF_RGB)
        { // DDPF_RGB
            if (pf.dwRGBBitCount == 32) {
                if (pf.dwRBitMask == 0x00ff0000 && pf.dwGBitMask == 0x0000ff00 && pf.dwBBitMask == 0x000000ff && pf.dwABitMask == 0xff000000)
                {
                    return nvrhi::Format::RGBA8_UNORM;
                }
            }
            else if (pf.dwRGBBitCount == 24) {
                if (pf.dwRBitMask == 0x00ff0000 && pf.dwGBitMask == 0x0000ff00 && pf.dwBBitMask == 0x000000ff && pf.dwABitMask == 0x00000000)
                {
                    SDL_LOG_ASSERT_FAIL("24-bit RGB format not supported", "24-bit RGB format not supported");
                    return nvrhi::Format::UNKNOWN;
                }
            }
            SDL_LOG_ASSERT_FAIL("Unsupported RGB format", "Unsupported RGB format");
            return nvrhi::Format::UNKNOWN;
        }
        SDL_LOG_ASSERT_FAIL("Unsupported pixel format", "Unsupported pixel format");
        return nvrhi::Format::UNKNOWN;
    }
}

static nvrhi::TextureDimension GetDimensionFromDDS(bool hasDX10, const DDS_HEADER_DXT10& dx10, uint32_t arraySize)
{
    if (hasDX10)
    {
        switch (dx10.resourceDimension)
        {
        case DDS_RESOURCE_DIMENSION_TEXTURE1D: return arraySize > 1 ? nvrhi::TextureDimension::Texture1DArray : nvrhi::TextureDimension::Texture1D;
        case DDS_RESOURCE_DIMENSION_TEXTURE2D:
        {
            if (arraySize == 6 && (dx10.miscFlag & DDS_RESOURCE_MISC_TEXTURECUBE)) return nvrhi::TextureDimension::TextureCube;
            if (arraySize > 1) return nvrhi::TextureDimension::Texture2DArray;
            return nvrhi::TextureDimension::Texture2D;
        }
        case DDS_RESOURCE_DIMENSION_TEXTURE3D: return nvrhi::TextureDimension::Texture3D;
        default: return nvrhi::TextureDimension::Texture2D;
        }
    }
    return nvrhi::TextureDimension::Texture2D;
}

void LoadDDSTexture(std::string_view filePath, nvrhi::TextureDesc& desc, std::unique_ptr<ITextureDataReader>& data)
{
    std::unique_ptr<MemoryMappedDataReader> mappedData = std::make_unique<MemoryMappedDataReader>(filePath);
    if (!mappedData->IsValid())
    {
        SDL_LOG_ASSERT_FAIL("Cannot map file", "Cannot map file: %s", std::string(filePath).c_str());
        return;
    }

    const uint8_t* ptr = static_cast<const uint8_t*>(mappedData->GetData());
    size_t size = mappedData->GetSize();

    if (size < sizeof(uint32_t) + sizeof(DDS_HEADER))
    {
        SDL_LOG_ASSERT_FAIL("Invalid DDS file size", "Invalid DDS file size");
        return;
    }

    uint32_t magic = *reinterpret_cast<const uint32_t*>(ptr);
    if (magic != DDS_MAGIC)
    {
        SDL_LOG_ASSERT_FAIL("Not a DDS file", "Not a DDS file");
        return;
    }

    const DDS_HEADER& header = *reinterpret_cast<const DDS_HEADER*>(ptr + sizeof(uint32_t));
    if (header.dwSize != sizeof(DDS_HEADER))
    {
        SDL_LOG_ASSERT_FAIL("Invalid DDS header size", "Invalid DDS header size");
        return;
    }

    bool hasDX10 = (header.ddspf.dwFlags & DDS_DDPF_FOURCC) && (header.ddspf.dwFourCC == DDS_FOURCC_DX10);
    DDS_HEADER_DXT10 dx10Header = {};
    size_t offset = sizeof(uint32_t) + sizeof(DDS_HEADER);

    if (hasDX10)
    {
        if (size < offset + sizeof(DDS_HEADER_DXT10))
        {
            SDL_LOG_ASSERT_FAIL("Invalid DDS file size (DX10)", "Invalid DDS file size (DX10)");
            return;
        }
        dx10Header = *reinterpret_cast<const DDS_HEADER_DXT10*>(ptr + offset);
        offset += sizeof(DDS_HEADER_DXT10);
    }

    // Initialize TextureDesc
    desc.width = header.dwWidth;
    desc.height = header.dwHeight;
    desc.depth = header.dwDepth ? header.dwDepth : 1;
    desc.arraySize = hasDX10 ? dx10Header.arraySize : 1;
    desc.mipLevels = header.dwMipMapCount ? header.dwMipMapCount : 1;
    desc.format = GetFormatFromDDS(header.ddspf, hasDX10, dx10Header);
    desc.dimension = GetDimensionFromDDS(hasDX10, dx10Header, desc.arraySize);
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;

    mappedData->SetOffset(offset);
    data = std::move(mappedData);
}

void LoadSTBITexture(std::string_view filePath, nvrhi::TextureDesc& desc, std::unique_ptr<ITextureDataReader>& data)
{
    int width, height, channels;
    unsigned char* img = stbi_load(std::string(filePath).c_str(), &width, &height, &channels, 4);
    if (!img)
    {
        SDL_LOG_ASSERT_FAIL("Failed to load image", "STBI failed to load %s", std::string(filePath).c_str());
        return;
    }

    desc.width = static_cast<uint32_t>(width);
    desc.height = static_cast<uint32_t>(height);
    desc.depth = 1;
    desc.arraySize = 1;
    desc.mipLevels = 1;
    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;

    size_t dataSize = static_cast<size_t>(width * height * 4);
    data = std::make_unique<StbiDataReader>(img, dataSize);
}

bool LoadTexture(std::string_view filePath, nvrhi::TextureDesc& desc, std::unique_ptr<ITextureDataReader>& data)
{
    std::filesystem::path path(filePath);
    std::string extension = path.extension().string();
    for (char& c : extension) c = (char)std::tolower(c);

    if (extension == ".dds")
    {
        LoadDDSTexture(filePath, desc, data);
        return data != nullptr;
    }
    else
    {
        LoadSTBITexture(filePath, desc, data);
        return data != nullptr;
    }

    return false;
}
