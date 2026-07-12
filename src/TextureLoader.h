#pragma once

#include <nvrhi/nvrhi.h>
#include "shaders/srrhi/cpp/Common.h"

class MemoryMappedDataReader; // defined in Utilities.h

void UploadTexture(nvrhi::ICommandList* cmd, nvrhi::ITexture* texture, const nvrhi::TextureDesc& desc, const void* data, size_t dataSize = 0);

bool LoadTexture(std::string_view filePath, nvrhi::TextureDesc& desc, std::unique_ptr<MemoryMappedDataReader>& data);
void LoadDDSTexture(std::string_view filePath, nvrhi::TextureDesc& desc, std::unique_ptr<MemoryMappedDataReader>& data);
void LoadSTBITexture(std::string_view filePath, nvrhi::TextureDesc& desc, std::unique_ptr<MemoryMappedDataReader>& data);

// Compute per-mip byte offsets within DDS pixel data using proper format-aware math.
// Fills outOffsets[m] with the byte offset (from start of pixel data) of each mip.
// Returns the number of mips written (desc.mipLevels, clamped to MAX_MIP_COUNT).
uint32_t ComputeDDSMipOffsets(const nvrhi::TextureDesc& desc, size_t outOffsets[srrhi::CommonConsts::MAX_MIP_COUNT]);