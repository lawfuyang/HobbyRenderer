#pragma once

#include "pch.h"

#include <nvrhi/nvrhi.h>

bool LoadTexture(std::string_view filePath, nvrhi::TextureDesc& desc, std::vector<uint8_t>& data);
void LoadDDSTexture(std::string_view filePath, nvrhi::TextureDesc& desc, std::vector<uint8_t>& data);
void LoadSTBITexture(std::string_view filePath, nvrhi::TextureDesc& desc, std::vector<uint8_t>& data);