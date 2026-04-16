#pragma once



#include <nvrhi/nvrhi.h>

void UploadTexture(nvrhi::ICommandList* cmd, nvrhi::ITexture* texture, const nvrhi::TextureDesc& desc, const void* data, size_t dataSize = 0);

class ITextureDataReader
{
public:
    virtual ~ITextureDataReader() = default;
    virtual const void* GetData() const = 0;
    virtual size_t GetSize() const = 0;
};

bool LoadTexture(std::string_view filePath, nvrhi::TextureDesc& desc, std::unique_ptr<ITextureDataReader>& data);
void LoadDDSTexture(std::string_view filePath, nvrhi::TextureDesc& desc, std::unique_ptr<ITextureDataReader>& data);
void LoadSTBITexture(std::string_view filePath, nvrhi::TextureDesc& desc, std::unique_ptr<ITextureDataReader>& data);