#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>

#ifndef VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
    #define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#endif
#include <vulkan/vulkan.hpp>
