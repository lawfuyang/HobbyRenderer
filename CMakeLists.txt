cmake_minimum_required(VERSION 3.16)
project(HobbyRenderer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
            message(STATUS "${LIB_NAME} version mismatch (installed: ${INSTALLED_VERSION}, requested: ${LIB_VERSION}). Deleting old version and downloading new one...")
            # Delete the old library directory to clean up
            file(REMOVE_RECURSE "${LIB_INSTALL_DIR}")
            file(MAKE_DIRECTORY "${LIB_INSTALL_DIR}")
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
    file(CREATE_LINK "${SDL3_INSTALL_DIR}/SDL3-${SDL3_VERSION}/lib/x64/SDL3.dll" "${CMAKE_SOURCE_DIR}/bin/SDL3.dll" SYMBOLIC)
endif()

# Agility SDK Configuration
set(AGILITY_SDK_VERSION "1.618.1")
set(AGILITY_SDK_URL "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/${AGILITY_SDK_VERSION}")
download_library("AgilitySDK" "${AGILITY_SDK_VERSION}" "${AGILITY_SDK_URL}" "AgilitySDK-*")
set(AGILITY_SDK_INSTALL_DIR "${CMAKE_SOURCE_DIR}/external/AgilitySDK")
set(D3D12MA_AGILITY_SDK_DIRECTORY "${AGILITY_SDK_INSTALL_DIR}")
if(WIN32)
    file(CREATE_LINK "${AGILITY_SDK_INSTALL_DIR}/build/native/bin/x64/D3D12Core.dll" "${CMAKE_SOURCE_DIR}/bin/D3D12Core.dll" SYMBOLIC)
    file(CREATE_LINK "${AGILITY_SDK_INSTALL_DIR}/build/native/bin/x64/d3d12SDKLayers.dll" "${CMAKE_SOURCE_DIR}/bin/d3d12SDKLayers.dll" SYMBOLIC)
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
set(DXC_VERSION "v1.9.2602")
set(DXC_URL "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DXC_VERSION}/dxc_2026_02_20.zip")
download_library("dxc" "${DXC_VERSION}" "${DXC_URL}" "dxc_*")
set(DXC_INSTALL_DIR "${CMAKE_SOURCE_DIR}/external/dxc")
set(DXC_EXE "${DXC_INSTALL_DIR}/bin/x64/dxc.exe")
set(NVRHI_WITH_VULKAN OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 ON CACHE BOOL "" FORCE)

# ============================================================================
# MathLib (ML)
# ============================================================================
set(ML_VERSION "11")
set(ML_URL "https://github.com/NVIDIA-RTX/MathLib/archive/refs/tags/v${ML_VERSION}.zip")
download_library("MathLib" "${ML_VERSION}" "${ML_URL}" "MathLib-*")
set(ML_INSTALL_DIR "${CMAKE_SOURCE_DIR}/external/MathLib")
set(ML_SUBDIR "MathLib-${ML_VERSION}")
set(ML_SRC_DIR "${ML_INSTALL_DIR}/${ML_SUBDIR}")

# ----------------------------------------------------------------------------
# NVIDIA REAL-TIME DENOISERS (NRD)
# ----------------------------------------------------------------------------
set(NRD_VERSION "4.17.2")
set(NRD_URL "https://github.com/NVIDIA-RTX/NRD/archive/refs/tags/v${NRD_VERSION}.zip")
download_library("NRD" "${NRD_VERSION}" "${NRD_URL}" "NRD-*")
set(NRD_INSTALL_DIR "${CMAKE_SOURCE_DIR}/external/NRD")
set(NRD_SUBDIR "NRD-${NRD_VERSION}")
set(NRD_SRC_DIR "${NRD_INSTALL_DIR}/${NRD_SUBDIR}")
if(WIN32)
    file(CREATE_LINK "${NRD_SRC_DIR}/Shaders/Shaders.cfg" "${CMAKE_SOURCE_DIR}/src/Shaders/NRDShaders.cfg" SYMBOLIC)
    file(CREATE_LINK "${NRD_SRC_DIR}/Shaders/NRD.hlsli" "${CMAKE_SOURCE_DIR}/src/Shaders/NRD.hlsli" SYMBOLIC)
    file(CREATE_LINK "${NRD_SRC_DIR}/Shaders/NRDConfig.hlsli" "${CMAKE_SOURCE_DIR}/src/Shaders/NRDConfig.hlsli" SYMBOLIC)
endif()

# ----------------------------------------------------------------------------
# AMD FSR SDK
# ----------------------------------------------------------------------------
SET(FSR_SDK_VERSION "2.2.0")
SET(FSR_SDK_URL "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/v${FSR_SDK_VERSION}.zip")
download_library("FSR_SDK" "${FSR_SDK_VERSION}" "${FSR_SDK_URL}" "FidelityFX-SDK-*")
set(FSR_SDK_INSTALL_DIR "${CMAKE_SOURCE_DIR}/external/FSR_SDK")
set(FSR_SDK_SUBDIR "FidelityFX-SDK-${FSR_SDK_VERSION}")
set(FSR_SDK_SRC_DIR "${FSR_SDK_INSTALL_DIR}/${FSR_SDK_SUBDIR}")
SET(FSR_SDK_FIDELITY_FX_DIR "${FSR_SDK_SRC_DIR}/Kits/FidelityFX")
SET(FSR_SDK_BIN_DIR "${FSR_SDK_FIDELITY_FX_DIR}/signedbin")

# Create symbolic links for DLL files in FSR_SDK_BIN_DIR to bin directory
if(WIN32)
    file(GLOB FSR_DLL_FILES "${FSR_SDK_BIN_DIR}/*.dll")
    foreach(FSR_DLL ${FSR_DLL_FILES})
        get_filename_component(FSR_DLL_FILENAME ${FSR_DLL} NAME)
        file(CREATE_LINK "${FSR_DLL}" "${CMAKE_SOURCE_DIR}/bin/${FSR_DLL_FILENAME}" SYMBOLIC)
    endforeach()

    file(CREATE_LINK "${FSR_SDK_FIDELITY_FX_DIR}" "${CMAKE_SOURCE_DIR}/src/FidelityFX" SYMBOLIC)
endif()

set(SRRHI_BUILD_VALIDATION_TARGETS OFF)
add_subdirectory(external/srrhi)
file(CREATE_LINK "${CMAKE_SOURCE_DIR}/external/srrhi/srrhi.h" "${CMAKE_SOURCE_DIR}/src/srrhi.h" SYMBOLIC)

set(SHADERMAKE_FIND_FXC OFF)
set(SHADERMAKE_FIND_DXC OFF)
add_subdirectory(external/ShaderMake)

add_subdirectory(${ML_SRC_DIR})

set(NRD_STATIC_LIBRARY ON)
set(NRD_DISABLE_SHADER_COMPILATION ON)
set(NRD_EMBEDS_SPIRV_SHADERS OFF)
set(NRD_EMBEDS_DXIL_SHADERS OFF)
set(NRD_EMBEDS_DXBC_SHADERS OFF)
add_subdirectory(${NRD_SRC_DIR})

add_subdirectory(external/nvrhi)
add_subdirectory(external/meshoptimizer)
add_subdirectory(external/microprofile)
add_subdirectory(external/rtxdi)

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
target_include_directories(${PROJECT_NAME} PRIVATE src/shaders)
target_include_directories(${PROJECT_NAME} PRIVATE src/shaders/rtxdi/SharedShaderInclude/SharedShaderInclude)
target_include_directories(${PROJECT_NAME} PUBLIC "${AGILITY_SDK_INSTALL_DIR}/build/native/include")

# ImGui Include
target_include_directories(${PROJECT_NAME} PRIVATE ${IMGUI_SOURCE_DIR})
target_include_directories(${PROJECT_NAME} PRIVATE ${IMGUI_SOURCE_DIR}/backends)

# SDL3 Include and Link
target_include_directories(${PROJECT_NAME} PRIVATE "${SDL3_INSTALL_DIR}/SDL3-${SDL3_VERSION}/include")
target_link_directories(${PROJECT_NAME} PRIVATE "${SDL3_INSTALL_DIR}/SDL3-${SDL3_VERSION}/lib/x64")
target_link_libraries(${PROJECT_NAME} PRIVATE SDL3)

target_link_libraries(${PROJECT_NAME} PRIVATE dxgi)

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/ShaderMake/ShaderMake")
target_link_libraries(${PROJECT_NAME} PRIVATE ShaderMakeBlob)

# Link NVRHI
target_link_libraries(${PROJECT_NAME} PRIVATE nvrhi)
target_link_libraries(${PROJECT_NAME} PRIVATE nvrhi_d3d12)

# Link meshoptimizer
target_link_libraries(${PROJECT_NAME} PRIVATE meshoptimizer)

# Link Microprofile
target_link_libraries(${PROJECT_NAME} PRIVATE microprofile)
target_compile_definitions(microprofile PUBLIC MICROPROFILE_USE_CONFIG=0 MICROPROFILE_GPU_TIMERS_D3D12=1)

# Link RTXDI
target_link_libraries(${PROJECT_NAME} PRIVATE Rtxdi)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/external/rtxdi/Include")

# cgltf Include
target_include_directories(${PROJECT_NAME} PRIVATE "${CGLTF_SRC_DIR}")

# meshoptimizer Include
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/external/meshoptimizer/src")

# NRD
target_link_libraries(${PROJECT_NAME} PRIVATE NRD)
target_include_directories(${PROJECT_NAME} PRIVATE "${NRD_SRC_DIR}/Include")

# FidelityFX
target_link_libraries(${PROJECT_NAME} PRIVATE "${FSR_SDK_BIN_DIR}/amd_fidelityfx_loader_dx12.lib")

# ============================================================================
# ShaderMake Integration (offline HLSL compilation)
# ============================================================================

# Add NVIDIA ShaderMake tool as a subdirectory.
# Use in-project target so we can depend on it and resolve the executable per-config.
set(SHADERMAKE_TOOL OFF CACHE BOOL "Use ShaderMake as external tool" FORCE)
set(SHADERMAKE_FIND_COMPILERS ON CACHE BOOL "Enable compiler discovery" FORCE)
set(SHADERMAKE_FIND_FXC OFF)
set(SHADERMAKE_FIND_DXC OFF)

set(SHADER_CONFIG ${CMAKE_SOURCE_DIR}/src/shaders/shaders.cfg)
set(NRD_SHADER_CONFIG ${NRD_SRC_DIR}/Shaders/shaders.cfg)
set(SHADER_OUTPUT_DIR_DXIL  ${CMAKE_SOURCE_DIR}/bin/shaders/dxil)
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR_DXIL})

# Resolve ShaderMake executable path per configuration when possible
if(TARGET ShaderMake)
    set(SHADERMAKE_PATH_GENEX $<TARGET_FILE:ShaderMake>)
else()
    set(SHADERMAKE_PATH_GENEX ${SHADERMAKE_PATH})
endif()

# Custom target to compile shaders offline with ShaderMake
add_custom_target(build_shaders

    # DXIL output for D3D12
    COMMAND ${SHADERMAKE_PATH_GENEX}
            -p DXIL
            --binaryBlob
            -D DXIL
            -c "${SHADER_CONFIG}"
            -o "${SHADER_OUTPUT_DIR_DXIL}"
            --embedPDB
            --compiler "${DXC_EXE}"
            --shaderModel 6_8
            --hlsl2021
            -D WITH_NRD=1
            -X "-enable-16bit-types"
            --include "${CMAKE_SOURCE_DIR}/src/shaders"
            --include "${CMAKE_SOURCE_DIR}/external/rtxdi/Include"
            --include "${CMAKE_SOURCE_DIR}/external/nvrhi/include"
            --include "${CMAKE_SOURCE_DIR}/src/shaders/rtxdi/SharedShaderInclude"
            --relaxedInclude "pch.h"

    # DXIL output for D3D12 for NRD
    COMMAND ${SHADERMAKE_PATH_GENEX}
            -p DXIL
            --binaryBlob
            -D DXIL
            -c "${NRD_SHADER_CONFIG}"
            -o "${SHADER_OUTPUT_DIR_DXIL}"
            --embedPDB
            --compiler "${DXC_EXE}"
            --allResourcesBound
            --shaderModel 6_8
            --hlsl2021
            --include "${NRD_SRC_DIR}/Shaders"
            --include "${ML_SRC_DIR}"
            -D NRD_INTERNAL

    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Compiling HLSL shaders to DXIL (D3D12) via ShaderMake"
)

# Ensure ShaderMake is built before running the shader compilation target
if(TARGET ShaderMake)
    add_dependencies(build_shaders ShaderMake)
endif()

# Shader compilation always runs before building the main executable
add_dependencies(${PROJECT_NAME} build_shaders)

# Convenience: show where ShaderMake.exe is located in the build logs
message(STATUS "ShaderMake executable: ${SHADERMAKE_PATH_GENEX}")

# ----------------------------------------------------------------------------
# Shader Resource Render Hardware Interface (SRRHI)
# ----------------------------------------------------------------------------
add_custom_target(build_srrhi
    COMMAND ${CMAKE_SOURCE_DIR}/bin/srrhi.exe
            -i "${CMAKE_SOURCE_DIR}/src/shaders"
            -o "${CMAKE_SOURCE_DIR}/src/shaders/srrhi"

    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating SRRHI shader headers via srrhi.exe"
)

# compile srrhi.exe first
add_dependencies(build_srrhi srrhi)

# ssrhi always run before shader compilation
add_dependencies(build_shaders build_srrhi)
