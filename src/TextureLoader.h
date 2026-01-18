#pragma once

#include "pch.h"

#include <nvrhi/nvrhi.h>

void LoadDDSTexture(std::string_view filePath, nvrhi::TextureDesc& desc, std::vector<uint8_t>& data);