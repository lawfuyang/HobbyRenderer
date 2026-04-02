#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ============================================================
// Internal shader type enum matching nvrhi::ShaderType values
// ============================================================
enum class ShaderType : uint16_t
{
    None          = 0x0000,
    Vertex        = 0x0001,
    Hull          = 0x0002,
    Domain        = 0x0004,
    Geometry      = 0x0008,
    Pixel         = 0x0010,
    Compute       = 0x0020,
    Amplification = 0x0040,
    Mesh          = 0x0080,
};

static const char* ShaderTypeToNvrhiName(ShaderType t)
{
    switch (t)
    {
    case ShaderType::Vertex:        return "nvrhi::ShaderType::Vertex";
    case ShaderType::Pixel:         return "nvrhi::ShaderType::Pixel";
    case ShaderType::Geometry:      return "nvrhi::ShaderType::Geometry";
    case ShaderType::Compute:       return "nvrhi::ShaderType::Compute";
    case ShaderType::Hull:          return "nvrhi::ShaderType::Hull";
    case ShaderType::Domain:        return "nvrhi::ShaderType::Domain";
    case ShaderType::Amplification: return "nvrhi::ShaderType::Amplification";
    case ShaderType::Mesh:          return "nvrhi::ShaderType::Mesh";
    default:                        return "nvrhi::ShaderType::None";
    }
}

// ============================================================
// Shader metadata — mirrors Renderer.cpp's ShaderMetadata
// ============================================================
struct ShaderMetadata
{
    fs::path            sourcePath;
    std::string         entryPoint;
    std::string         suffix;
    std::vector<std::string> defines; // one sorted permutation
    ShaderType          shaderType = ShaderType::None;
};

// ============================================================
// ParseDefineValues — exact copy of Renderer.cpp logic (no SDL)
// ============================================================
static std::map<std::string, std::vector<std::string>> ParseDefineValues(const std::vector<std::string>& defineStrings)
{
    std::map<std::string, std::vector<std::string>> result;
    for (const std::string& defineStr : defineStrings)
    {
        const size_t eqPos = defineStr.find('=');
        if (eqPos == std::string::npos)
            continue;

        std::string key      = defineStr.substr(0, eqPos);
        std::string valueStr = defineStr.substr(eqPos + 1);
        std::vector<std::string>& values = result[key];

        if (!valueStr.empty() && valueStr[0] == '{' && valueStr.back() == '}')
        {
            valueStr = valueStr.substr(1, valueStr.length() - 2);
            size_t start = 0;
            while (start < valueStr.length())
            {
                const size_t comma = valueStr.find(',', start);
                const size_t end   = (comma == std::string::npos) ? valueStr.length() : comma;
                values.push_back(valueStr.substr(start, end - start));
                start = (comma == std::string::npos) ? valueStr.length() : comma + 1;
            }
        }
        else
        {
            values.push_back(valueStr);
        }
    }
    return result;
}

// ============================================================
// GenerateDefinePermutations — exact copy of Renderer.cpp logic (no SDL)
// ============================================================
static std::vector<std::vector<std::string>> GenerateDefinePermutations(
    const std::map<std::string, std::vector<std::string>>& defineValues)
{
    std::vector<std::vector<std::string>> permutations;
    if (defineValues.empty())
    {
        permutations.push_back({});
        return permutations;
    }

    std::vector<std::string> keys;
    for (const auto& [k, _] : defineValues)
        keys.push_back(k);
    std::sort(keys.begin(), keys.end());

    std::vector<size_t> indices(keys.size(), 0);
    while (true)
    {
        std::vector<std::string> permutation;
        for (size_t i = 0; i < keys.size(); ++i)
            permutation.push_back(keys[i] + "=" + defineValues.at(keys[i])[indices[i]]);
        std::sort(permutation.begin(), permutation.end());
        permutations.push_back(permutation);

        size_t pos = keys.size() - 1;
        while (true)
        {
            indices[pos]++;
            if (indices[pos] < defineValues.at(keys[pos]).size())
                break;
            indices[pos] = 0;
            if (pos == 0)
                return permutations;
            --pos;
        }
    }
}

// ============================================================
// ParseShaderConfig — adapted from Renderer.cpp, no SDL deps
// Unrecognised flags (e.g. -m 6_8, --embedPDB) are silently ignored.
// ============================================================
static bool ParseShaderConfig(const fs::path& configPath, std::vector<ShaderMetadata>& outMetadata)
{
    std::ifstream configFile(configPath);
    if (!configFile.is_open())
    {
        std::cerr << "[ShaderIDsGenerator] Failed to open config: " << configPath << '\n';
        return false;
    }

    std::string line;
    while (std::getline(configFile, line))
    {
        if (line.empty() || line[0] == '/' || line[0] == '#')
            continue;

        const size_t first = line.find_first_not_of(" \t\r\n");
        const size_t last  = line.find_last_not_of(" \t\r\n");
        if (first == std::string::npos)
            continue;
        line = line.substr(first, last - first + 1);

        std::istringstream iss(line);
        std::string token;
        ShaderMetadata baseMetadata;
        std::vector<std::string> rawDefines;

        iss >> token;
        baseMetadata.sourcePath = fs::path(token);

        while (iss >> token)
        {
            if (token == "-T" || token == "--profile")
            {
                iss >> token;
                if      (token.find("vs") != std::string::npos) baseMetadata.shaderType = ShaderType::Vertex;
                else if (token.find("ps") != std::string::npos) baseMetadata.shaderType = ShaderType::Pixel;
                else if (token.find("gs") != std::string::npos) baseMetadata.shaderType = ShaderType::Geometry;
                else if (token.find("cs") != std::string::npos) baseMetadata.shaderType = ShaderType::Compute;
                else if (token.find("hs") != std::string::npos) baseMetadata.shaderType = ShaderType::Hull;
                else if (token.find("ds") != std::string::npos) baseMetadata.shaderType = ShaderType::Domain;
                else if (token.find("as") != std::string::npos) baseMetadata.shaderType = ShaderType::Amplification;
                else if (token.find("ms") != std::string::npos) baseMetadata.shaderType = ShaderType::Mesh;
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
            else if (token == "-m" || token == "--shaderModel"
                  || token == "--embedPDB" || token == "--binaryBlob"
                  || token == "--hlsl2021" || token == "--allResourcesBound"
                  || token == "-X" || token == "--relaxedInclude"
                  || token == "--include" || token == "-p")
            {
                // Consume the next token if these flags take a value argument
                if (token == "-m" || token == "-X" || token == "--relaxedInclude"
                    || token == "--include" || token == "-p")
                    iss >> token; // discard value
                // flags without values are already consumed
            }
            // Any other unrecognised token is silently skipped
        }

        if (baseMetadata.entryPoint.empty())
            baseMetadata.entryPoint = "main";

        if (baseMetadata.shaderType == ShaderType::None)
        {
            std::cerr << "[ShaderIDsGenerator] Warning: could not determine shader type for line: " << line << '\n';
            return 1;
        }

        const auto defineValues  = ParseDefineValues(rawDefines);
        const auto permutations  = GenerateDefinePermutations(defineValues);
        for (const auto& perm : permutations)
        {
            ShaderMetadata m = baseMetadata;
            m.defines = perm;
            outMetadata.push_back(m);
        }
    }

    return true;
}

// ============================================================
// ComputeKey — MUST match the key formula in Renderer.cpp LoadShaders()
// ============================================================
static std::string ComputeKey(const ShaderMetadata& m)
{
    const fs::path parentDir = m.sourcePath.parent_path();
    std::string folderPrefix = (!parentDir.empty() && parentDir != ".")
        ? parentDir.generic_string() + "/" : "";
    std::string key = folderPrefix + m.sourcePath.stem().string() + "_" + m.entryPoint + m.suffix;
    for (const std::string& def : m.defines)
        key += "_" + def;
    return key;
}

// ============================================================
// ComputeOutputFile — mirrors GetShaderOutputPath() in Renderer.cpp
// Returns a path relative to exeDir (e.g. "shaders/dxil/Bloom_Prefilter_PSMain.dxil")
// ============================================================
static std::string ComputeOutputFile(const ShaderMetadata& m)
{
    std::string outName = m.sourcePath.stem().string();
    if (m.entryPoint != "main")
        outName += "_" + m.entryPoint;
    outName += m.suffix;

    const fs::path parentDir = m.sourcePath.parent_path();
    if (!parentDir.empty() && parentDir != ".")
        return "shaders/dxil/" + parentDir.generic_string() + "/" + outName + ".dxil";
    return "shaders/dxil/" + outName + ".dxil";
}

// ============================================================
// ComputeDefinesStr — comma-separated "KEY=VALUE" pairs
// ============================================================
static std::string ComputeDefinesStr(const std::vector<std::string>& defines)
{
    std::string result;
    for (size_t i = 0; i < defines.size(); ++i)
    {
        if (i > 0) result += ',';
        result += defines[i];
    }
    return result;
}

// ============================================================
// SanitizeIdentifier — convert a cache key to a valid C++ identifier
//   - Uppercase all chars
//   - Replace '/', '.', '=', '-' with '_'
//   - Collapse consecutive underscores to a single '_'
//   - Trim leading/trailing underscores
// ============================================================
static std::string SanitizeIdentifier(const std::string& key)
{
    std::string result;
    result.reserve(key.size());
    for (char c : key)
    {
        if (c == '/' || c == '.' || c == '=' || c == '-')
            result += '_';
        else
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    // Deduplicate consecutive underscores
    std::string deduped;
    deduped.reserve(result.size());
    bool bLastUnderscore = false;
    for (char c : result)
    {
        if (c == '_')
        {
            if (!bLastUnderscore)
                deduped += c;
            bLastUnderscore = true;
        }
        else
        {
            deduped += c;
            bLastUnderscore = false;
        }
    }

    // Trim leading/trailing underscores
    const size_t first = deduped.find_first_not_of('_');
    const size_t last  = deduped.find_last_not_of('_');
    if (first == std::string::npos)
        return "UNNAMED";
    return deduped.substr(first, last - first + 1);
}

// ============================================================
// EscapeString — escape backslashes and double-quotes for C string literals
// ============================================================
static std::string EscapeString(const std::string& s)
{
    std::string result;
    result.reserve(s.size());
    for (char c : s)
    {
        if (c == '\\') result += "\\\\";
        else if (c == '"') result += "\\\"";
        else result += c;
    }
    return result;
}

// ============================================================
// main
// ============================================================
int main(int argc, char** argv)
{
    std::vector<fs::path> inputConfigs;
    fs::path              outputDir;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "-i" || arg == "--input") && i + 1 < argc)
        {
            inputConfigs.emplace_back(argv[++i]);
        }
        else if ((arg == "-o" || arg == "--output") && i + 1 < argc)
        {
            outputDir = argv[++i];
        }
        else
        {
            std::cerr << "[ShaderIDsGenerator] Unknown argument: " << arg
                      << "\nUsage: ShaderIDsGenerator -i <cfg> [-i <cfg> ...] -o <output_dir>\n";
            return 1;
        }
    }

    if (inputConfigs.empty())
    {
        std::cerr << "[ShaderIDsGenerator] No input config files specified. Use -i <path>.\n";
        return 1;
    }
    if (outputDir.empty())
    {
        std::cerr << "[ShaderIDsGenerator] No output directory specified. Use -o <dir>.\n";
        return 1;
    }

    const fs::path outputFile = outputDir / "ShaderIDs.h";

    // ----------------------------------------------------------
    // Timestamp check: skip if output is newer than all inputs
    // (inputs = each .cfg file + the generator executable itself)
    // ----------------------------------------------------------
    if (fs::exists(outputFile))
    {
        const auto outputTime = fs::last_write_time(outputFile);
        bool bInputsNewer = false;

        // Check the generator executable itself (argv[0])
        fs::path executablePath(argv[0]);
        if (fs::last_write_time(executablePath) > outputTime)
            bInputsNewer = true;

        // Check each .cfg input file
        if (!bInputsNewer)
        {
            for (const fs::path& cfg : inputConfigs)
            {
                if (!fs::exists(cfg) || fs::last_write_time(cfg) > outputTime)
                {
                    bInputsNewer = true;
                    break;
                }
            }
        }

        if (!bInputsNewer)
        {
            std::cout << "[ShaderIDsGenerator] ShaderIDs.h is up to date, skipping.\n";
            return 0;
        }
    }

    // ----------------------------------------------------------
    // Parse all config files
    // ----------------------------------------------------------
    std::vector<ShaderMetadata> allMetadata;
    for (const fs::path& cfg : inputConfigs)
    {
        std::cout << "[ShaderIDsGenerator] Parsing: " << cfg << '\n';
        if (!ParseShaderConfig(cfg, allMetadata))
        {
            std::cerr << "[ShaderIDsGenerator] Failed to parse: " << cfg << '\n';
            return 1;
        }
    }

    // ----------------------------------------------------------
    // Build and deduplicate entries, keyed by cache key
    // std::map gives automatic alphabetical ordering by key.
    // ----------------------------------------------------------
    struct Entry
    {
        std::string key;
        std::string identifier;
        std::string outputFile;
        std::string entryPoint;
        std::string definesStr;
        ShaderType  type;
        uint32_t    id = 0;
    };

    std::map<std::string, Entry> entryMap;
    for (const ShaderMetadata& m : allMetadata)
    {
        std::string key = ComputeKey(m);
        if (entryMap.count(key))
        {
            std::cerr << "[ShaderIDsGenerator] Warning: duplicate shader entry for key: " << key << "\n"
                      << "  This can happen if multiple config files contain the same shader with the same permutation of defines.\n"
                      << "  Only one entry will be generated for this key. Fix this.\n";
            return 1;
        }

        Entry e;
        e.key        = key;
        e.identifier = SanitizeIdentifier(key);
        e.outputFile = ComputeOutputFile(m);
        e.entryPoint = m.entryPoint;
        e.definesStr = ComputeDefinesStr(m.defines);
        e.type       = m.shaderType;
        entryMap[std::move(key)] = std::move(e);
    }

    // Assign IDs in alphabetical order
    std::vector<Entry> entries;
    entries.reserve(entryMap.size());
    uint32_t nextId = 0;
    for (auto& [k, e] : entryMap)
    {
        e.id = nextId++;
        entries.push_back(e);
    }

    const uint32_t count = static_cast<uint32_t>(entries.size());

    // ----------------------------------------------------------
    // Write ShaderIDs.h
    // ----------------------------------------------------------
    std::error_code ec;
    fs::create_directories(outputDir, ec);

    std::ofstream out(outputFile);
    if (!out.is_open())
    {
        std::cerr << "[ShaderIDsGenerator] Failed to open output file: " << outputFile << '\n';
        return 1;
    }

    out <<
        "// GENERATED FILE — do not edit manually.\n"
        "// Re-generated by ShaderIDsGenerator whenever any input .cfg file(s) change(s).\n"
        "#pragma once\n"
        "\n"
        "#include <nvrhi/nvrhi.h>\n"
        "\n"
        "namespace ShaderID\n"
        "{\n"
        "    struct ShaderEntry\n"
        "    {\n"
        "        std::string_view  key;        // Cache key matching Renderer::GetShaderHandle\n"
        "        std::string_view  outputFile; // Relative to exe dir: \"shaders/dxil/<name>.dxil\"\n"
        "        std::string_view  entryPoint; // HLSL entry point name\n"
        "        std::string_view  definesStr; // Comma-separated \"KEY=VALUE\" pairs\n"
        "        nvrhi::ShaderType type;       // Shader stage\n"
        "    };\n"
        "\n"
        "    // ----------------------------------------------------------------\n"
        "    // Shader ID constants (sorted alphabetically by cache key)\n"
        "    // ----------------------------------------------------------------\n";

    for (const Entry& e : entries)
    {
        out << "    inline constexpr uint32_t " << e.identifier
            << " = " << e.id << "u;\n";
    }

    out << '\n'
        << "    inline constexpr uint32_t COUNT = " << count << "u;\n"
        << '\n'
        << "    // ----------------------------------------------------------------\n"
        << "    // Entry metadata — indexed by shader ID constant above\n"
        << "    // ----------------------------------------------------------------\n"
        << "    inline constexpr ShaderEntry ENTRIES[COUNT] =\n"
        << "    {\n";

    for (const Entry& e : entries)
    {
        out << "        { \""  << EscapeString(e.key)
            << "\", \""        << EscapeString(e.outputFile)
            << "\", \""        << EscapeString(e.entryPoint)
            << "\", \""        << EscapeString(e.definesStr)
            << "\", "          << ShaderTypeToNvrhiName(e.type)
            << " }, // "       << e.identifier << '\n';
    }

    out << "    };\n"
        << "} // namespace ShaderID\n";

    out.close();

    std::cout << "[ShaderIDsGenerator] Generated " << outputFile
              << " with " << count << " shader entries.\n";
    return 0;
}
