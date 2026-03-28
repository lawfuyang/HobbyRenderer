#include "Renderer.h"
#include "Utilities.h"
#include "Config.h"
#include "CommonResources.h"
#include "SceneLoader.h"

#include <ShaderMake/ShaderBlob.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../external/microprofile/stb/stb_image_write.h"

#define FFX_CPU
#define FFX_STATIC static
using FfxUInt32 = uint32_t;
using FfxInt32 = int32_t;
using FfxFloat32 = float;
using FfxUInt32x2 = uint32_t[2];
using FfxUInt32x4 = uint32_t[4];
#define ffxMax(a, b) std::max(a, b)
#define ffxMin(a, b) std::min(a, b)
#include "shaders/ffx_spd.h"

#include "shaders/srrhi/cpp/SPD.h"

namespace
{
    // Parse a single line from shaders.cfg and extract shader metadata
    struct ShaderMetadata
    {
        std::filesystem::path sourcePath;
        std::string entryPoint;
        std::string suffix;
        std::vector<std::string> defines;
        nvrhi::ShaderType shaderType = nvrhi::ShaderType::None;
    };

    // ShaderMake naming: <sourcefile_without_ext>_<EntryPoint><Suffix>[_<Hash>].spirv
    std::filesystem::path GetShaderOutputPath(const std::filesystem::path& exeDir, const ShaderMetadata& metadata, nvrhi::GraphicsAPI api)
    {
        std::filesystem::path filename = metadata.sourcePath.stem();
        
        std::string outName = filename.string();

        // if the entry point is 'main', entryPoint won't be appended to the bin file name. Thanks ShaderMake. That's retarded.
        if (metadata.entryPoint != "main")
        {
            outName += "_" + metadata.entryPoint;
        }

        outName += metadata.suffix;

        std::string subDir = "spirv";
        std::string extension = ".spirv";

        if (api == nvrhi::GraphicsAPI::D3D12)
        {
            subDir = "dxil";
            extension = ".dxil";
        }

        // Preserve subdirectory structure from the source path (e.g. rtxdi/LightingPasses/DI/)
        // so that shaders in different folders with the same stem don't collide.
        const std::filesystem::path parentDir = metadata.sourcePath.parent_path();
        if (!parentDir.empty() && parentDir != ".")
            return exeDir / "shaders" / subDir / parentDir / (outName + extension);
        return exeDir / "shaders" / subDir / (outName + extension);
    }

    // Parse shaders.cfg to extract shader entries
    // Helper: Parse a define string and return all possible values
    // Handles both "KEY=VALUE" and "KEY={VAL1,VAL2,VAL3}" formats
    // Returns map of KEY -> vector of all possible values
    std::map<std::string, std::vector<std::string>> ParseDefineValues(const std::vector<std::string>& defineStrings)
    {
        std::map<std::string, std::vector<std::string>> result;
        
        for (const std::string& defineStr : defineStrings)
        {
            const size_t eqPos = defineStr.find('=');
            if (eqPos == std::string::npos)
            {
                continue; // Skip invalid defines
            }
            
            std::string key = defineStr.substr(0, eqPos);
            std::string valueStr = defineStr.substr(eqPos + 1);
            
            std::vector<std::string>& values = result[key];
            
            // Check if values are in braces (multiple values)
            if (!valueStr.empty() && valueStr[0] == '{' && valueStr.back() == '}')
            {
                // Remove braces
                valueStr = valueStr.substr(1, valueStr.length() - 2);
                
                // Split by comma
                size_t start = 0;
                while (start < valueStr.length())
                {
                    const size_t comma = valueStr.find(',', start);
                    const size_t end = (comma == std::string::npos) ? valueStr.length() : comma;
                    values.push_back(valueStr.substr(start, end - start));
                    start = (comma == std::string::npos) ? valueStr.length() : comma + 1;
                }
            }
            else
            {
                // Single value
                values.push_back(valueStr);
            }
        }
        
        return result;
    }
    
    // Helper: Generate all permutations of defines and sort them
    // Returns a vector of all combinations, each sorted alphabetically
    std::vector<std::vector<std::string>> GenerateDefinePermutations(
        const std::map<std::string, std::vector<std::string>>& defineValues)
    {
        std::vector<std::vector<std::string>> permutations;
        
        if (defineValues.empty())
        {
            permutations.push_back(std::vector<std::string>()); // One empty permutation
            return permutations;
        }
        
        // Get keys in sorted order
        std::vector<std::string> keys;
        for (const auto& [key, values] : defineValues)
        {
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        
        // Generate all combinations using cartesian product
        std::vector<size_t> indices(keys.size(), 0);
        
        while (true)
        {
            std::vector<std::string> permutation;
            for (size_t i = 0; i < keys.size(); ++i)
            {
                const std::string& key = keys[i];
                const std::string& value = defineValues.at(key)[indices[i]];
                permutation.push_back(key + "=" + value);
            }
            
            // Sort the permutation alphabetically
            std::sort(permutation.begin(), permutation.end());
            permutations.push_back(permutation);
            
            // Next combination
            size_t pos = keys.size() - 1;
            while (true)
            {
                indices[pos]++;
                if (indices[pos] < defineValues.at(keys[pos]).size())
                {
                    break;
                }
                indices[pos] = 0;
                if (pos == 0)
                {
                    // All combinations generated
                    return permutations;
                }
                --pos;
            }
        }
    }

    void ParseShaderConfig(std::string_view configPath, std::vector<ShaderMetadata>& outMetadata)
    {
        std::ifstream configFile{std::filesystem::path{configPath}};

        if (!configFile.is_open())
        {
            SDL_LOG_ASSERT_FAIL("Failed to open shader config", "[Shader] Failed to open shader config: %.*s", static_cast<int>(configPath.size()), configPath.data());
            return;
        }

        std::string line;
        while (std::getline(configFile, line))
        {
            // Skip empty lines and comments
            if (line.empty() || line[0] == '/' || line[0] == '#')
                continue;

            // Trim leading/trailing whitespace
            const size_t start = line.find_first_not_of(" \t\r\n");
            const size_t end = line.find_last_not_of(" \t\r\n");
            if (start == std::string::npos)
                continue;

            line = line.substr(start, end - start + 1);

            // Parse: <shader_path> -T <profile> -E <entry> [other options]
            std::istringstream iss{line};
            std::string token;
            ShaderMetadata baseMetadata;
            std::vector<std::string> rawDefines;

            iss >> token;
            baseMetadata.sourcePath = std::filesystem::path{token};

            while (iss >> token)
            {
                if (token == "-T" || token == "--profile")
                {
                    iss >> token;
                    if (token.find("vs") != std::string::npos)
                        baseMetadata.shaderType = nvrhi::ShaderType::Vertex;
                    else if (token.find("ps") != std::string::npos)
                        baseMetadata.shaderType = nvrhi::ShaderType::Pixel;
                    else if (token.find("gs") != std::string::npos)
                        baseMetadata.shaderType = nvrhi::ShaderType::Geometry;
                    else if (token.find("cs") != std::string::npos)
                        baseMetadata.shaderType = nvrhi::ShaderType::Compute;
                    else if (token.find("hs") != std::string::npos)
                        baseMetadata.shaderType = nvrhi::ShaderType::Hull;
                    else if (token.find("ds") != std::string::npos)
                        baseMetadata.shaderType = nvrhi::ShaderType::Domain;
                    else if (token.find("as") != std::string::npos)
                        baseMetadata.shaderType = nvrhi::ShaderType::Amplification;
                    else if (token.find("ms") != std::string::npos)
                        baseMetadata.shaderType = nvrhi::ShaderType::Mesh;
                }
                else if (token == "-E" || token == "--entryPoint")
                {
                    iss >> baseMetadata.entryPoint;
                }
                else if (token == "-s" || token == "--outputSuffix")
                {
                    iss >> baseMetadata.suffix;
                }
                else if (token == "-D" || token == "--define")
                {
                    iss >> token;
                    rawDefines.push_back(token);
                }
            }

            if (baseMetadata.entryPoint.empty())
            {
                baseMetadata.entryPoint = "main"; // Default entry point if not specified
            }

            SDL_assert(baseMetadata.shaderType != nvrhi::ShaderType::None && "Failed to parse shader entry from config");

            // Parse defines and generate all permutations
            const std::map<std::string, std::vector<std::string>> defineValues = ParseDefineValues(rawDefines);
            const std::vector<std::vector<std::string>> permutations = GenerateDefinePermutations(defineValues);
            
            // Create a metadata entry for each permutation
            for (const std::vector<std::string>& definesInPermutation : permutations)
            {
                ShaderMetadata metadata = baseMetadata;
                metadata.defines = definesInPermutation;
                outMetadata.push_back(metadata);
            }

            const char* typeStr = "Unknown";
            switch (baseMetadata.shaderType)
            {
            case nvrhi::ShaderType::Vertex:        typeStr = "VS"; break;
            case nvrhi::ShaderType::Pixel:         typeStr = "PS"; break;
            case nvrhi::ShaderType::Geometry:      typeStr = "GS"; break;
            case nvrhi::ShaderType::Compute:       typeStr = "CS"; break;
            case nvrhi::ShaderType::Hull:          typeStr = "HS"; break;
            case nvrhi::ShaderType::Domain:        typeStr = "DS"; break;
            case nvrhi::ShaderType::Amplification: typeStr = "AS"; break;
            case nvrhi::ShaderType::Mesh:          typeStr = "MS"; break;
            default: break;
            }

            SDL_Log("[Shader] Parsed: %s (%s) -> entry: %s, permutations: %zu", 
                     baseMetadata.sourcePath.generic_string().c_str(),
                     typeStr, baseMetadata.entryPoint.c_str(), permutations.size());
        }

        SDL_Log("[Shader] Parsed %zu shader entries from config", outMetadata.size());
    }

    void InitSDL()
    {
        SDL_Log("[Init] Starting SDL initialization");
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            SDL_LOG_ASSERT_FAIL("SDL_Init failed", "SDL_Init failed: %s", SDL_GetError());
            return;
        }
        SDL_Log("[Init] SDL initialized");
    }

    SDL_Window* CreateWindowScaled()
    {
        int windowW = 0;
        int windowH = 0;
        ChooseWindowSize(&windowW, &windowH);

        SDL_Log("[Init] Creating window");
        SDL_Window* window = SDL_CreateWindow("Agentic Renderer", windowW, windowH, 0);
        if (!window)
        {
            SDL_LOG_ASSERT_FAIL("SDL_CreateWindow failed", "SDL_CreateWindow failed: %s", SDL_GetError());
            return nullptr;
        }

        SDL_Log("[Init] Window created (%dx%d)", windowW, windowH);
        return window;
    }
}

Renderer* Renderer::s_Instance = nullptr;

void Renderer::SetInstance(Renderer* instance)
{
    s_Instance = instance;
}

Renderer* Renderer::GetInstance()
{
    SDL_assert(s_Instance && "Renderer instance is not set");
    return s_Instance;
}

void Renderer::LoadShaders()
{
    SDL_Log("[Init] Loading compiled shaders from config");

    // Get executable directory
    const char* basePathCStr = SDL_GetBasePath();
    if (!basePathCStr)
    {
        SDL_Log("[Init] Failed to get base path");
        return;
    }
    const std::filesystem::path exeDir = basePathCStr;

    std::vector<ShaderMetadata> shaderMetadata;

    // Parse shaders.cfg to get list of shaders to load
    std::filesystem::path configPath = exeDir / ".." / "src" / "shaders" / "shaders.cfg";    
    ParseShaderConfig(configPath.generic_string(), shaderMetadata);

    // parse NRDShaders.cfg as well for NRD shader permutations
    configPath = configPath.parent_path() / "NRDShaders.cfg";
    ParseShaderConfig(configPath.generic_string(), shaderMetadata);

    if (shaderMetadata.empty())
    {
        SDL_Log("[Init] No shaders to load from config");
        return; // Not an error, just no shaders defined yet
    }

    // Load each shader by creating a map of shader file paths to shader metadata
    std::unordered_map<std::string, std::vector<const ShaderMetadata*>> filePathToMetadata;

    for (const ShaderMetadata& metadata : shaderMetadata)
    {
        // Use GetShaderOutputPath to get the correct file path with proper extension
        // (This will be either .dxil or .spirv depending on the graphics API)
        const std::filesystem::path shaderPath = GetShaderOutputPath(exeDir, metadata, m_RHI->GetGraphicsAPI());
        filePathToMetadata[shaderPath.generic_string()].push_back(&metadata);
    }

    // Load and process each unique shader file
    size_t loadedShaderCount = 0;
    for (const auto& [shaderPathStr, metadataList] : filePathToMetadata)
    {
        const std::filesystem::path shaderPath = shaderPathStr;

        // Read the shader binary
        const std::vector<uint8_t> shaderBinary = ReadBinaryFile(shaderPath);
        if (shaderBinary.empty())
        {
            SDL_LOG_ASSERT_FAIL("Failed to read shader file", "Failed to read shader file: %s", shaderPath.generic_string().c_str());
        }

        // Detect if this is a shader blob with multiple permutations or a simple single-shader binary
        constexpr size_t BLOB_SIGNATURE_SIZE = 4;
        const char* BLOB_SIGNATURE = "NVSP";
        
        bool isShaderBlob = (shaderBinary.size() >= BLOB_SIGNATURE_SIZE) && 
                            (std::memcmp(shaderBinary.data(), BLOB_SIGNATURE, BLOB_SIGNATURE_SIZE) == 0);

        SDL_Log("[Init] Loaded shader file: %s (%zu bytes, %s)", 
                 shaderPath.generic_string().c_str(), shaderBinary.size(),
                 isShaderBlob ? "blob with permutations" : "single compiled shader");

        if (isShaderBlob)
        {
            // SHADER BLOB PATH: File contains multiple permutations
            // Enumerate all available permutations in the blob
            std::vector<std::string> availablePermutations;
            ShaderMake::EnumeratePermutationsInBlob(shaderBinary.data(), shaderBinary.size(), availablePermutations);
            SDL_Log("[Init]   Available permutations in blob: %zu", availablePermutations.size());

            // Load each requested shader permutation from the blob
            for (const ShaderMetadata* metadata : metadataList)
            {
                // ShaderMake::ShaderConstant stores raw char* pointers, causing lifetime issues
                // Create a safe vector of strings to ensure the data remains valid.
                struct ShaderConstantSafe
                {
                    std::string name;
                    std::string value;
                };

                // Build shader constants from defines for permutation lookup
                std::vector<ShaderConstantSafe> shaderConstants;
                for (const std::string& define : metadata->defines)
                {
                    // Parse "KEY=VALUE" format
                    const size_t eqPos = define.find('=');
                    if (eqPos != std::string::npos)
                    {
                        std::string key = define.substr(0, eqPos);
                        std::string value = define.substr(eqPos + 1);
                        shaderConstants.push_back(ShaderConstantSafe{ key, value });
                    }
                    else
                    {
                        SDL_LOG_ASSERT_FAIL("Invalid define format in metadata", "Invalid define format in shader metadata for shader %s: '%s'", 
                                                 shaderPath.generic_string().c_str(), define.c_str());
                    }
                }

                std::vector<ShaderMake::ShaderConstant> shaderConstantsRaw;
                for (const ShaderConstantSafe& constant : shaderConstants)
                {
                    shaderConstantsRaw.push_back({ constant.name.c_str(), constant.value.c_str() });
                }

                // Find the specific permutation in the blob
                const void* permutationBinary = nullptr;
                size_t permutationSize = 0;
                if (!ShaderMake::FindPermutationInBlob(shaderBinary.data(), shaderBinary.size(), 
                                                        shaderConstantsRaw.data(), static_cast<uint32_t>(shaderConstantsRaw.size()),
                                                        &permutationBinary, &permutationSize))
                {
                    SDL_Log("[Init] Failed to find shader permutation in blob: %s", shaderPath.generic_string().c_str());
                    const std::string errorMsg = ShaderMake::FormatShaderNotFoundMessage(shaderBinary.data(), shaderBinary.size(),
                                                                                          shaderConstantsRaw.data(), static_cast<uint32_t>(shaderConstantsRaw.size()));
                    
                    SDL_LOG_ASSERT_FAIL("Failed to find shader permutation in blob", "Failed to find shader permutation in blob: %s\n%s", 
                                             shaderPath.generic_string().c_str(), errorMsg.c_str());
                }

                // Create shader descriptor
                nvrhi::ShaderDesc desc;
                desc.shaderType = metadata->shaderType;
                desc.entryName = metadata->entryPoint;
                desc.debugName = shaderPath.generic_string();

                // Create shader handle
                const nvrhi::ShaderHandle handle = m_RHI->m_NvrhiDevice->createShader(desc, permutationBinary, permutationSize);
                if (!handle)
                {
                    SDL_LOG_ASSERT_FAIL("Failed to create shader handle", "Failed to create shader handle for: %s", shaderPath.generic_string().c_str());
                }

                // Keyed by logical name with folder prefix and sorted defines
                // e.g. "rtxdi/LightingPasses/DI/GenerateInitialSamples_main" or "ForwardLighting_PSMain_KEY=VAL"
                {
                    const std::filesystem::path parentDir = metadata->sourcePath.parent_path();
                    std::string folderPrefix = (!parentDir.empty() && parentDir != ".")
                        ? parentDir.generic_string() + "/" : "";
                    std::string key = folderPrefix + metadata->sourcePath.stem().string() + "_" + metadata->entryPoint + metadata->suffix;
                    for (const std::string& define : metadata->defines)
                        key += "_" + define;

                    SDL_assert(!m_ShaderCache.contains(key));
                    m_ShaderCache[key] = handle;

                    SDL_Log("[Init] Loaded shader: %s (key=%s, permutations=%zu)",
                             shaderPath.generic_string().c_str(), key.c_str(), shaderConstants.size());
                    ++loadedShaderCount;
                }
            }
        }
        else
        {
            // SINGLE SHADER PATH: File contains only one compiled shader (no permutations)
            // This is used when the shader has no permutation variations
            for (const ShaderMetadata* metadata : metadataList)
            {
                // Verify that a single-shader file should have no defines
                if (!metadata->defines.empty())
                {
                    SDL_assert(metadata->defines.empty() && "Shader with permutations should be stored as a blob");
                }

                // Create shader descriptor
                nvrhi::ShaderDesc desc;
                desc.shaderType = metadata->shaderType;
                desc.entryName = metadata->entryPoint;
                desc.debugName = shaderPath.generic_string();

                // Create shader handle from the entire binary
                const nvrhi::ShaderHandle handle = m_RHI->m_NvrhiDevice->createShader(desc, shaderBinary.data(), shaderBinary.size());
                if (!handle)
                {
                    SDL_LOG_ASSERT_FAIL("Failed to create shader handle", "Failed to create shader handle for: %s", shaderPath.generic_string().c_str());
                }

                // Keyed by logical name with folder prefix (e.g. "rtxdi/PostprocessGBuffer_main")
                {
                    const std::filesystem::path parentDir = metadata->sourcePath.parent_path();
                    std::string folderPrefix = (!parentDir.empty() && parentDir != ".")
                        ? parentDir.generic_string() + "/" : "";
                    const std::string key = folderPrefix + metadata->sourcePath.stem().string() + "_" + metadata->entryPoint + metadata->suffix;

                    SDL_assert(!m_ShaderCache.contains(key));
                    m_ShaderCache[key] = handle;

                    SDL_Log("[Init] Loaded shader: %s (key=%s, no permutations)",
                             shaderPath.generic_string().c_str(), key.c_str());
                    ++loadedShaderCount;
                }
            }
        }
    }

    SDL_Log("[Init] All %zu shader(s) loaded successfully from %zu file(s)", loadedShaderCount, filePathToMetadata.size());
}

void Renderer::UnloadShaders()
{
    SDL_Log("[Shutdown] Unloading shaders");
    m_ShaderCache.clear();
    SDL_Log("[Shutdown] Shaders unloaded");
}

void Renderer::ReloadShaders()
{
    SDL_Log("[HotReload] Reloading shaders requested...");

    // Wait for device to be idle before destroying resources that might be in use
    m_RHI->m_NvrhiDevice->waitForIdle();

    // Clear all caches
    m_GraphicsPipelineCache.clear();
    m_MeshletPipelineCache.clear();
    m_ComputePipelineCache.clear();
    m_BindingLayoutCache.clear();
    m_ShaderCache.clear();

    SDL_Log("[HotReload] Caches cleared. Re-loading shaders from disk...");

    LoadShaders();

    SDL_Log("[HotReload] Shader reload complete.");
    m_RequestedShaderReload = false;
}

nvrhi::ShaderHandle Renderer::GetShaderHandle(std::string_view name) const
{
    std::unordered_map<std::string, nvrhi::ShaderHandle>::const_iterator it = m_ShaderCache.find(name.data());
    if (it != m_ShaderCache.end())
        return it->second;
    
    SDL_LOG_ASSERT_FAIL("Requested shader not found in cache", "[Error] Shader '%s' not found in cache!", name.data());
    return {};
}

nvrhi::TextureHandle Renderer::GetCurrentBackBufferTexture() const
{
    return m_RHI->m_NvrhiSwapchainTextures[m_SwapChainImageIdx];
}

void Renderer::SaveBackBufferScreenshot()
{
    PROFILE_FUNCTION();
    
    // Get the backbuffer texture
    nvrhi::TextureHandle backbuffer = GetCurrentBackBufferTexture();

    // Get backbuffer dimensions
    const nvrhi::TextureDesc& bbDesc = backbuffer->getDesc();
    uint32_t width = bbDesc.width;
    uint32_t height = bbDesc.height;
    nvrhi::Format format = bbDesc.format;

    SDL_assert(width > 0 && height > 0 && "Backbuffer has invalid dimensions");
    SDL_assert(format != nvrhi::Format::UNKNOWN && "Backbuffer has unknown format");

    // Create a staging texture for readback  
    nvrhi::TextureDesc stagingDesc;
    stagingDesc.width = width;
    stagingDesc.height = height;
    stagingDesc.format = format;
    stagingDesc.debugName = "Screenshot Staging Texture";

    nvrhi::StagingTextureHandle stagingTexture = m_RHI->m_NvrhiDevice->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);

    // Acquire a command list for the copy operation
    nvrhi::CommandListHandle cmdList = AcquireCommandList();

    {
        // Copy from backbuffer to staging texture
        ScopedCommandList scoped(cmdList, "Screenshot Copy");
        cmdList->copyTexture(stagingTexture, nvrhi::TextureSlice{}, backbuffer, nvrhi::TextureSlice{});
    }

    // Execute the command list immediately - this will synchronize
    ExecutePendingCommandLists();

    // Map the staging texture and read the data
    size_t rowPitch = 0;
    void* mappedData = m_RHI->m_NvrhiDevice->mapStagingTexture(stagingTexture, nvrhi::TextureSlice{}, nvrhi::CpuAccessMode::Read, &rowPitch);
    SDL_assert(mappedData && "Failed to map staging texture for screenshot");

    // Get the base path where to save the screenshot
    const char* basePath = SDL_GetBasePath();
    std::string screenshotPath{ basePath };
    screenshotPath += "screenshot.jpg";

    // Write the image as JPEG
    int writeResult = stbi_write_jpg(screenshotPath.c_str(), (int)width, (int)height, 4, mappedData, 0);
    SDL_assert(writeResult && "Failed to write screenshot using stb_image_write");

    // Unmap the staging texture
    m_RHI->m_NvrhiDevice->unmapStagingTexture(stagingTexture);
}

void Renderer::Initialize()
{
    ScopedTimerLog initScope{"[Timing] Init phase:"};

    MicroProfileOnThreadCreate("Main");
	MicroProfileSetEnableAllGroups(true);
	MicroProfileSetForceMetaCounters(true);

    m_TaskScheduler = std::make_unique<TaskScheduler>();

    InitSDL();

    m_Window = CreateWindowScaled();
    if (!m_Window)
    {
        SDL_Quit();
        return;
    }

    m_RHI = CreateGraphicRHI();
    m_RHI->Initialize(m_Window);

    SDL_assert(m_RHI->m_NvrhiDevice && "NVRHI device is null after RHI initialization");
    SDL_assert(m_RHI->m_NvrhiDevice->queryFeatureSupport(nvrhi::Feature::HeapDirectlyIndexed));
    SDL_assert(m_RHI->m_NvrhiDevice->queryFeatureSupport(nvrhi::Feature::Meshlets));
    SDL_assert(m_RHI->m_NvrhiDevice->queryFeatureSupport(nvrhi::Feature::RayQuery));
    SDL_assert(m_RHI->m_NvrhiDevice->queryFeatureSupport(nvrhi::Feature::RayTracingAccelStruct));

    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);

    if (!m_RHI->CreateSwapchain(static_cast<uint32_t>(windowWidth), static_cast<uint32_t>(windowHeight)))
    {
        Shutdown();
        return;
    }

    const char* basePathCStr = SDL_GetBasePath();
    m_IrradianceTexturePath = (std::filesystem::path{ basePathCStr } / "irradiance.dds").string();
    m_RadianceTexturePath = (std::filesystem::path{ basePathCStr } / "radiance.dds").string();
    m_BRDFLutTexture = (std::filesystem::path{ basePathCStr } / "brdf_lut.dds").string();

    InitializeStaticBindlessTextures();
    InitializeStaticBindlessSamplers();
    CommonResources::GetInstance().Initialize();
    CommonResources::GetInstance().RegisterDefaultTextures();
    LoadShaders();

    m_ImGuiLayer.Initialize();

    // Initialize renderers now that shaders and device are ready
    for (const RendererRegistry::Creator& creator : RendererRegistry::GetCreators())
    {
        std::shared_ptr<IRenderer> renderer = creator();
        renderer->Initialize();
        
        m_Renderers.push_back(renderer);
        renderer->m_GPUQueries[0] = m_RHI->m_NvrhiDevice->createTimerQuery();
        renderer->m_GPUQueries[1] = m_RHI->m_NvrhiDevice->createTimerQuery();
    }

    m_GPUQueries[0] = m_RHI->m_NvrhiDevice->createTimerQuery();
    m_GPUQueries[1] = m_RHI->m_NvrhiDevice->createTimerQuery();

    // Load scene (if configured) after all renderer resources are ready
    m_Scene.LoadScene();

    // Initialize renderers with scene-dependent resources
    for (const std::shared_ptr<IRenderer>& renderer : m_Renderers)
    {
        renderer->PostSceneLoad();
    }

    ExecutePendingCommandLists();
}

void Renderer::Run()
{
    ScopedTimerLog runScope{"[Timing] Run phase:"};

    SDL_Log("[Run ] Entering main loop");

    while (m_Running)
    {
        PROFILE_SCOPED("Frame");
        const uint64_t frameStart = SDL_GetTicksNS();
        const uint32_t kFrameDurationNs = SDL_NS_PER_SECOND / m_TargetFPS;

        {
            PROFILE_SCOPED("Event Polling");

            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                m_ImGuiLayer.ProcessEvent(event);
                m_Scene.m_Camera.ProcessEvent(event);

                if (event.type == SDL_EVENT_QUIT)
                {
                    SDL_Log("[Run ] Received quit event");
                    m_Running = false;
                    break;
                }

                if (event.type == SDL_EVENT_KEY_DOWN)
                {
                    if (event.key.scancode == SDL_SCANCODE_F5)
                    {
                        m_RequestedShaderReload = true;
                    }

                    if ((event.key.mod & SDL_KMOD_CTRL) != 0 && event.key.scancode == SDL_SCANCODE_P)
                    {
                        SaveBackBufferScreenshot();
                    }
                }
            }
        };

        SDL_WindowFlags flags = SDL_GetWindowFlags(m_Window);
        const bool bWindowIsInFocus = (flags & SDL_WINDOW_INPUT_FOCUS) != 0;

        if (!bWindowIsInFocus)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue; // Skip rendering when window is not in focus to save resources
        }

        if (m_RequestedShaderReload)
        {
            ReloadShaders();
        }

        const bool bSwapChainImageAcquireSuccess = m_RHI->AcquireNextSwapchainImage(&m_AcquiredSwapchainImageIdx);
        
        m_TaskScheduler->ScheduleTask([this]() {
            PROFILE_SCOPED("Garbage Collection");
            m_RHI->m_NvrhiDevice->runGarbageCollection();
        });

        // Prepare ImGui UI (NewFrame + UI creation + ImGui::Render)
        m_ImGuiLayer.UpdateFrame();

        // Update animations
        if (m_EnableAnimations)
        {
            m_Scene.Update(static_cast<float>(m_FrameTime / 1000.0));

            if (m_Scene.AreInstanceTransformsDirty())
            {
                nvrhi::CommandListHandle cmd = AcquireCommandList();
                ScopedCommandList scopedCmd{ cmd, "Upload Animated Instances" };
                uint32_t startIdx = m_Scene.m_InstanceDirtyRange.first;
                uint32_t count = m_Scene.m_InstanceDirtyRange.second - startIdx + 1;
                scopedCmd->writeBuffer(m_Scene.m_InstanceDataBuffer,
                    &m_Scene.m_InstanceData[startIdx],
                    count * sizeof(PerInstanceData),
                    startIdx * sizeof(PerInstanceData));

                if (m_Scene.m_RTInstanceDescBuffer)
                {
                    scopedCmd->writeBuffer(m_Scene.m_RTInstanceDescBuffer,
                        &m_Scene.m_RTInstanceDescs[startIdx],
                        count * sizeof(nvrhi::rt::InstanceDesc),
                        startIdx * sizeof(nvrhi::rt::InstanceDesc));
                }
            }
        }

        if (m_Scene.m_LightsDirty)
        {
            SceneLoader::CreateAndUploadLightBuffer(m_Scene, this);
            m_Scene.m_LightsDirty = false;
        }

        // Upload material constants for materials changed by emissive intensity animations
        if (m_Scene.m_MaterialDirtyRange.first <= m_Scene.m_MaterialDirtyRange.second && m_Scene.m_MaterialConstantsBuffer)
        {
            PROFILE_SCOPED("Upload Animated Materials");
            const uint32_t firstMat = m_Scene.m_MaterialDirtyRange.first;
            const uint32_t lastMat  = m_Scene.m_MaterialDirtyRange.second;
            const uint32_t count    = lastMat - firstMat + 1;

            std::vector<MaterialConstants> materialConstants(count);
            for (uint32_t i = 0; i < count; ++i)
                materialConstants[i] = MaterialConstantsFromMaterial(m_Scene.m_Materials[firstMat + i], m_Scene.m_Textures);

            nvrhi::CommandListHandle cmd = AcquireCommandList();
            ScopedCommandList scopedCmd{ cmd, "Upload Animated Material Constants" };
            scopedCmd->writeBuffer(m_Scene.m_MaterialConstantsBuffer,
                materialConstants.data(),
                count * sizeof(MaterialConstants),
                firstMat * sizeof(MaterialConstants));

            m_Scene.m_MaterialDirtyRange = { UINT32_MAX, 0 };
        }

        // Update camera (camera retrieves frame time internally)
        m_Scene.m_ViewPrev = m_Scene.m_View;
        m_Scene.m_Camera.Update();

        // Handle debug mode settings
        if (m_DebugMode != m_ActiveDebugMode)
        {
            if (m_DebugMode != srrhi::CommonConsts::DEBUG_MODE_NONE && m_ActiveDebugMode == srrhi::CommonConsts::DEBUG_MODE_NONE)
            {
                // Entering debug mode: save current state
                m_DebugBackup.m_EnableBloom = m_EnableBloom;
                m_DebugBackup.m_EnableAutoExposure = m_EnableAutoExposure;
                m_DebugBackup.m_ExposureValue = m_Scene.m_Camera.m_ExposureValue;
                m_DebugBackup.m_ExposureCompensation = m_Scene.m_Camera.m_ExposureCompensation;

                // Set debug defaults
                m_EnableBloom = false;
                m_EnableAutoExposure = false;
            }
            else if (m_DebugMode == srrhi::CommonConsts::DEBUG_MODE_NONE && m_ActiveDebugMode != srrhi::CommonConsts::DEBUG_MODE_NONE)
            {
                // Leaving debug mode: restore state
                m_EnableBloom = m_DebugBackup.m_EnableBloom;
                m_EnableAutoExposure = m_DebugBackup.m_EnableAutoExposure;
                m_Scene.m_Camera.m_ExposureValue = m_DebugBackup.m_ExposureValue;
                m_Scene.m_Camera.m_ExposureCompensation = m_DebugBackup.m_ExposureCompensation;
            }
            m_ActiveDebugMode = m_DebugMode;
        }

        if (m_DebugMode != srrhi::CommonConsts::DEBUG_MODE_NONE)
        {
            // Lock settings in debug mode for consistent raw output
            m_EnableBloom = false;
            m_EnableAutoExposure = false;
            m_Scene.m_Camera.m_Exposure = 1.0f;
        }

        int windowW, windowH;
        SDL_GetWindowSize(m_Window, &windowW, &windowH);
        m_Scene.m_ViewPrev = m_Scene.m_View;
        m_Scene.m_Camera.FillPlanarViewConstants(m_Scene.m_View, (float)windowW, (float)windowH);

        const int readIndex = m_FrameNumber % 2;
        const int writeIndex = (m_FrameNumber + 1) % 2;

        // GPU query for frame timer is super expensive on the CPU for some reason. i give up using it
        if constexpr (false)
        {
            PROFILE_SCOPED("GPU Frame Start");
            nvrhi::CommandListHandle cmd = AcquireCommandList();
            ScopedCommandList scopedCmd{ cmd, "GPU Frame Start" };
            m_RHI->m_NvrhiDevice->resetTimerQuery(m_GPUQueries[readIndex]);
            scopedCmd->beginTimerQuery(m_GPUQueries[writeIndex]);
        }

        for (const std::shared_ptr<IRenderer>& renderer : m_Renderers)
        {
            renderer->m_bPassEnabled = false;
        }
        
        m_RenderGraph.Reset();

        extern IRenderer* g_TLASRenderer;
        extern IRenderer* g_ClearRenderer;
        extern IRenderer* g_OpaqueRenderer;
        extern IRenderer* g_MaskedPassRenderer;
        extern IRenderer* g_HZBGeneratorPhase2;
        extern IRenderer* g_RTXDIRenderer;
        extern IRenderer* g_DeferredRenderer;
        extern IRenderer* g_SkyRenderer;
        extern IRenderer* g_TransparentPassRenderer;
        extern IRenderer* g_BloomRenderer;
        extern IRenderer* g_HDRRenderer;
        extern IRenderer* g_ImGuiRenderer;
        extern IRenderer* g_PathTracerRenderer;

        m_RenderGraph.ScheduleRenderer(g_ClearRenderer);

        if (m_Mode == RenderingMode::ReferencePathTracer)
        {
            m_RenderGraph.ScheduleRenderer(g_PathTracerRenderer);
        }
        else
        {
            m_RenderGraph.ScheduleRenderer(g_OpaqueRenderer);
            m_RenderGraph.ScheduleRenderer(g_MaskedPassRenderer);
            m_RenderGraph.ScheduleRenderer(g_HZBGeneratorPhase2);
            m_RenderGraph.ScheduleRenderer(g_TLASRenderer);
            m_RenderGraph.ScheduleRenderer(g_RTXDIRenderer);
            m_RenderGraph.ScheduleRenderer(g_DeferredRenderer);
            m_RenderGraph.ScheduleRenderer(g_SkyRenderer);
            m_RenderGraph.ScheduleRenderer(g_TransparentPassRenderer);
            m_RenderGraph.ScheduleRenderer(g_BloomRenderer);
        }

        m_RenderGraph.ScheduleRenderer(g_HDRRenderer);
        m_RenderGraph.ScheduleRenderer(g_ImGuiRenderer);

        // Compile render graph: compute lifetimes and allocate resources
        m_RenderGraph.Compile();

        // Wait for all render passes to finish recording
        m_TaskScheduler->ExecuteAllScheduledTasks();

        // GPU query for frame timer is super expensive on the CPU for some reason. i give up using it
        if constexpr (false)
        {
            PROFILE_SCOPED("GPU Frame End");
            nvrhi::CommandListHandle cmd = AcquireCommandList();
            ScopedCommandList scopedCmd{ cmd, "GPU Frame End" };
            scopedCmd->endTimerQuery(m_GPUQueries[writeIndex]);
        }

        // Execute any queued GPU work in submission order
        ExecutePendingCommandLists();

        // Present swapchain
        SDL_assert(bSwapChainImageAcquireSuccess);
        SDL_assert(m_SwapChainImageIdx == m_SwapChainImageIdx);
        if (!m_RHI->PresentSwapchain(m_SwapChainImageIdx))
        {
            SDL_LOG_ASSERT_FAIL("PresentSwapchain failed", "[Run ] PresentSwapchain failed");
            break;
        }
        m_SwapChainImageIdx = 1 - m_SwapChainImageIdx;

        const uint64_t workTimeNs = SDL_GetTicksNS() - frameStart;

        // Sleep to maintain target framerate (if needed)
        if (workTimeNs < kFrameDurationNs)
        {
            PROFILE_SCOPED("Sleep");
            SDL_Delay(static_cast<uint32_t>(SDL_NS_TO_MS(kFrameDurationNs  - workTimeNs)));
        }

        // Recompute total frame time (including any sleep) so reported FPS matches ImGui's DeltaTime
        const uint64_t totalFrameTime = SDL_GetTicksNS() - frameStart;

        // Calculate frame time (ms) and FPS
        m_FrameTime = SDL_NS_TO_MS(static_cast<double>(totalFrameTime));
        if (m_FrameTime > 0.0)
            m_FPS = 1000.0 / m_FrameTime;

        // Increment frame number for double buffering
        m_FrameNumber++;

        MicroProfileFlip(nullptr);
    }
}

void Renderer::Shutdown()
{
    ScopedTimerLog shutdownScope{"[Timing] Shutdown phase:"};

    MicroProfileShutdown();

    m_RHI->m_NvrhiDevice->waitForIdle();
    m_RHI->m_NvrhiDevice->runGarbageCollection();

    m_RenderGraph.Shutdown();

    m_InFlightCommandLists.clear();
    m_PendingCommandLists.clear();
    m_CommandListFreeList.clear();

    m_ImGuiLayer.Shutdown();
    CommonResources::GetInstance().Shutdown();

    // Shutdown global bindless systems
    m_StaticTextureDescriptorTable = nullptr;
    m_StaticTextureBindingLayout = nullptr;
    m_NextTextureIndex = 0;

    m_StaticSamplerDescriptorTable = nullptr;
    m_StaticSamplerBindingLayout = nullptr;

    // Shutdown scene and free its GPU resources
    m_Scene.Shutdown();

    m_RadianceTexture = nullptr;
    m_IrradianceTexture = nullptr;

    // Free renderer instances
    m_Renderers.clear();

    m_GPUQueries[0] = nullptr;
    m_GPUQueries[1] = nullptr;

    m_BindingLayoutCache.clear();
    m_GraphicsPipelineCache.clear();
    m_ComputePipelineCache.clear();
    m_MeshletPipelineCache.clear();

    UnloadShaders();

    m_RHI->m_NvrhiDevice->waitForIdle();
    m_RHI->m_NvrhiDevice->runGarbageCollection();

    m_RHI->Shutdown();

    if (m_Window)
    {
        SDL_DestroyWindow(m_Window);
        m_Window = nullptr;
    }

    SDL_Quit();
    SDL_Log("[Shutdown] Clean exit");
}

void Renderer::SetCameraFromSceneCamera(const Scene::Camera& sceneCam)
{
    if (sceneCam.m_NodeIndex >= 0 && sceneCam.m_NodeIndex < static_cast<int>(m_Scene.m_Nodes.size()))
    {
        const Matrix& worldTransform = m_Scene.m_Nodes[sceneCam.m_NodeIndex].m_WorldTransform;
        m_Scene.m_Camera.SetFromMatrix(worldTransform);
        m_Scene.m_Camera.SetProjection(sceneCam.m_Projection);

        m_Scene.m_Camera.m_ExposureValue = sceneCam.m_ExposureValue;
        m_Scene.m_Camera.m_ExposureCompensation = sceneCam.m_ExposureCompensation;
        m_Scene.m_Camera.m_ExposureValueMin = sceneCam.m_ExposureValueMin;
        m_Scene.m_Camera.m_ExposureValueMax = sceneCam.m_ExposureValueMax;
    }
}

// Hash helper for BindingLayoutDesc
static size_t HashBindingLayoutDesc(const nvrhi::BindingLayoutDesc& d)
{
    size_t h = std::hash<uint32_t>()(static_cast<uint32_t>(d.visibility));
    h = h * 1315423911u + std::hash<uint32_t>()(d.registerSpace);
    h = h * 1315423911u + std::hash<uint32_t>()(d.registerSpaceIsDescriptorSet ? 1u : 0u);
    h = h * 1315423911u + std::hash<uint32_t>()(d.bindingOffsets.shaderResource);
    h = h * 1315423911u + std::hash<uint32_t>()(d.bindingOffsets.sampler);
    h = h * 1315423911u + std::hash<uint32_t>()(d.bindingOffsets.constantBuffer);
    h = h * 1315423911u + std::hash<uint32_t>()(d.bindingOffsets.unorderedAccess);
    for (const nvrhi::BindingLayoutItem& it : d.bindings)
    {
        // combine slot, type and size
        uint64_t v = (uint64_t(it.slot) << 32) | (uint64_t(it.type) << 16) | uint64_t(it.size);
        h = h * 1315423911u + std::hash<uint64_t>()(v);
    }
    return h;
}

static size_t HashBindlessLayoutDesc(const nvrhi::BindlessLayoutDesc& d)
{
    size_t h = std::hash<uint32_t>()(static_cast<uint32_t>(d.visibility));
    h = h * 1315423911u + std::hash<uint32_t>()(d.firstSlot);
    h = h * 1315423911u + std::hash<uint32_t>()(d.maxCapacity);
    h = h * 1315423911u + std::hash<uint32_t>()(static_cast<uint32_t>(d.layoutType));
    for (const nvrhi::BindingLayoutItem& it : d.registerSpaces)
    {
        // combine slot, type and size
        uint64_t v = (uint64_t(it.slot) << 32) | (uint64_t(it.type) << 16) | uint64_t(it.size);
        h = h * 1315423911u + std::hash<uint64_t>()(v);
    }
    return h;
}

static bool BindingLayoutDescEqual(const nvrhi::BindingLayoutDesc& a, const nvrhi::BindingLayoutDesc& b)
{
    if (a.visibility != b.visibility) return false;
    if (a.registerSpace != b.registerSpace) return false;
    if (a.registerSpaceIsDescriptorSet != b.registerSpaceIsDescriptorSet) return false;
    if (a.bindingOffsets.shaderResource != b.bindingOffsets.shaderResource) return false;
    if (a.bindingOffsets.sampler != b.bindingOffsets.sampler) return false;
    if (a.bindingOffsets.constantBuffer != b.bindingOffsets.constantBuffer) return false;
    if (a.bindingOffsets.unorderedAccess != b.bindingOffsets.unorderedAccess) return false;
    if (a.bindings.size() != b.bindings.size()) return false;
    for (size_t i = 0; i < a.bindings.size(); ++i)
    {
        if (a.bindings[i] != b.bindings[i])
            return false;
    }
    return true;
}

nvrhi::BindingLayoutHandle Renderer::GetOrCreateBindingLayoutFromBindingSetDesc(const nvrhi::BindingSetDesc& setDesc, uint32_t registerSpace)
{
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.registerSpace = registerSpace;
    layoutDesc.registerSpaceIsDescriptorSet = true;

    for (const nvrhi::BindingSetItem& item : setDesc.bindings)
    {
        nvrhi::BindingLayoutItem b{};
        b.slot = item.slot;
        b.type = item.type;
        if (item.type == nvrhi::ResourceType::PushConstants)
            b.size = uint16_t(item.range.byteSize);
        else
            b.size = 1;
        layoutDesc.bindings.push_back(b);
    }

    // Sort deterministically: slot then type
    std::sort(layoutDesc.bindings.begin(), layoutDesc.bindings.end(), [](const nvrhi::BindingLayoutItem& a, const nvrhi::BindingLayoutItem& b){
        if (a.slot != b.slot) return a.slot < b.slot;
        return a.type < b.type;
    });

    // Hash and lookup in cache
    const size_t h = HashBindingLayoutDesc(layoutDesc);

    std::lock_guard<std::mutex> lock(m_CacheMutex);

    auto cacheIt = m_BindingLayoutCache.find(h);
    if (cacheIt != m_BindingLayoutCache.end())
    {
        return cacheIt->second;
    }

    // Not found - create it and cache it
    nvrhi::BindingLayoutHandle handle = m_RHI->m_NvrhiDevice->createBindingLayout(layoutDesc);
    if (handle)
    {
        m_BindingLayoutCache.emplace(h, handle);
    }

    return handle;
}

nvrhi::BindingLayoutHandle Renderer::GetOrCreateBindlessLayout(const nvrhi::BindlessLayoutDesc& desc)
{
    SINGLE_THREAD_GUARD();

    // Hash and lookup in cache
    const size_t h = HashBindlessLayoutDesc(desc);
        auto cacheIt = m_BindingLayoutCache.find(h);
        if (cacheIt != m_BindingLayoutCache.end())
        {
            return cacheIt->second;
    }

    // Not found - create it and cache it
    nvrhi::BindingLayoutHandle handle = m_RHI->m_NvrhiDevice->createBindlessLayout(desc);
    if (handle)
    {
        m_BindingLayoutCache.emplace(h, handle);
    }

    return handle;
}

void Renderer::InitializeStaticBindlessTextures()
{
    static const uint32_t kInitialTextureCapacity = 1024;

    // Create bindless layout for static textures
    nvrhi::BindlessLayoutDesc bindlessDesc;
    bindlessDesc.visibility = nvrhi::ShaderType::All;
    bindlessDesc.maxCapacity = kInitialTextureCapacity; // Large capacity for many textures
    bindlessDesc.layoutType = nvrhi::BindlessLayoutDesc::LayoutType::MutableSrvUavCbv;

    m_StaticTextureBindingLayout = GetOrCreateBindlessLayout(bindlessDesc);
    if (!m_StaticTextureBindingLayout)
    {
        SDL_LOG_ASSERT_FAIL("Failed to create static bindless layout for textures", "[Renderer] Failed to create static bindless layout for textures");
        return;
    }

    // Create descriptor table
    m_StaticTextureDescriptorTable = m_RHI->m_NvrhiDevice->createDescriptorTable(m_StaticTextureBindingLayout);
    if (!m_StaticTextureDescriptorTable)
    {
        SDL_LOG_ASSERT_FAIL("Failed to create static texture descriptor table", "[Renderer] Failed to create static texture descriptor table");
        return;
    }

    m_RHI->m_NvrhiDevice->resizeDescriptorTable(m_StaticTextureDescriptorTable, bindlessDesc.maxCapacity, false);
    
    SDL_Log("[Renderer] Static bindless texture system initialized");
}

uint32_t Renderer::RegisterTexture(nvrhi::TextureHandle texture)
{
    if (!texture || !m_StaticTextureDescriptorTable)
    {
        SDL_LOG_ASSERT_FAIL("Invalid texture or descriptor table not initialized", "[Renderer] Invalid texture or descriptor table not initialized");
        return UINT32_MAX;
    }

    SINGLE_THREAD_GUARD();

    const uint32_t index = m_NextTextureIndex++;
    const bool bResult = RegisterTextureAtIndex(index, texture);
    if (!bResult)
    {
        SDL_LOG_ASSERT_FAIL("Failed to register texture in global descriptor table", "[Renderer] Failed to register texture at index %u", index);
        return UINT32_MAX;
    }
    return index;
}

bool Renderer::RegisterTextureAtIndex(uint32_t index, nvrhi::TextureHandle texture)
{
    if (!texture || !m_StaticTextureDescriptorTable)
    {
        return false;
    }

    SINGLE_THREAD_GUARD();

    const nvrhi::BindingSetItem item = nvrhi::BindingSetItem::Texture_SRV(index, texture);
    if (!m_RHI->m_NvrhiDevice->writeDescriptorTable(m_StaticTextureDescriptorTable, item))
    {
        SDL_LOG_ASSERT_FAIL("Failed to register texture in static descriptor table", "[Renderer] Failed to register texture at index %u", index);
        return false;
    }
    SDL_Log("[Renderer] Registered texture (%s) at index %u", texture->getDesc().debugName.c_str(), index);
    return true;
}

void Renderer::InitializeStaticBindlessSamplers()
{
    static const uint32_t kInitialSamplerCapacity = 128;

    // Create bindless layout for static samplers
    nvrhi::BindlessLayoutDesc bindlessDesc;
    bindlessDesc.visibility = nvrhi::ShaderType::All;
    bindlessDesc.maxCapacity = kInitialSamplerCapacity;
    bindlessDesc.layoutType = nvrhi::BindlessLayoutDesc::LayoutType::MutableSampler;

    m_StaticSamplerBindingLayout = GetOrCreateBindlessLayout(bindlessDesc);
    if (!m_StaticSamplerBindingLayout)
    {
        SDL_LOG_ASSERT_FAIL("Failed to create static bindless layout for samplers", "[Renderer] Failed to create static bindless layout for samplers");
        return;
    }

    // Create descriptor table
    m_StaticSamplerDescriptorTable = m_RHI->m_NvrhiDevice->createDescriptorTable(m_StaticSamplerBindingLayout);
    if (!m_StaticSamplerDescriptorTable)
    {
        SDL_LOG_ASSERT_FAIL("Failed to create static sampler descriptor table", "[Renderer] Failed to create static sampler descriptor table");
        return;
    }

    m_RHI->m_NvrhiDevice->resizeDescriptorTable(m_StaticSamplerDescriptorTable, bindlessDesc.maxCapacity, false);

    SDL_Log("[Renderer] Static bindless sampler system initialized");
}

bool Renderer::RegisterSamplerAtIndex(uint32_t index, nvrhi::SamplerHandle sampler)
{
    if (!sampler || !m_StaticSamplerDescriptorTable)
    {
        return false;
    }

    SINGLE_THREAD_GUARD();

    const nvrhi::BindingSetItem item = nvrhi::BindingSetItem::Sampler(index, sampler);
    if (!m_RHI->m_NvrhiDevice->writeDescriptorTable(m_StaticSamplerDescriptorTable, item))
    {
        SDL_LOG_ASSERT_FAIL("Failed to register sampler in global descriptor table", "[Renderer] Failed to register sampler at index %u", index);
        return false;
    }
    SDL_Log("[Renderer] Registered sampler at index %u", index);
    return true;
}

void Renderer::HashPipelineCommonState(size_t& h, const nvrhi::RenderState& renderState, const nvrhi::FramebufferInfoEx& fbInfo, const nvrhi::BindingLayoutVector& bindingLayouts)
{
    // Raster State
    const nvrhi::RasterState& rs = renderState.rasterState;
    h = h * 1099511628211u + std::hash<int>()((int)rs.fillMode);
    h = h * 1099511628211u + std::hash<int>()((int)rs.cullMode);
    h = h * 1099511628211u + std::hash<bool>()(rs.frontCounterClockwise);
    h = h * 1099511628211u + std::hash<int>()(rs.depthBias);
    h = h * 1099511628211u + std::hash<float>()(rs.slopeScaledDepthBias);
    h = h * 1099511628211u + std::hash<bool>()(rs.depthClipEnable);
    h = h * 1099511628211u + std::hash<bool>()(rs.scissorEnable);
    h = h * 1099511628211u + std::hash<bool>()(rs.multisampleEnable);
    h = h * 1099511628211u + std::hash<bool>()(rs.conservativeRasterEnable);

    // Depth Stencil State
    const nvrhi::DepthStencilState& dss = renderState.depthStencilState;
    h = h * 1099511628211u + std::hash<bool>()(dss.depthTestEnable);
    h = h * 1099511628211u + std::hash<bool>()(dss.depthWriteEnable);
    h = h * 1099511628211u + std::hash<int>()((int)dss.depthFunc);
    h = h * 1099511628211u + std::hash<bool>()(dss.stencilEnable);
    h = h * 1099511628211u + std::hash<uint32_t>()(dss.stencilReadMask);
    h = h * 1099511628211u + std::hash<uint32_t>()(dss.stencilWriteMask);

    // Blend State
    const nvrhi::BlendState& bs = renderState.blendState;
    h = h * 1099511628211u + std::hash<bool>()(bs.alphaToCoverageEnable);
    for (const nvrhi::BlendState::RenderTarget& target : bs.targets)
    {
        h = h * 1099511628211u + std::hash<bool>()(target.blendEnable);
        h = h * 1099511628211u + std::hash<int>()((int)target.srcBlend);
        h = h * 1099511628211u + std::hash<int>()((int)target.destBlend);
        h = h * 1099511628211u + std::hash<int>()((int)target.blendOp);
        h = h * 1099511628211u + std::hash<int>()((int)target.srcBlendAlpha);
        h = h * 1099511628211u + std::hash<int>()((int)target.destBlendAlpha);
        h = h * 1099511628211u + std::hash<int>()((int)target.blendOpAlpha);
        h = h * 1099511628211u + std::hash<int>()((int)target.colorWriteMask);
    }

    // Framebuffer Info
    h = h * 1099511628211u + std::hash<int>()((int)fbInfo.depthFormat);
    for (nvrhi::Format format : fbInfo.colorFormats)
    {
        h = h * 1099511628211u + std::hash<int>()((int)format);
    }
    h = h * 1099511628211u + std::hash<uint32_t>()(fbInfo.sampleCount);

    // Binding Layouts
    for (const nvrhi::BindingLayoutHandle& bl : bindingLayouts)
    {
        h = h * 1099511628211u + std::hash<const void*>()(bl.Get());
    }
}

nvrhi::GraphicsPipelineHandle Renderer::GetOrCreateGraphicsPipeline(const nvrhi::GraphicsPipelineDesc& pipelineDesc, const nvrhi::FramebufferInfoEx& fbInfo)
{
    // Hash shaders and input-related properties
    size_t h = 1469598103934665603ull;
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.VS.Get());
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.PS.Get());
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.HS.Get());
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.DS.Get());
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.GS.Get());
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.inputLayout.Get());
    h = h * 1099511628211u + std::hash<int>()(static_cast<int>(pipelineDesc.primType));
    h = h * 1099511628211u + std::hash<uint32_t>()(pipelineDesc.patchControlPoints);
    h = h * 1099511628211u + std::hash<bool>()(pipelineDesc.useDrawIndex);
    
    // Hash common state: RenderState, FramebufferInfo, BindingLayouts
    HashPipelineCommonState(h, pipelineDesc.renderState, fbInfo, pipelineDesc.bindingLayouts);

    std::lock_guard<std::mutex> lock(m_CacheMutex);

    auto it = m_GraphicsPipelineCache.find(h);
    if (it != m_GraphicsPipelineCache.end())
        return it->second;

    SDL_Log("[Pipeline Cache] Creating new graphics pipeline (hash=%zu)", h);

    // Create pipeline and cache it
    nvrhi::GraphicsPipelineHandle pipeline = m_RHI->m_NvrhiDevice->createGraphicsPipeline(pipelineDesc, fbInfo);
    SDL_assert(pipeline && "Failed to create graphics pipeline");
    if (pipeline)
    {
        m_GraphicsPipelineCache.emplace(h, pipeline);
    }

    return pipeline;
}

nvrhi::MeshletPipelineHandle Renderer::GetOrCreateMeshletPipeline(const nvrhi::MeshletPipelineDesc& pipelineDesc, const nvrhi::FramebufferInfoEx& fbInfo)
{
    // Hash shaders and meshlet-specific properties
    size_t h = 1469598103934665603ull;
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.AS.Get());
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.MS.Get());
    h = h * 1099511628211u + std::hash<const void*>()(pipelineDesc.PS.Get());
    h = h * 1099511628211u + std::hash<int>()(static_cast<int>(pipelineDesc.primType));
    h = h * 1099511628211u + std::hash<bool>()(pipelineDesc.useDrawIndex);

    // Hash common state: RenderState, FramebufferInfo, BindingLayouts
    HashPipelineCommonState(h, pipelineDesc.renderState, fbInfo, pipelineDesc.bindingLayouts);

    std::lock_guard<std::mutex> lock(m_CacheMutex);

    auto it = m_MeshletPipelineCache.find(h);
    if (it != m_MeshletPipelineCache.end())
        return it->second;

    SDL_Log("[Pipeline Cache] Creating new meshlet pipeline (hash=%zu)", h);

    // Create pipeline and cache it
    nvrhi::MeshletPipelineHandle pipeline = m_RHI->m_NvrhiDevice->createMeshletPipeline(pipelineDesc, fbInfo);
    SDL_assert(pipeline && "Failed to create meshlet pipeline");
    if (pipeline)
    {
        m_MeshletPipelineCache.emplace(h, pipeline);
    }

    return pipeline;
}

nvrhi::ComputePipelineHandle Renderer::GetOrCreateComputePipeline(nvrhi::ShaderHandle shader, const nvrhi::BindingLayoutVector& bindingLayouts)
{
    // Hash relevant pipeline properties: CS shader handle, binding layout pointers
    size_t h = 1469598103934665603ull;
    h = h * 1099511628211u + std::hash<const void*>()(shader.Get());
    for (const nvrhi::BindingLayoutHandle& layout : bindingLayouts)
    {
        h = h * 1099511628211u + std::hash<const void*>()(layout.Get());
    }

    std::lock_guard<std::mutex> lock(m_CacheMutex);

    auto it = m_ComputePipelineCache.find(h);
    if (it != m_ComputePipelineCache.end())
        return it->second;

    SDL_Log("[Pipeline Cache] Creating new compute pipeline (hash=%zu)", h);

    // Create pipeline and cache it
    nvrhi::ComputePipelineDesc desc;
    desc.CS = shader;
    desc.bindingLayouts = bindingLayouts;
    nvrhi::ComputePipelineHandle pipeline = m_RHI->m_NvrhiDevice->createComputePipeline(desc);
    SDL_assert(pipeline && "Failed to create compute pipeline");
    if (pipeline)
    {
        m_ComputePipelineCache.emplace(h, pipeline);
    }

    return pipeline;
}

void Renderer::AddFullScreenPass(const RenderPassParams& params)
{
    PROFILE_FUNCTION();
    PROFILE_GPU_SCOPED(params.shaderName, params.commandList);

    nvrhi::MeshletPipelineDesc desc;
    desc.MS = GetShaderHandle("FullScreen_MSMain");
    desc.PS = GetShaderHandle(params.shaderName);

    std::vector<nvrhi::BindingSetHandle> bindingSets;

    const nvrhi::BindingLayoutHandle layout = GetOrCreateBindingLayoutFromBindingSetDesc(params.bindingSetDesc, params.registerSpace);
    const nvrhi::BindingSetHandle bindingSet = m_RHI->m_NvrhiDevice->createBindingSet(params.bindingSetDesc, layout);

    desc.bindingLayouts.push_back(layout);
    bindingSets.push_back(bindingSet);

    for (const RenderPassParams::BindingSetDescAndRegisterSpace& bsetAndSpace : params.additionalBindingSets)
    {
        const nvrhi::BindingLayoutHandle additionalLayout = GetOrCreateBindingLayoutFromBindingSetDesc(bsetAndSpace.bindingSetDesc, bsetAndSpace.registerSpace);
        const nvrhi::BindingSetHandle additionalBindingSet = m_RHI->m_NvrhiDevice->createBindingSet(bsetAndSpace.bindingSetDesc, additionalLayout);

        desc.bindingLayouts.push_back(additionalLayout);
        bindingSets.push_back(additionalBindingSet);
    }

    if (params.bIncludeBindlessResources)
    {
        desc.bindingLayouts.push_back(GetStaticTextureBindingLayout());
        bindingSets.push_back(GetStaticTextureDescriptorTable());

        desc.bindingLayouts.push_back(GetStaticSamplerBindingLayout());
        bindingSets.push_back(GetStaticSamplerDescriptorTable());
    }

    desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    desc.renderState.depthStencilState = params.depthStencilState ? *params.depthStencilState : CommonResources::GetInstance().DepthDisabled;

    nvrhi::MeshletPipelineHandle pipeline = GetOrCreateMeshletPipeline(desc, params.framebuffer->getFramebufferInfo());

    nvrhi::MeshletState state;
    if (params.depthStencilState)
    {
        state.dynamicStencilRefValue = params.depthStencilState->stencilRefValue;
    }
    state.pipeline = pipeline;
    for (const nvrhi::BindingSetHandle& bindingSet : bindingSets)
    {
        state.bindings.push_back(bindingSet.Get());
    }
    state.framebuffer = params.framebuffer;

    const nvrhi::FramebufferDesc& fbDesc = params.framebuffer->getDesc();
    nvrhi::TextureDesc texDesc = fbDesc.colorAttachments[0].texture->getDesc();
    uint32_t mipLevel = fbDesc.colorAttachments[0].subresources.baseMipLevel;
    uint32_t width = std::max(1u, texDesc.width >> mipLevel);
    uint32_t height = std::max(1u, texDesc.height >> mipLevel);

    state.viewport.viewports.push_back(nvrhi::Viewport(0, (float)width, 0, (float)height, 0, 1));
    state.viewport.scissorRects.push_back(nvrhi::Rect(0, (int)width, 0, (int)height));

    params.commandList->setMeshletState(state);
    if (params.pushConstants && params.pushConstantsSize > 0)
    {
        params.commandList->setPushConstants(params.pushConstants, params.pushConstantsSize);
    }
    params.commandList->dispatchMesh(1, 1, 1);
}

void Renderer::AddComputePass(const RenderPassParams& params)
{
    PROFILE_FUNCTION();
    PROFILE_GPU_SCOPED(params.shaderName, params.commandList);

    nvrhi::BindingLayoutVector layouts;
    std::vector<nvrhi::BindingSetHandle> bindingSets;

    const nvrhi::BindingLayoutHandle layout = GetOrCreateBindingLayoutFromBindingSetDesc(params.bindingSetDesc, params.registerSpace);
    const nvrhi::BindingSetHandle bindingSet = m_RHI->m_NvrhiDevice->createBindingSet(params.bindingSetDesc, layout);

    layouts.push_back(layout);
    bindingSets.push_back(bindingSet);

    for (const RenderPassParams::BindingSetDescAndRegisterSpace& bsetAndSpace : params.additionalBindingSets)
    {
        const nvrhi::BindingLayoutHandle additionalLayout = GetOrCreateBindingLayoutFromBindingSetDesc(bsetAndSpace.bindingSetDesc, bsetAndSpace.registerSpace);
        const nvrhi::BindingSetHandle additionalBindingSet = m_RHI->m_NvrhiDevice->createBindingSet(bsetAndSpace.bindingSetDesc, additionalLayout);

        layouts.push_back(additionalLayout);
        bindingSets.push_back(additionalBindingSet);
    }

    if (params.bIncludeBindlessResources)
    {
        layouts.push_back(GetStaticTextureBindingLayout());
        bindingSets.push_back(GetStaticTextureDescriptorTable());

        layouts.push_back(GetStaticSamplerBindingLayout());
        bindingSets.push_back(GetStaticSamplerDescriptorTable());
    }

    nvrhi::ComputeState state;
    state.pipeline = GetOrCreateComputePipeline(GetShaderHandle(params.shaderName), layouts);
    for (const nvrhi::BindingSetHandle& bindingSet : bindingSets)
    {
        state.bindings.push_back(bindingSet.Get());
    }

    if (params.dispatchParams.indirectBuffer)
    {
        state.indirectParams = params.dispatchParams.indirectBuffer;
    }

    params.commandList->setComputeState(state);

    if (params.pushConstants && params.pushConstantsSize > 0)
    {
        params.commandList->setPushConstants(params.pushConstants, params.pushConstantsSize);
    }

    if (!params.dispatchParams.indirectBuffer)
    {
        params.commandList->dispatch(params.dispatchParams.x, params.dispatchParams.y, params.dispatchParams.z);
    }
    else
    {
        params.commandList->dispatchIndirect(params.dispatchParams.indirectOffsetBytes);
    }
}

void Renderer::GenerateMipsUsingSPD(nvrhi::TextureHandle texture, nvrhi::BufferHandle spdAtomicCounter, nvrhi::CommandListHandle commandList, const char* markerName, uint32_t reductionType)
{
    PROFILE_FUNCTION();
    PROFILE_GPU_SCOPED(markerName, commandList);
    nvrhi::utils::ScopedMarker spdMarker{ commandList, markerName };

    const nvrhi::FormatInfo& formatInfo = nvrhi::getFormatInfo(texture->getDesc().format);
    const uint32_t numChannels = formatInfo.hasBlue ? 3 : 1;

    const uint32_t numMips = texture->getDesc().mipLevels;

    // We generate mips 1..N. SPD will be configured to take mip 0 as source.
    // So SPD "mips" count is numMips - 1. 
    // Note: SPD refers to how many downsample steps to take.
    const uint32_t spdmips = numMips - 1;

    FfxUInt32x2 dispatchThreadGroupCountXY;
    FfxUInt32x2 workGroupOffset;
    FfxUInt32x2 numWorkGroupsAndMips;
    FfxUInt32x4 rectInfo = { 0, 0, texture->getDesc().width, texture->getDesc().height };

    ffxSpdSetup(dispatchThreadGroupCountXY, workGroupOffset, numWorkGroupsAndMips, rectInfo, spdmips);

    srrhi::SpdInputs inputs;
    inputs.m_SpdConstants.SetMips(numWorkGroupsAndMips[1]);
    inputs.m_SpdConstants.SetNumWorkGroups(numWorkGroupsAndMips[0]);
    inputs.m_SpdConstants.SetWorkGroupOffset(Vector2U{ workGroupOffset[0], workGroupOffset[1] });
    inputs.m_SpdConstants.SetReductionType(reductionType);
    inputs.SetAtomicCounter(spdAtomicCounter);
    inputs.SetMip0(texture, 0, 1);
    inputs.SetOut1(numMips > 1 ? texture : CommonResources::GetInstance().DummyUAVTexture, numMips > 1 ? 1 : 0);
    inputs.SetOut2(numMips > 2 ? texture : CommonResources::GetInstance().DummyUAVTexture, numMips > 2 ? 2 : 0);
    inputs.SetOut3(numMips > 3 ? texture : CommonResources::GetInstance().DummyUAVTexture, numMips > 3 ? 3 : 0);
    inputs.SetOut4(numMips > 4 ? texture : CommonResources::GetInstance().DummyUAVTexture, numMips > 4 ? 4 : 0);
    inputs.SetOut5(numMips > 5 ? texture : CommonResources::GetInstance().DummyUAVTexture, numMips > 5 ? 5 : 0);
    inputs.SetOut6(numMips > 6 ? texture : CommonResources::GetInstance().DummyUAVTexture, numMips > 6 ? 6 : 0);
    inputs.SetOut7(numMips > 7 ? texture : CommonResources::GetInstance().DummyUAVTexture, numMips > 7 ? 7 : 0);
    inputs.SetOut8(numMips > 8 ? texture : CommonResources::GetInstance().DummyUAVTexture, numMips > 8 ? 8 : 0);
    inputs.SetOut9(numMips > 9 ? texture : CommonResources::GetInstance().DummyUAVTexture, numMips > 9 ? 9 : 0);
    inputs.SetOut10(numMips > 10 ? texture : CommonResources::GetInstance().DummyUAVTexture, numMips > 10 ? 10 : 0);
    inputs.SetOut11(numMips > 11 ? texture : CommonResources::GetInstance().DummyUAVTexture, numMips > 11 ? 11 : 0);
    inputs.SetOut12(numMips > 12 ? texture : CommonResources::GetInstance().DummyUAVTexture, numMips > 12 ? 12 : 0);

    // Clear atomic counter
    commandList->clearBufferUInt(spdAtomicCounter, 0);

    nvrhi::BindingSetDesc spdBset = CreateBindingSetDesc(inputs);

    char shaderName[256]{};
    std::snprintf(shaderName, sizeof(shaderName), "SPD_SPD_CSMain_SPD_NUM_CHANNELS=%u", numChannels);

    Renderer::RenderPassParams params{
        .commandList = commandList,
        .shaderName = shaderName,
        .bindingSetDesc = spdBset,
        .pushConstants = &inputs.m_SpdConstants,
        .pushConstantsSize = srrhi::SpdInputs::PushConstantBytes,
        .dispatchParams = { .x = dispatchThreadGroupCountXY[0], .y = dispatchThreadGroupCountXY[1], .z = 1 }
    };

    AddComputePass(params);
}

nvrhi::CommandListHandle Renderer::AcquireCommandList(bool bImmediatelyQueue)
{
    PROFILE_FUNCTION();
    SINGLE_THREAD_GUARD();

    nvrhi::CommandListHandle handle;

    if (!m_CommandListFreeList.empty())
    {
        handle = m_CommandListFreeList.back();
        m_CommandListFreeList.pop_back();
    }
    else
    {
        const nvrhi::CommandListParameters params{ .enableImmediateExecution = false, .queueType = nvrhi::CommandQueue::Graphics };
        handle = m_RHI->m_NvrhiDevice->createCommandList(params);
    }

    SDL_assert(handle && "Failed to acquire command list");

    if (bImmediatelyQueue)
    {
        m_PendingCommandLists.push_back(handle);
    }

    return handle;
}

void Renderer::ExecutePendingCommandLists()
{
    PROFILE_FUNCTION();
    SINGLE_THREAD_GUARD();

    // Wait for GPU to finish all work before presenting
    {
        PROFILE_SCOPED("WaitForIdle");
        m_RHI->m_NvrhiDevice->waitForIdle();
     }

    m_CommandListFreeList.insert(m_CommandListFreeList.end(), m_InFlightCommandLists.begin(), m_InFlightCommandLists.end());
    m_InFlightCommandLists.clear();

    if (!m_PendingCommandLists.empty())
    {
        // Submit GPU profiling blocks in the same order as command list execution.
        // MICROPROFILE_GPU_SUBMIT must be called in submission order so microprofile
        // can correlate GPU timestamps across frames correctly.
        for (const nvrhi::CommandListHandle& handle : m_PendingCommandLists)
        {
            if (handle->m_GPULog != ULLONG_MAX)
            {
                MicroProfileGpuSubmit((uint32_t)nvrhi::CommandQueue::Graphics, handle->m_GPULog);
                handle->m_GPULog = ULLONG_MAX;
            }
        }

        if (Config::Get().ExecutePerPass || Config::Get().ExecutePerPassAndWait)
        {
            for (const nvrhi::CommandListHandle& handle : m_PendingCommandLists)
            {
                m_RHI->m_NvrhiDevice->executeCommandList(handle);

                if (Config::Get().ExecutePerPassAndWait)
                {
                    m_RHI->m_NvrhiDevice->waitForIdle();
                }
            }
        }
        else
        {
            std::vector<nvrhi::ICommandList*> rawLists;
            rawLists.reserve(m_PendingCommandLists.size());
            for (const nvrhi::CommandListHandle& handle : m_PendingCommandLists)
            {
                rawLists.push_back(handle.Get());
            }

            m_RHI->m_NvrhiDevice->executeCommandLists(rawLists.data(), rawLists.size());
        }

        m_InFlightCommandLists.insert(m_InFlightCommandLists.end(), m_PendingCommandLists.begin(), m_PendingCommandLists.end());
        m_PendingCommandLists.clear();
    }
}

MicroProfileThreadLogGpu*& Renderer::GetGPULogForCurrentThread()
{
    thread_local MicroProfileThreadLogGpu* tl_GPULog = nullptr;
    return tl_GPULog;
}

nvrhi::BindingSetDesc Renderer::CreateBindingSetDesc(std::span<const srrhi::ResourceEntry> resources, uint32_t pushConstantBytes)
{
    auto SRRHIDimensionToNVRHIDimension = [](srrhi::TextureDimension dim)
    {
        switch (dim)
        {
        case srrhi::TextureDimension::None:
            SDL_assert(false && "Unexpected TextureDimension::None for a texture resource");
            return nvrhi::TextureDimension::Unknown;

        case srrhi::TextureDimension::Texture1D: return nvrhi::TextureDimension::Texture1D;
        case srrhi::TextureDimension::Texture1DArray: return nvrhi::TextureDimension::Texture1DArray;
        case srrhi::TextureDimension::Texture2D: return nvrhi::TextureDimension::Texture2D;
        case srrhi::TextureDimension::Texture2DArray: return nvrhi::TextureDimension::Texture2DArray;
        case srrhi::TextureDimension::Texture2DMS: return nvrhi::TextureDimension::Texture2DMS;
        case srrhi::TextureDimension::Texture2DMSArray: return nvrhi::TextureDimension::Texture2DMSArray;
        case srrhi::TextureDimension::Texture3D: return nvrhi::TextureDimension::Texture3D;
        }

        SDL_assert(false && "Unknown texture dimension");
        return nvrhi::TextureDimension::Unknown;
    };

    nvrhi::BindingSetDesc desc;

    for (const srrhi::ResourceEntry& entry : resources)
    {
        // Build texture subresource set from the mip/slice fields.
        // -1 values map to AllMipLevels / AllArraySlices.
        const nvrhi::TextureSubresourceSet subresources{
            static_cast<nvrhi::MipLevel>(entry.baseMipLevel),
            entry.numMipLevels < 0
                ? nvrhi::TextureSubresourceSet::AllMipLevels
                : static_cast<nvrhi::MipLevel>(entry.numMipLevels),
            static_cast<nvrhi::ArraySlice>(entry.baseArraySlice),
            entry.numArraySlices < 0
                ? nvrhi::TextureSubresourceSet::AllArraySlices
                : static_cast<nvrhi::ArraySlice>(entry.numArraySlices)
        };

        switch (entry.type)
        {
        case srrhi::ResourceType::Texture_SRV:
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(
                entry.slot,
                static_cast<nvrhi::ITexture*>(entry.pResource),
                nvrhi::Format::UNKNOWN,
                subresources,
                SRRHIDimensionToNVRHIDimension(entry.textureDimension)
            ));
            break;

        case srrhi::ResourceType::Texture_UAV:
            // UAV subresource set: numMipLevels is always 1 per NVRHI convention,
            // already guaranteed by the srrhi setter (SetFoo(ptr, baseMip) hardcodes 1).
            desc.addItem(nvrhi::BindingSetItem::Texture_UAV(
                entry.slot, 
                static_cast<nvrhi::ITexture*>(entry.pResource),
                nvrhi::Format::UNKNOWN,
                subresources,
                SRRHIDimensionToNVRHIDimension(entry.textureDimension)
            ));
            break;

        case srrhi::ResourceType::TypedBuffer_SRV:
            desc.addItem(nvrhi::BindingSetItem::TypedBuffer_SRV(entry.slot, static_cast<nvrhi::IBuffer*>(entry.pResource)));
            break;

        case srrhi::ResourceType::TypedBuffer_UAV:
            desc.addItem(nvrhi::BindingSetItem::TypedBuffer_UAV(entry.slot, static_cast<nvrhi::IBuffer*>(entry.pResource)));
            break;

        case srrhi::ResourceType::StructuredBuffer_SRV:
            desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(entry.slot, static_cast<nvrhi::IBuffer*>(entry.pResource)));
            break;

        case srrhi::ResourceType::StructuredBuffer_UAV:
            desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(entry.slot, static_cast<nvrhi::IBuffer*>(entry.pResource)));
            break;

        case srrhi::ResourceType::RawBuffer_SRV:
            desc.addItem(nvrhi::BindingSetItem::RawBuffer_SRV(entry.slot, static_cast<nvrhi::IBuffer*>(entry.pResource)));
            break;

        case srrhi::ResourceType::RawBuffer_UAV:
            desc.addItem(nvrhi::BindingSetItem::RawBuffer_UAV(entry.slot, static_cast<nvrhi::IBuffer*>(entry.pResource)));
            break;

        case srrhi::ResourceType::ConstantBuffer:
            desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(entry.slot, static_cast<nvrhi::IBuffer*>(entry.pResource)));
            break;

        case srrhi::ResourceType::Sampler:
            desc.addItem(nvrhi::BindingSetItem::Sampler(entry.slot, static_cast<nvrhi::ISampler*>(entry.pResource)));
            break;

        case srrhi::ResourceType::RayTracingAccelStruct:
            desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(entry.slot, static_cast<nvrhi::rt::IAccelStruct*>(entry.pResource)));
            break;

        case srrhi::ResourceType::PushConstants:
            SDL_assert(pushConstantBytes > 0);
            desc.addItem(nvrhi::BindingSetItem::PushConstants(entry.slot, pushConstantBytes));
            break;
        }
    }

    return desc;
}

std::string_view ScopedGpuProfile::BuildMicroProfileName(std::string_view name)
{
    constexpr size_t kMaxMicroProfileTimerNameLen = MICROPROFILE_NAME_MAX_LEN - 1;

    if (name.size() < kMaxMicroProfileTimerNameLen)
    {
        return name;
    }
    
    // Take the final MICROPROFILE_NAME_MAX_LEN - 1 characters
    return std::string_view{name.data() + (name.size() - kMaxMicroProfileTimerNameLen), kMaxMicroProfileTimerNameLen};
}

ScopedGpuProfile::ScopedGpuProfile(std::string_view name, const nvrhi::CommandListHandle& commandList)
    : m_Marker(commandList, name.data())
    , m_Token(MicroProfileGetToken("GPU", BuildMicroProfileName(name).data(), MP_AUTO, MicroProfileTokenTypeGpu, 0))
    , m_Scope(m_Token, Renderer::GetGPULogForCurrentThread())
{
}

ScopedCommandList::ScopedCommandList(const nvrhi::CommandListHandle& commandList, std::string_view markerName)
    : m_CommandList(commandList)
    , m_MarkerName(markerName)
    , m_HasMarker(!m_MarkerName.empty())
{
    m_CommandList->open();

    if (m_HasMarker)
    {
        Renderer::GetInstance()->m_RHI->SetCommandListDebugName(commandList, markerName);
    }

    if (m_HasMarker)
    {
        m_CommandList->beginMarker(m_MarkerName.c_str());
    }

    if (!Renderer::GetGPULogForCurrentThread())
    {
        Renderer::GetGPULogForCurrentThread() = MicroProfileThreadLogGpuAlloc();
    }
    MicroProfileGpuBegin(m_CommandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer, Renderer::GetGPULogForCurrentThread());
}

ScopedCommandList::~ScopedCommandList()
{
    SDL_assert(Renderer::GetGPULogForCurrentThread());
    m_CommandList->m_GPULog = MicroProfileGpuEnd(Renderer::GetGPULogForCurrentThread());

    if (m_HasMarker)
    {
        m_CommandList->endMarker();
    }
    m_CommandList->close();
}

int main(int argc, char* argv[])
{
    Renderer renderer{};
    Renderer::SetInstance(&renderer);
    Config::ParseCommandLine(argc, argv);

    renderer.Initialize();

    renderer.Run();
    renderer.Shutdown();
    Renderer::SetInstance(nullptr);
    return 0;
}