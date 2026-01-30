#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cfloat>
#include <functional>
#include <memory>
#include <numbers>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_filesystem.h>

#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>
#include <nvrhi/utils.h>

#ifndef VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
    #define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#endif
#include <vulkan/vulkan.hpp>

// DirectXMath aliases moved from MathTypes.h
#include <DirectXMath.h>
#include <DirectXCollision.h>

#include <dxgiformat.h>

#include "../external/microprofile/microprofile.h"

using Vector = DirectX::XMVECTOR;
using Matrix = DirectX::XMFLOAT4X4;

using Vector2 = DirectX::XMFLOAT2;
using Vector3 = DirectX::XMFLOAT3;
using Vector4 = DirectX::XMFLOAT4;
using Vector3A = DirectX::XMFLOAT3A;
using Vector4A = DirectX::XMFLOAT4A;

using Vector2U = DirectX::XMUINT2;
using Vector2I = DirectX::XMINT2;
using Vector3U = DirectX::XMUINT3;
using Vector3I = DirectX::XMINT3;

using Color = DirectX::XMFLOAT4;

using Sphere = DirectX::BoundingSphere;
using AABB = DirectX::BoundingBox;
using OBB = DirectX::BoundingOrientedBox;
using Frustum = DirectX::BoundingFrustum;

#define SDL_LOG_ASSERT_FAIL(assertMsg, logFmt, ...) do { SDL_Log(logFmt, ##__VA_ARGS__); SDL_assert(false && assertMsg); } while(0)

#define PROFILE_SCOPED(NAME) MICROPROFILE_SCOPEI("", NAME, MP_AUTO);
#define PROFILE_FUNCTION() MICROPROFILE_SCOPEI("", __FUNCTION__, MP_AUTO);