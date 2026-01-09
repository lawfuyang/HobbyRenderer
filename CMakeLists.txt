cmake_minimum_required(VERSION 3.16)
project(AgenticRenderer)

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

# NVRHI Configuration
set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)

add_subdirectory(external/nvrhi)

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
add_executable(${PROJECT_NAME} ${SOURCES})

# Define Windows-specific macros to prevent min/max macro conflicts
if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Precompiled header to speed up builds.
target_precompile_headers(${PROJECT_NAME} PRIVATE src/pch.h)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE src)

# SDL3 Include and Link
target_include_directories(${PROJECT_NAME} PRIVATE "${SDL3_INSTALL_DIR}/SDL3-${SDL3_VERSION}/include")
target_link_directories(${PROJECT_NAME} PRIVATE "${SDL3_INSTALL_DIR}/SDL3-${SDL3_VERSION}/lib/x64")
target_link_libraries(${PROJECT_NAME} PRIVATE SDL3)

# Link NVRHI
target_link_libraries(${PROJECT_NAME} PRIVATE nvrhi)
target_link_libraries(${PROJECT_NAME} PRIVATE nvrhi_vk)
target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan-Headers)
