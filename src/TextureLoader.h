#pragma once



#include <nvrhi/nvrhi.h>

class MemoryMappedDataReader; // defined in Utilities.h

void UploadTexture(nvrhi::ICommandList* cmd, nvrhi::ITexture* texture, const nvrhi::TextureDesc& desc, const void* data, size_t dataSize = 0);

bool LoadTexture(std::string_view filePath, nvrhi::TextureDesc& desc, std::unique_ptr<MemoryMappedDataReader>& data);
void LoadDDSTexture(std::string_view filePath, nvrhi::TextureDesc& desc, std::unique_ptr<MemoryMappedDataReader>& data);
void LoadSTBITexture(std::string_view filePath, nvrhi::TextureDesc& desc, std::unique_ptr<MemoryMappedDataReader>& data);