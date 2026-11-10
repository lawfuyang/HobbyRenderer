// Offline cluster LOD builder tests: the phase 0 gates of docs/cluster-lod-implementation-plan.md
// (V0.1 monotonicity, V0.2 watertightness, V0.3 no overlap, V0.4 determinism, V0.5 cache round-trip).
//
// The cache payload round-trip uses the same serialisation the cooked mesh file uses
// (ClusterLod::WriteToStream/ReadFromStream); the container (magic, version, config hash) is checked by
// running the renderer twice against a cooked Bistro cache with --cluster-stats.

#include "ClusterLod.h"

namespace
{

    int g_Failures = 0;
    int g_Checks = 0;

    void Check(bool bCondition, const std::string& what)
    {
        g_Checks++;
        if (!bCondition)
        {
            g_Failures++;
            SDL_Log("[ClusterLodTests] FAIL: %s", what.c_str());
        }
    }

    struct TestMesh
    {
        std::vector<float> m_Positions;
        std::vector<float> m_Normals;
        std::vector<float> m_Uvs;
        std::vector<uint32_t> m_Indices;

        uint32_t GetVertexCount() const { return uint32_t(m_Positions.size() / 3); }

        uint32_t AddVertex(float px, float py, float pz, float nx, float ny, float nz, float u, float v)
        {
            m_Positions.insert(m_Positions.end(), { px, py, pz });
            m_Normals.insert(m_Normals.end(), { nx, ny, nz });
            m_Uvs.insert(m_Uvs.end(), { u, v });
            return GetVertexCount() - 1;
        }
    };

    // Open surface with a border: an (cells x cells) grid with a ripple so decimation has real error.
    TestMesh MakeGrid(uint32_t cells, float size)
    {
        TestMesh mesh;
        for (uint32_t y = 0; y <= cells; ++y)
        {
            for (uint32_t x = 0; x <= cells; ++x)
            {
                const float u = float(x) / float(cells);
                const float v = float(y) / float(cells);
                const float px = (u - 0.5f) * size;
                const float py = (v - 0.5f) * size;
                const float pz = 0.05f * size * std::sin(u * 12.0f) * std::cos(v * 12.0f);
                mesh.AddVertex(px, py, pz, 0.0f, 0.0f, 1.0f, u, v);
            }
        }

        for (uint32_t y = 0; y < cells; ++y)
        {
            for (uint32_t x = 0; x < cells; ++x)
            {
                const uint32_t v0 = y * (cells + 1) + x;
                const uint32_t v1 = v0 + 1;
                const uint32_t v2 = v0 + (cells + 1);
                const uint32_t v3 = v2 + 1;
                mesh.m_Indices.insert(mesh.m_Indices.end(), { v0, v1, v3, v0, v3, v2 });
            }
        }
        return mesh;
    }

    // Closed manifold with no border at all.
    TestMesh MakeTorus(uint32_t major, uint32_t minor)
    {
        const float kMajorRadius = 1.0f;
        const float kMinorRadius = 0.35f;
        const float kPi = 3.14159265358979323846f;

        TestMesh mesh;
        for (uint32_t i = 0; i < major; ++i)
        {
            const float u = 2.0f * kPi * float(i) / float(major);
            for (uint32_t j = 0; j < minor; ++j)
            {
                const float v = 2.0f * kPi * float(j) / float(minor);
                const float cosV = std::cos(v);
                const float sinV = std::sin(v);
                const float px = (kMajorRadius + kMinorRadius * cosV) * std::cos(u);
                const float py = (kMajorRadius + kMinorRadius * cosV) * std::sin(u);
                const float pz = kMinorRadius * sinV;
                mesh.AddVertex(px, py, pz, cosV * std::cos(u), cosV * std::sin(u), sinV,
                    float(i) / float(major), float(j) / float(minor));
            }
        }

        for (uint32_t i = 0; i < major; ++i)
        {
            for (uint32_t j = 0; j < minor; ++j)
            {
                const uint32_t i1 = (i + 1) % major;
                const uint32_t j1 = (j + 1) % minor;
                const uint32_t v0 = i * minor + j;
                const uint32_t v1 = i * minor + j1;
                const uint32_t v2 = i1 * minor + j;
                const uint32_t v3 = i1 * minor + j1;
                mesh.m_Indices.insert(mesh.m_Indices.end(), { v0, v2, v3, v0, v3, v1 });
            }
        }
        return mesh;
    }

    // Smaller than one cluster: the DAG must still be valid and the cut must still tile the mesh.
    TestMesh MakeCube(float size)
    {
        const float h = size * 0.5f;
        // Per face: normal, then two in-plane tangents.
        const float faces[6][9] =
        {
            { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f },
            { -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f },
            { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f },
            { 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f },
        };
        const float corners[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };

        TestMesh mesh;
        for (uint32_t face = 0; face < 6; ++face)
        {
            const uint32_t base = mesh.GetVertexCount();
            for (uint32_t c = 0; c < 4; ++c)
            {
                const float a = corners[c][0];
                const float b = corners[c][1];
                const float px = faces[face][0] * h + faces[face][3] * a * h + faces[face][6] * b * h;
                const float py = faces[face][1] * h + faces[face][4] * a * h + faces[face][7] * b * h;
                const float pz = faces[face][2] * h + faces[face][5] * a * h + faces[face][8] * b * h;
                mesh.AddVertex(px, py, pz, faces[face][0], faces[face][1], faces[face][2],
                    a * 0.5f + 0.5f, b * 0.5f + 0.5f);
            }
            mesh.m_Indices.insert(mesh.m_Indices.end(),
                { base, base + 1, base + 2, base, base + 2, base + 3 });
        }
        return mesh;
    }

    bool SameVector(const auto& a, const auto& b)
    {
        return a.size() == b.size() &&
            (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(typename std::decay_t<decltype(a)>::value_type)) == 0);
    }

    bool SameClusters(const ClusterLod::PrimitiveClusters& a, const ClusterLod::PrimitiveClusters& b,
                      const std::string& prefix = std::string())
    {
        if (!SameVector(a.m_Nodes, b.m_Nodes))
        {
            SDL_Log("[ClusterLodTests] %snodes differ (%zu vs %zu)", prefix.c_str(), a.m_Nodes.size(), b.m_Nodes.size());
            for (size_t i = 0; i < std::min(a.m_Nodes.size(), b.m_Nodes.size()); ++i)
            {
                if (a.m_Nodes[i].m_Packed != b.m_Nodes[i].m_Packed)
                {
                    SDL_Log("[ClusterLodTests] %sfirst differing node %zu: %u vs %u", prefix.c_str(), i,
                        a.m_Nodes[i].m_Packed, b.m_Nodes[i].m_Packed);
                    break;
                }
            }
            return false;
        }
        if (!SameVector(a.m_NodeBounds, b.m_NodeBounds)) { SDL_Log("[ClusterLodTests] %snode bounds differ", prefix.c_str()); return false; }
        if (!SameVector(a.m_Groups, b.m_Groups)) { SDL_Log("[ClusterLodTests] %sgroups differ", prefix.c_str()); return false; }
        if (!SameVector(a.m_Clusters, b.m_Clusters)) { SDL_Log("[ClusterLodTests] %sclusters differ", prefix.c_str()); return false; }
        if (!SameVector(a.m_GeneratingGroups, b.m_GeneratingGroups)) { SDL_Log("[ClusterLodTests] %sgenerating groups differ", prefix.c_str()); return false; }
        if (!SameVector(a.m_Triangles, b.m_Triangles)) { SDL_Log("[ClusterLodTests] %striangles differ", prefix.c_str()); return false; }
        if (!SameVector(a.m_VertexIndices, b.m_VertexIndices)) { SDL_Log("[ClusterLodTests] %svertex indices differ", prefix.c_str()); return false; }
        if (a.m_LODLevelCount != b.m_LODLevelCount) { SDL_Log("[ClusterLodTests] %slevel count differs", prefix.c_str()); return false; }
        return true;
    }

    ClusterLod::BuildInput MakeInput(const TestMesh& mesh)
    {
        ClusterLod::BuildInput input;
        input.m_Positions = mesh.m_Positions.data();
        input.m_PositionStride = 3 * sizeof(float);
        input.m_Normals = mesh.m_Normals.data();
        input.m_NormalStride = 3 * sizeof(float);
        input.m_Uvs = mesh.m_Uvs.data();
        input.m_UvStride = 2 * sizeof(float);
        input.m_VertexCount = mesh.GetVertexCount();
        input.m_Indices = mesh.m_Indices.data();
        input.m_IndexCount = mesh.m_Indices.size();
        return input;
    }

    void RunMesh(const char* name, const TestMesh& mesh)
    {
        const std::string prefix = std::string(name) + ": ";
        const ClusterLod::BuildInput input = MakeInput(mesh);

        ClusterLod::PrimitiveClusters clusters;
        ClusterLod::BuildStats stats;
        Check(ClusterLod::Build(input, ClusterLod::GetActiveConfig(), clusters, stats), prefix + "build");
        if (clusters.IsEmpty())
            return;

        Check(stats.m_ClusterCount > 0 && stats.m_GroupCount > 0 && stats.m_LODLevelCount > 0,
            prefix + "build stats are filled");

        std::string error;
        const bool bDagValid = ClusterLod::ValidateDag(clusters, error);
        Check(bDagValid, prefix + "V0.1 " + error);

        // A threshold sweep from nearby (all levels too coarse, the finest level is drawn) to far away (only the
        // coarsest level is drawn). Every cut must tile the mesh and never cover a region twice.
        const float kDistances[] = { 0.01f, 0.5f, 4.0f, 1000.0f };
        for (float distance : kDistances)
        {
            ClusterLod::CutParams params;
            params.m_ViewPosition = Vector3{ distance, distance, distance };
            params.m_FocalPixels = 800.0f;

            std::string cutError;
            const bool bCutValid = ClusterLod::ValidateCutSweep(clusters, mesh.m_Positions.data(), 3 * sizeof(float), mesh.m_Indices.data(),
                mesh.m_Indices.size(), params, cutError);
            Check(bCutValid, prefix + "V0.2/V0.3 at distance " + std::to_string(distance) + ": " + cutError);
        }

        ClusterLod::PrimitiveClusters rebuilt;
        ClusterLod::BuildStats rebuiltStats;
        Check(ClusterLod::Build(input, ClusterLod::GetActiveConfig(), rebuilt, rebuiltStats), prefix + "rebuild");
        Check(SameClusters(clusters, rebuilt, prefix), prefix + "V0.4 deterministic rebuild");

        const std::filesystem::path path = std::filesystem::temp_directory_path() / "clusterlod_tests_roundtrip.bin";
        {
            std::ofstream os(path, std::ios::binary | std::ios::trunc);
            ClusterLod::WriteToStream(os, clusters);
            Check(os.good(), prefix + "V0.5 write");
        }

        ClusterLod::PrimitiveClusters loaded;
        {
            std::ifstream is(path, std::ios::binary);
            ClusterLod::ReadFromStream(is, loaded);
            Check(is.good(), prefix + "V0.5 read");
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);

        Check(SameClusters(clusters, loaded), prefix + "V0.5 round-trip");
        const bool bLoadedDagValid = ClusterLod::ValidateDag(loaded, error);
        Check(bLoadedDagValid, prefix + "V0.5 loaded DAG is valid: " + error);

        SDL_Log("[ClusterLodTests] %s: %u clusters, %u groups, %u levels", name, stats.m_ClusterCount,
            stats.m_GroupCount, stats.m_LODLevelCount);
    }

}

int main(int, char**)
{
    RunMesh("grid", MakeGrid(96, 4.0f));
    RunMesh("torus", MakeTorus(128, 64));
    RunMesh("cube", MakeCube(1.0f));

    // A changed clusterisation parameter must change the cache identity, or a stale DAG would be loaded.
    ClusterLod::Config changed = ClusterLod::GetActiveConfig();
    changed.m_MaxTriangles = 64;
    Check(ClusterLod::GetCacheConfigHash(changed) != ClusterLod::GetCacheConfigHash(ClusterLod::GetActiveConfig()),
        "V0.5 a config change alters the cache hash");

    // Degenerate input is rejected instead of producing an empty DAG.
    ClusterLod::PrimitiveClusters empty;
    ClusterLod::BuildStats emptyStats;
    ClusterLod::BuildInput noInput;
    Check(!ClusterLod::Build(noInput, ClusterLod::GetActiveConfig(), empty, emptyStats), "empty input is rejected");

    SDL_Log("[ClusterLodTests] %d checks, %d failures", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
