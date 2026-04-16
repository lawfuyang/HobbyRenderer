#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <span>
#include <sstream>
#include <string_view>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_filesystem.h>

#include <nvrhi/validation.h>
#include <nvrhi/utils.h>

// Windows API
#include <windows.h>

// DirectXMath aliases moved from MathTypes.h
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <DirectXCollision.h>

#include <dxgiformat.h>

#include "../external/microprofile/microprofile.h"

using Vector = DirectX::XMVECTOR;
using Matrix = DirectX::XMFLOAT4X4;

using Vector2 = DirectX::XMFLOAT2;
using Vector3 = DirectX::XMFLOAT3;
using Vector4 = DirectX::XMFLOAT4;
using Quaternion = DirectX::XMFLOAT4;
using Vector3A = DirectX::XMFLOAT3A;
using Vector4A = DirectX::XMFLOAT4A;

using Vector2U = DirectX::XMUINT2;
using Vector2I = DirectX::XMINT2;
using Vector3U = DirectX::XMUINT3;
using Vector3I = DirectX::XMINT3;

using Sphere = DirectX::BoundingSphere;
using AABB = DirectX::BoundingBox;
using OBB = DirectX::BoundingOrientedBox;
using Frustum = DirectX::BoundingFrustum;

#define JOIN_MACROS_INTERNAL( Arg1, Arg2 ) Arg1##Arg2
#define JOIN_MACROS( Arg1, Arg2 )          JOIN_MACROS_INTERNAL( Arg1, Arg2 )
#define GENERATE_UNIQUE_VARIABLE(basename) JOIN_MACROS(basename, __COUNTER__)

#define SDL_LOG_ASSERT_FAIL(assertMsg, logFmt, ...) do { SDL_Log(logFmt, ##__VA_ARGS__); SDL_assert(false && assertMsg); } while(0)

#define PROFILE_SCOPED(NAME) MICROPROFILE_SCOPE_CSTR(NAME);
#define PROFILE_FUNCTION() MICROPROFILE_SCOPEI("", __FUNCTION__, MP_AUTO);

#define SingletonFunctionsCommon(ClassName)          \
    ClassName(const ClassName&)            = delete; \
    ClassName(ClassName&&)                 = delete; \
    ClassName& operator=(const ClassName&) = delete; \
    ClassName& operator=(ClassName&&)      = delete; \
    inline static ClassName* ms_Instance   = nullptr;

#define SingletonFunctionsSimple(ClassName)                      \
private:                                                         \
    SingletonFunctionsCommon(ClassName);                         \
public:                                                          \
    ClassName() { SDL_assert(!ms_Instance); ms_Instance = this; }    \
    ~ClassName() { if (ms_Instance == this) ms_Instance = nullptr; } \
    static ClassName& GetInstance() { SDL_assert(ms_Instance); return *ms_Instance; }
    