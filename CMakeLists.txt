cmake_minimum_required(VERSION 3.16)
project(AgenticRenderer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Download stb_image.h if not present
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/stb_image.h")
    message(STATUS "Downloading stb_image.h...")
    file(DOWNLOAD "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h" "${CMAKE_SOURCE_DIR}/external/stb_image.h" SHOW_PROGRESS)
endif()

# Speed up MSVC builds by enabling multi-processor compilation.
if(MSVC)
    add_compile_options(/MP)
endif()

# ============================================================================
# Function to download and extract a library from GitHub
# ============================================================================
function(download_library LIB_NAME LIB_VERSION DOWNLOAD_URL LIB_PATTERN)
    set(LIB_INSTALL_DIR "${CMAKE_SOURCE_DIR}/external/${LIB_NAME}")
    set(LIB_VERSION_FILE "${LIB_INSTALL_DIR}/version.txt")

    # Ensure the library directory exists before any further operations
    if(NOT EXISTS "${LIB_INSTALL_DIR}")
        file(MAKE_DIRECTORY "${LIB_INSTALL_DIR}")
    endif()
    
    # Check if library needs to be downloaded
    set(NEEDS_DOWNLOAD TRUE)
    if(EXISTS "${LIB_VERSION_FILE}")
        file(READ "${LIB_VERSION_FILE}" INSTALLED_VERSION)
        string(STRIP "${INSTALLED_VERSION}" INSTALLED_VERSION)
        if("${INSTALLED_VERSION}" STREQUAL "${LIB_VERSION}")
            set(NEEDS_DOWNLOAD FALSE)
            message(STATUS "${LIB_NAME} ${INSTALLED_VERSION} already installed, skipping download")
        else()
            message(STATUS "${LIB_NAME} version mismatch (installed: ${INSTALLED_VERSION}, requested: ${LIB_VERSION})")
        endif()
    endif()
    
    if(NEEDS_DOWNLOAD)
        message(STATUS "Downloading ${LIB_NAME} ${LIB_VERSION}...")
        
        # Create external directory if it doesn't exist
        file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/external")
        
        set(LIB_ZIP "${CMAKE_SOURCE_DIR}/external/${LIB_NAME}-${LIB_VERSION}.zip")
        
        # Download library binaries
        file(DOWNLOAD "${DOWNLOAD_URL}" "${LIB_ZIP}" 
            SHOW_PROGRESS
            STATUS DOWNLOAD_STATUS)
        
        list(GET DOWNLOAD_STATUS 0 DOWNLOAD_RESULT)
        if(NOT DOWNLOAD_RESULT EQUAL 0)
            list(GET DOWNLOAD_STATUS 1 ERROR_MESSAGE)
            message(FATAL_ERROR "Failed to download ${LIB_NAME}: ${ERROR_MESSAGE}")
        endif()
        
        # Extract the zip file
        message(STATUS "Extracting ${LIB_NAME}...")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xf "${LIB_ZIP}" --format=zip
            WORKING_DIRECTORY "${LIB_INSTALL_DIR}/"
            RESULT_VARIABLE EXTRACT_RESULT
        )
        
        if(NOT EXTRACT_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to extract ${LIB_NAME} zip file")
        endif()
        
        # Delete the zip file
        file(REMOVE "${LIB_ZIP}")
        
        # Create version file to track installed version
        file(WRITE "${LIB_VERSION_FILE}" "${LIB_VERSION}")
        message(STATUS "${LIB_NAME} ${LIB_VERSION} installed successfully")
    endif()
endfunction()

# ============================================================================
# Download Libraries
# ============================================================================

# SDL3 Configuration
set(SDL3_VERSION "3.4.0")
set(SDL3_URL "https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION}/SDL3-devel-${SDL3_VERSION}-VC.zip")
download_library("SDL3" "${SDL3_VERSION}" "${SDL3_URL}" "SDL3-*")
set(SDL3_INSTALL_DIR "${CMAKE_SOURCE_DIR}/external/SDL3")
if(WIN32)
    set(SDL3_DLL_PATH "${SDL3_INSTALL_DIR}/SDL3-${SDL3_VERSION}/lib/x64/SDL3.dll")
    set(SDL3_DLL_LINK "${CMAKE_SOURCE_DIR}/bin/SDL3.dll")
    file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
    execute_process(COMMAND ${CMAKE_COMMAND} -E remove -f "${SDL3_DLL_LINK}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink "${SDL3_DLL_PATH}" "${SDL3_DLL_LINK}")
endif()

# ---------------------------------------------------------------------------
# cgltf (glTF loader) - downloaded into external/cgltf and compiled
# ---------------------------------------------------------------------------
set(CGLTF_VERSION "1.15")
set(CGLTF_URL "https://github.com/jkuhlmann/cgltf/archive/refs/tags/v${CGLTF_VERSION}.zip")
download_library("cgltf" "v${CGLTF_VERSION}" "${CGLTF_URL}" "cgltf-*")
set(CGLTF_INSTALL_DIR "${CMAKE_SOURCE_DIR}/external/cgltf")
set(CGLTF_SUBDIR "cgltf-${CGLTF_VERSION}")
set(CGLTF_SRC_DIR "${CGLTF_INSTALL_DIR}/${CGLTF_SUBDIR}")

# ----------------------------------------------------------------------------
# DirectX Shader Compiler (DXC) - downloaded into external/dxc
# ----------------------------------------------------------------------------
set(DXC_VERSION "v1.8.2505.1")
set(DXC_URL "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DXC_VERSION}/dxc_2025_07_14.zip")
download_library("dxc" "${DXC_VERSION}" "${DXC_URL}" "dxc_*")
set(DXC_INSTALL_DIR "${CMAKE_SOURCE_DIR}/external/dxc")
set(DXC_EXE "${DXC_INSTALL_DIR}/bin/x64/dxc.exe")
set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)

add_subdirectory(external/nvrhi)
add_subdirectory(external/meshoptimizer)

# ============================================================================
# ImGui Configuration
# ============================================================================
set(IMGUI_SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/imgui")
set(IMGUI_SOURCES
    ${IMGUI_SOURCE_DIR}/imgui.cpp
    ${IMGUI_SOURCE_DIR}/imgui_demo.cpp
    ${IMGUI_SOURCE_DIR}/imgui_draw.cpp
    ${IMGUI_SOURCE_DIR}/imgui_tables.cpp
    ${IMGUI_SOURCE_DIR}/imgui_widgets.cpp
    ${IMGUI_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
)

# Output all executables to /bin/ directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_SOURCE_DIR}/bin)

# Find all C++ source files in src directory
file(GLOB_RECURSE SOURCES 
    "src/*.cpp"
    "src/*.h"
    "src/*.hpp"
)

# Create executable
add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${IMGUI_SOURCES})

# Define Windows-specific macros to prevent min/max macro conflicts
if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Precompiled header to speed up builds.
target_precompile_headers(${PROJECT_NAME} PRIVATE src/pch.h)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE src)

# ImGui Include
target_include_directories(${PROJECT_NAME} PRIVATE ${IMGUI_SOURCE_DIR})
target_include_directories(${PROJECT_NAME} PRIVATE ${IMGUI_SOURCE_DIR}/backends)

# SDL3 Include and Link
target_include_directories(${PROJECT_NAME} PRIVATE "${SDL3_INSTALL_DIR}/SDL3-${SDL3_VERSION}/include")
target_link_directories(${PROJECT_NAME} PRIVATE "${SDL3_INSTALL_DIR}/SDL3-${SDL3_VERSION}/lib/x64")
target_link_libraries(${PROJECT_NAME} PRIVATE SDL3)

# Link NVRHI
target_link_libraries(${PROJECT_NAME} PRIVATE nvrhi)
target_link_libraries(${PROJECT_NAME} PRIVATE nvrhi_vk)
target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan-Headers)

# Link meshoptimizer
target_link_libraries(${PROJECT_NAME} PRIVATE meshoptimizer)

# cgltf Include
target_include_directories(${PROJECT_NAME} PRIVATE "${CGLTF_SRC_DIR}")

# meshoptimizer Include
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/external/meshoptimizer/src")

# ============================================================================
# ShaderMake Integration (offline HLSL -> SPIRV compilation)
# ============================================================================

# Add NVIDIA ShaderMake tool as a subdirectory.
# Use in-project target so we can depend on it and resolve the executable per-config.
set(SHADERMAKE_TOOL OFF CACHE BOOL "Use ShaderMake as external tool" FORCE)
set(SHADERMAKE_FIND_COMPILERS ON CACHE BOOL "Enable compiler discovery" FORCE)
set(SHADERMAKE_FIND_FXC OFF)
set(SHADERMAKE_FIND_DXC OFF)

add_subdirectory(external/ShaderMake)

set(SHADER_CONFIG       ${CMAKE_SOURCE_DIR}/src/shaders/shaders.cfg)
set(SHADER_OUTPUT_DIR   ${CMAKE_SOURCE_DIR}/bin/shaders)
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# Resolve ShaderMake executable path per configuration when possible
if(TARGET ShaderMake)
    set(SHADERMAKE_PATH_GENEX $<TARGET_FILE:ShaderMake>)
else()
    set(SHADERMAKE_PATH_GENEX ${SHADERMAKE_PATH})
endif()

# Custom target to compile shaders offline with ShaderMake (SPIR-V output for Vulkan)
add_custom_target(build_shaders
    COMMAND ${SHADERMAKE_PATH_GENEX}
            -p SPIRV
            --binary
            -c "${SHADER_CONFIG}"
            -o "${SHADER_OUTPUT_DIR}"
            --compiler "${DXC_EXE}"
            --shaderModel 6_8
            --hlsl2021
            --vulkanVersion 1.3
            --vulkanMemoryLayout dx
            --include "${CMAKE_SOURCE_DIR}/src/shaders"
            --relaxedInclude "pch.h"
            --verbose
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Compiling HLSL shaders to SPIR-V for Vulkan via ShaderMake"
)

# Ensure ShaderMake is built before running the shader compilation target
if(TARGET ShaderMake)
    add_dependencies(build_shaders ShaderMake)
endif()

# Shader compilation always runs before building the main executable
add_dependencies(${PROJECT_NAME} build_shaders)

# Convenience: show where ShaderMake.exe is located in the build logs
message(STATUS "ShaderMake executable: ${SHADERMAKE_PATH}")