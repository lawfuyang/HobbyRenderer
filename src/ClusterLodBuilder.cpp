#include "ClusterLod.h"

#include "meshoptimizer.h"

namespace ClusterLod
{

namespace
{

    constexpr size_t kAttributeCount = 5; // normal xyz + uv

    Vector3 LoadPosition(const BuildInput& input, uint32_t vertex)
    {
        const float* p = reinterpret_cast<const float*>(
            reinterpret_cast<const char*>(input.m_Positions) + size_t(vertex) * input.m_PositionStride);
        return Vector3{ p[0], p[1], p[2] };
    }

    // A cluster while one level is being built, in primitive-local vertex indices.
    AABB ToBox(const Bounds& bounds)
    {
        AABB box;
        box.Center = Vector3{ (bounds.m_Min.x + bounds.m_Max.x) * 0.5f, (bounds.m_Min.y + bounds.m_Max.y) * 0.5f,
            (bounds.m_Min.z + bounds.m_Max.z) * 0.5f };
        box.Extents = Vector3{ (bounds.m_Max.x - bounds.m_Min.x) * 0.5f, (bounds.m_Max.y - bounds.m_Min.y) * 0.5f,
            (bounds.m_Max.z - bounds.m_Min.z) * 0.5f };
        return box;
    }

    void SetEdgeLengths(Bounds& bounds)
    {
        const float sizeX = bounds.m_Max.x - bounds.m_Min.x;
        const float sizeY = bounds.m_Max.y - bounds.m_Min.y;
        const float sizeZ = bounds.m_Max.z - bounds.m_Min.z;
        bounds.m_ShortestEdge = std::min(std::min(sizeX, sizeY), sizeZ);
        bounds.m_LongestEdge = std::max(std::max(sizeX, sizeY), sizeZ);
    }

    Bounds FromBox(const AABB& box)
    {
        Bounds bounds;
        bounds.m_Min = Vector3{ box.Center.x - box.Extents.x, box.Center.y - box.Extents.y, box.Center.z - box.Extents.z };
        bounds.m_Max = Vector3{ box.Center.x + box.Extents.x, box.Center.y + box.Extents.y, box.Center.z + box.Extents.z };
        SetEdgeLengths(bounds);
        return bounds;
    }

    Sphere ToSphere(const TraversalMetric& metric)
    {
        Sphere sphere;
        sphere.Center = Vector3{ metric.m_Sphere.x, metric.m_Sphere.y, metric.m_Sphere.z };
        sphere.Radius = metric.m_Sphere.w;
        return sphere;
    }

    // Grows `outer` so it also covers `inner`.
    void MergeSphere(TraversalMetric& outer, const TraversalMetric& inner)
    {
        Sphere merged;
        Sphere::CreateMerged(merged, ToSphere(outer), ToSphere(inner));

        outer.m_Sphere.x = merged.Center.x;
        outer.m_Sphere.y = merged.Center.y;
        outer.m_Sphere.z = merged.Center.z;
        outer.m_Sphere.w = merged.Radius;
    }


    struct WorkCluster
    {
        std::vector<uint32_t> m_Vertices;
        std::vector<uint8_t> m_Triangles;
        std::vector<uint32_t> m_Indices;      // expanded triangles, for partition and simplification
        uint32_t m_Group = kNoGroup;          // group index within the level being built
        uint32_t m_SourceGroup = kNoGroup;    // group of the level below that produced it
    };

    struct LevelGroup
    {
        uint32_t m_ClusterOffset = 0;
        uint32_t m_ClusterCount = 0;
        Bounds m_Bounds = {};
        std::vector<uint32_t> m_Simplified;
        float m_RawError = 0.0f;   // error of simplifying this group into the next level
        float m_Error = 0.0f;      // error of using this level's clusters instead of the original mesh
    };

    bool ClusterizeIndices(const BuildInput& input, const Config& config, const std::vector<uint32_t>& indices,
                           std::vector<WorkCluster>& outClusters)
    {
        outClusters.clear();
        if (indices.size() < 3)
            return false;

        const size_t maxVertices = config.m_MaxVertices;
        const size_t maxTriangles = config.m_MaxTriangles;
        const size_t minTriangles = maxTriangles / 2;

        const size_t maxMeshlets = meshopt_buildMeshletsBound(indices.size(), maxVertices, maxTriangles);
        std::vector<meshopt_Meshlet> meshlets(maxMeshlets);
        std::vector<uint32_t> meshletVertices(maxMeshlets * maxVertices);
        std::vector<uint8_t> meshletTriangles(maxMeshlets * maxTriangles * 3);

        const size_t meshletCount = meshopt_buildMeshletsFlex(meshlets.data(), meshletVertices.data(),
            meshletTriangles.data(), indices.data(), indices.size(),
            input.m_Positions, input.m_VertexCount, input.m_PositionStride,
            maxVertices, minTriangles, maxTriangles, 0.0f, 1.0f);

        if (meshletCount == 0)
            return false;

        outClusters.resize(meshletCount);
        for (size_t i = 0; i < meshletCount; ++i)
        {
            const meshopt_Meshlet& meshlet = meshlets[i];
            WorkCluster& cluster = outClusters[i];

            cluster.m_Vertices.assign(meshletVertices.begin() + meshlet.vertex_offset,
                meshletVertices.begin() + meshlet.vertex_offset + meshlet.vertex_count);
            cluster.m_Triangles.assign(meshletTriangles.begin() + meshlet.triangle_offset,
                meshletTriangles.begin() + meshlet.triangle_offset + meshlet.triangle_count * 3);

            cluster.m_Indices.resize(meshlet.triangle_count * 3);
            for (size_t t = 0; t < cluster.m_Triangles.size(); ++t)
                cluster.m_Indices[t] = cluster.m_Vertices[cluster.m_Triangles[t]];
        }
        return true;
    }

    // A compact copy of the primitive that the whole build works on. meshoptimizer's vertex remap drops every vertex
    // the index buffer never references and merges the ones whose attributes are all identical, and sharing that edge
    // is what gives the simplifier something to collapse along: a primitive may be a triangle soup in which no two
    // triangles share an index, and then level 0 is also the last level.
    struct Deduplicated
    {
        std::vector<float> m_Positions;   // xyz per vertex
        std::vector<float> m_Attributes;  // normal xyz + uv per vertex, empty when the primitive has neither
        std::vector<uint32_t> m_Indices;
        std::vector<uint32_t> m_Sources;  // deduplicated vertex -> primitive vertex it keeps the attributes of
    };

    void DeduplicateVertices(const BuildInput& input, Deduplicated& out)
    {
        constexpr size_t kPositionStride = sizeof(float) * 3;
        constexpr size_t kAttributeStride = sizeof(float) * kAttributeCount;

        const size_t vertexCount = input.m_VertexCount;
        std::vector<float> positions(vertexCount * 3);
        for (size_t v = 0; v < vertexCount; ++v)
        {
            const Vector3 position = LoadPosition(input, uint32_t(v));
            positions[v * 3 + 0] = position.x;
            positions[v * 3 + 1] = position.y;
            positions[v * 3 + 2] = position.z;
        }

        const bool bWithAttributes = input.m_Normals != nullptr && input.m_Uvs != nullptr;
        std::vector<float> attributes;
        if (bWithAttributes)
        {
            attributes.resize(vertexCount * kAttributeCount);
            for (size_t v = 0; v < vertexCount; ++v)
            {
                const float* normal = reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(input.m_Normals) + v * input.m_NormalStride);
                const float* uv = reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(input.m_Uvs) + v * input.m_UvStride);
                float* dst = &attributes[v * kAttributeCount];
                dst[0] = normal[0];
                dst[1] = normal[1];
                dst[2] = normal[2];
                dst[3] = uv[0];
                dst[4] = uv[1];
            }
        }

        std::vector<uint32_t> remap(vertexCount, 0);
        size_t uniqueCount = 0;
        if (bWithAttributes)
        {
            const meshopt_Stream streams[2] =
            {
                meshopt_Stream{ positions.data(), kPositionStride, kPositionStride },
                meshopt_Stream{ attributes.data(), kAttributeStride, kAttributeStride },
            };
            uniqueCount = meshopt_generateVertexRemapMulti(remap.data(), input.m_Indices, input.m_IndexCount,
                vertexCount, streams, 2);
        }
        else
        {
            uniqueCount = meshopt_generateVertexRemap(remap.data(), input.m_Indices, input.m_IndexCount,
                positions.data(), vertexCount, kPositionStride);
        }

        out.m_Positions.resize(uniqueCount * 3);
        meshopt_remapVertexBuffer(out.m_Positions.data(), positions.data(), vertexCount, kPositionStride, remap.data());

        out.m_Attributes.clear();
        if (bWithAttributes)
        {
            out.m_Attributes.resize(uniqueCount * kAttributeCount);
            meshopt_remapVertexBuffer(out.m_Attributes.data(), attributes.data(), vertexCount, kAttributeStride,
                remap.data());
        }

        out.m_Indices.assign(input.m_Indices, input.m_Indices + input.m_IndexCount);
        meshopt_remapIndexBuffer(out.m_Indices.data(), out.m_Indices.data(), out.m_Indices.size(), remap.data());

        // The emitted clusters index the primitive's own vertex block, so the survivor of each weld is recorded; the
        // remap is a function from primitive vertex to deduplicated vertex and this is its inverse. Vertices the
        // index buffer never references have no deduplicated vertex of their own.
        out.m_Sources.assign(uniqueCount, 0);
        for (size_t v = 0; v < vertexCount; ++v)
        {
            if (remap[v] == ~0u)
                continue;
            SDL_assert(remap[v] < uniqueCount);
            out.m_Sources[remap[v]] = uint32_t(v);
        }
    }

    // Interleaves the bits of a quantized point, so that points that are close in space are close in the order.
    uint64_t MortonKey(const Vector3& point, const Vector3& origin, const Vector3& scale)
    {
        const auto expand = [](uint32_t value)
        {
            uint64_t x = value & 0x3FFu;
            x = (x | (x << 16)) & 0x030000FFull;
            x = (x | (x << 8)) & 0x0300F00Full;
            x = (x | (x << 4)) & 0x030C30C3ull;
            x = (x | (x << 2)) & 0x09249249ull;
            return x;
        };

        const auto quantize = [](float value, float axisOrigin, float axisScale)
        {
            return uint32_t(std::clamp((value - axisOrigin) * axisScale, 0.0f, 1.0f) * 1023.0f);
        };

        return expand(quantize(point.x, origin.x, scale.x)) |
            (expand(quantize(point.y, origin.y, scale.y)) << 1) |
            (expand(quantize(point.z, origin.z, scale.z)) << 2);
    }

    // The first level has no finer groups to respect, so the partitioner places the clusters and they only have to
    // be ordered so that every group owns a contiguous run of at most kMaxClustersPerGroup clusters (a node
    // addresses at most 256 of them).
    uint32_t PartitionFirstLevel(const BuildInput& input, const Config& config, std::vector<WorkCluster>& clusters)
    {
        const uint32_t clusterCount = static_cast<uint32_t>(clusters.size());

        std::vector<uint32_t> partition(clusterCount, 0);
        if (clusterCount > 1)
        {
            std::vector<uint32_t> allIndices;
            std::vector<uint32_t> counts(clusterCount, 0);
            for (uint32_t i = 0; i < clusterCount; ++i)
            {
                counts[i] = static_cast<uint32_t>(clusters[i].m_Indices.size());
                allIndices.insert(allIndices.end(), clusters[i].m_Indices.begin(), clusters[i].m_Indices.end());
            }

            meshopt_partitionClusters(partition.data(), allIndices.data(), allIndices.size(), counts.data(), clusterCount,
                input.m_Positions, input.m_VertexCount, input.m_PositionStride,
                std::max<uint32_t>(1, config.m_GroupSize));
        }

        std::vector<uint32_t> order(clusterCount, 0);
        for (uint32_t i = 0; i < clusterCount; ++i)
            order[i] = i;
        std::stable_sort(order.begin(), order.end(),
            [&partition](uint32_t a, uint32_t b) { return partition[a] < partition[b]; });

        std::vector<WorkCluster> sorted;
        sorted.reserve(clusterCount);
        uint32_t groupCount = 0;
        uint32_t chunkClusters = kMaxClustersPerGroup;
        uint32_t previousPartition = UINT32_MAX;

        for (uint32_t i = 0; i < clusterCount; ++i)
        {
            const uint32_t index = order[i];
            if (partition[index] != previousPartition || chunkClusters == kMaxClustersPerGroup)
            {
                groupCount++;
                chunkClusters = 0;
            }

            sorted.push_back(std::move(clusters[index]));
            sorted.back().m_Group = groupCount - 1;
            chunkClusters++;
            previousPartition = partition[index];
        }

        clusters = std::move(sorted);
        return groupCount;
    }

    // Every level above the first is a set of units: one finer group together with all the clusters its decimation
    // became, which are contiguous because they were produced group by group. The cut drops a finer group as a whole
    // and only once *its* replacement is detailed enough, so a unit may never be split between two coarser groups -
    // that leaves uncovered bands that move with the camera. All this grouping decides is which neighbouring units
    // share a group, and since a group is the granularity of the cut, they are packed along a space-filling curve
    // until a group is full.
    uint32_t PackUnits(const BuildInput& input, const Config& config, std::vector<WorkCluster>& clusters)
    {
        struct Unit
        {
            uint32_t m_First = 0;
            uint32_t m_Count = 0;        // clusters
            uint32_t m_Vertices = 0;
            Vector3 m_Sum = {};          // sum of the unit's vertex positions
            uint64_t m_Key = 0;
        };

        std::vector<Unit> units;
        Vector3 boundsMin{ FLT_MAX, FLT_MAX, FLT_MAX };
        Vector3 boundsMax{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

        for (uint32_t i = 0; i < uint32_t(clusters.size()); ++i)
        {
            const WorkCluster& cluster = clusters[i];
            if (units.empty() || clusters[units.back().m_First].m_SourceGroup != cluster.m_SourceGroup)
            {
                Unit unit;
                unit.m_First = i;
                units.push_back(unit);
            }

            Unit& unit = units.back();
            unit.m_Count++;
            for (uint32_t v : cluster.m_Vertices)
            {
                const Vector3 position = LoadPosition(input, v);
                unit.m_Sum.x += position.x;
                unit.m_Sum.y += position.y;
                unit.m_Sum.z += position.z;
                unit.m_Vertices++;
                boundsMin = Vector3{ std::min(boundsMin.x, position.x), std::min(boundsMin.y, position.y),
                    std::min(boundsMin.z, position.z) };
                boundsMax = Vector3{ std::max(boundsMax.x, position.x), std::max(boundsMax.y, position.y),
                    std::max(boundsMax.z, position.z) };
            }
        }

        const Vector3 extent{ std::max(boundsMax.x - boundsMin.x, 1e-6f), std::max(boundsMax.y - boundsMin.y, 1e-6f),
            std::max(boundsMax.z - boundsMin.z, 1e-6f) };
        const Vector3 scale{ 1.0f / extent.x, 1.0f / extent.y, 1.0f / extent.z };

        for (Unit& unit : units)
        {
            SDL_assert(unit.m_Vertices != 0);
            const float inverse = 1.0f / float(unit.m_Vertices);
            const Vector3 center{ unit.m_Sum.x * inverse, unit.m_Sum.y * inverse, unit.m_Sum.z * inverse };
            unit.m_Key = MortonKey(center, boundsMin, scale);
        }

        std::stable_sort(units.begin(), units.end(), [](const Unit& a, const Unit& b) { return a.m_Key < b.m_Key; });

        std::vector<WorkCluster> sorted;
        sorted.reserve(clusters.size());
        uint32_t groupCount = 0;
        uint32_t chunkClusters = kMaxClustersPerGroup;
        const uint32_t groupSize = std::clamp(config.m_GroupSize, 1u, kMaxClustersPerGroup);

        for (const Unit& unit : units)
        {
            // A unit larger than a whole group is a group of its own: the cut stays correct either way, it only
            // becomes less selective.
            if (chunkClusters + unit.m_Count > groupSize)
            {
                groupCount++;
                chunkClusters = 0;
            }

            for (uint32_t c = 0; c < unit.m_Count; ++c)
            {
                sorted.push_back(std::move(clusters[unit.m_First + c]));
                sorted.back().m_Group = groupCount - 1;
            }
            chunkClusters += unit.m_Count;
        }

        clusters = std::move(sorted);
        return groupCount;
    }

    // Assigns each cluster a group, then orders the clusters so every group owns a contiguous run.
    uint32_t AssignGroups(const BuildInput& input, const Config& config, std::vector<WorkCluster>& clusters)
    {
        for (const WorkCluster& cluster : clusters)
        {
            if (cluster.m_SourceGroup != kNoGroup)
                return PackUnits(input, config, clusters);
        }

        return PartitionFirstLevel(input, config, clusters);
    }

    // Which vertices may not collapse. The simplifier's own border lock only sees the topology of the group's
    // index subset, and a group border can run straight through a UV seam, where the two sides are coincident
    // vertices that share no index: positions therefore decide what counts as a border vertex, and every vertex
    // whose position is used by another group of this level is locked. Locking more than strictly necessary is
    // harmless (it only costs simplification quality), locking less leaves a crack.
    void ComputeLocks(const BuildInput& input, const std::vector<WorkCluster>& clusters, size_t vertexCount,
                      std::vector<uint8_t>& outLocks)
    {
        outLocks.assign(vertexCount, 0);

        std::unordered_map<size_t, uint32_t> firstGroup;
        std::vector<size_t> vertexHashes(vertexCount, 0);
        std::unordered_set<size_t> sharedPositions;

        for (uint32_t v = 0; v < vertexCount; ++v)
            vertexHashes[v] = HashPosition(input.m_Positions, input.m_PositionStride, v);

        for (const WorkCluster& cluster : clusters)
        {
            for (uint32_t v : cluster.m_Vertices)
            {
                const size_t hash = vertexHashes[v];
                const auto inserted = firstGroup.emplace(hash, cluster.m_Group);
                if (!inserted.second && inserted.first->second != cluster.m_Group)
                    sharedPositions.insert(hash);
            }
        }

        for (uint32_t v = 0; v < vertexCount; ++v)
        {
            if (sharedPositions.count(vertexHashes[v]))
                outLocks[v] = meshopt_SimplifyVertex_Lock;
        }
    }

    // Simplifies one group's index list down to `target` indices. Permissive mode is what lets a group collapse at
    // all when its vertices are all attribute seams, which is what a triangle soup is made of. The clamped error
    // keeps the attribute error of those collapses at the scale of the position error, so a group does not read as
    // arbitrarily expensive because its normals disagree.
    void SimplifyGroup(const BuildInput& input, const Config& config, const std::vector<float>& attributes,
                       const std::vector<uint8_t>& locks, const std::vector<uint32_t>& groupIndices, size_t target,
                       std::vector<uint32_t>& outIndices, float& outError)
    {
        const size_t attributeCount = attributes.empty() ? 0 : kAttributeCount;
        const std::vector<float> weights{ config.m_NormalWeight, config.m_NormalWeight, config.m_NormalWeight,
            config.m_UvWeight, config.m_UvWeight };
        const uint32_t options = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute |
            meshopt_SimplifyErrorClamped | meshopt_SimplifyPermissive | meshopt_SimplifyLockBorder |
            meshopt_SimplifyRegularize;

        std::vector<uint32_t> simplified(groupIndices.size(), 0);
        float error = 0.0f;
        size_t count = meshopt_simplifyWithAttributes(simplified.data(), groupIndices.data(), groupIndices.size(),
            input.m_Positions, input.m_VertexCount, input.m_PositionStride,
            attributeCount ? attributes.data() : nullptr, attributeCount * sizeof(float),
            attributeCount ? weights.data() : nullptr, attributeCount,
            locks.data(), target, FLT_MAX, options, &error);

        // An empty result leaves the region with no coarser representation at all, so the first triangle survives.
        if (count < 3 && groupIndices.size() >= 3)
        {
            simplified[0] = groupIndices[0];
            simplified[1] = groupIndices[1];
            simplified[2] = groupIndices[2];
            count = 3;
        }

        outIndices.assign(simplified.begin(), simplified.begin() + count);
        outError = error;
    }

    void ComputeGroupBounds(const BuildInput& input, const std::vector<WorkCluster>& clusters, LevelGroup& group)
    {
        Vector3 boundsMin{ FLT_MAX, FLT_MAX, FLT_MAX };
        Vector3 boundsMax{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (uint32_t i = 0; i < group.m_ClusterCount; ++i)
        {
            for (uint32_t v : clusters[group.m_ClusterOffset + i].m_Vertices)
            {
                const Vector3 p = LoadPosition(input, v);
                boundsMin = Vector3{ std::min(boundsMin.x, p.x), std::min(boundsMin.y, p.y), std::min(boundsMin.z, p.z) };
                boundsMax = Vector3{ std::max(boundsMax.x, p.x), std::max(boundsMax.y, p.y), std::max(boundsMax.z, p.z) };
            }
        }

        group.m_Bounds.m_Min = boundsMin;
        group.m_Bounds.m_Max = boundsMax;
        SetEdgeLengths(group.m_Bounds);
    }

    TraversalMetric ComputeGroupMetric(const BuildInput& input, const std::vector<WorkCluster>& clusters,
                                       const LevelGroup& group, float error)
    {
        const Vector3 center{ (group.m_Bounds.m_Min.x + group.m_Bounds.m_Max.x) * 0.5f,
            (group.m_Bounds.m_Min.y + group.m_Bounds.m_Max.y) * 0.5f,
            (group.m_Bounds.m_Min.z + group.m_Bounds.m_Max.z) * 0.5f };
        float radius = 0.0f;
        for (uint32_t i = 0; i < group.m_ClusterCount; ++i)
        {
            for (uint32_t v : clusters[group.m_ClusterOffset + i].m_Vertices)
            {
                const Vector3 position = LoadPosition(input, v);
                const float dx = position.x - center.x;
                const float dy = position.y - center.y;
                const float dz = position.z - center.z;
                radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
        }

        TraversalMetric metric;
        metric.m_Sphere.x = center.x;
        metric.m_Sphere.y = center.y;
        metric.m_Sphere.z = center.z;
        metric.m_Sphere.w = radius;
        metric.m_MaxQuadricError = error;
        return metric;
    }

    struct TempNode
    {
        Bounds m_Bounds = {};
        int32_t m_Group = -1;
        std::vector<uint32_t> m_Children;
    };

    Bounds UnionBounds(const PrimitiveClusters& clusters, const std::vector<uint32_t>& groups, uint32_t begin, uint32_t end)
    {
        AABB merged = ToBox(clusters.m_Groups[groups[begin]].m_Bounds);
        for (uint32_t i = begin + 1; i < end; ++i)
            AABB::CreateMerged(merged, merged, ToBox(clusters.m_Groups[groups[i]].m_Bounds));
        return FromBox(merged);
    }

    // Splits a range of groups spatially until each leaf is a single group.
    uint32_t BuildTempTree(const PrimitiveClusters& clusters, std::vector<uint32_t>& groupOrder,
                           std::vector<TempNode>& tempNodes, uint32_t begin, uint32_t end)
    {
        TempNode node;
        node.m_Bounds = UnionBounds(clusters, groupOrder, begin, end);

        if (end - begin == 1)
        {
            node.m_Group = int32_t(groupOrder[begin]);
            tempNodes.push_back(std::move(node));
            return uint32_t(tempNodes.size() - 1);
        }

        const float extentX = node.m_Bounds.m_Max.x - node.m_Bounds.m_Min.x;
        const float extentY = node.m_Bounds.m_Max.y - node.m_Bounds.m_Min.y;
        const float extentZ = node.m_Bounds.m_Max.z - node.m_Bounds.m_Min.z;
        int axis = 0;
        if (extentY > extentX && extentY >= extentZ)
            axis = 1;
        else if (extentZ > extentX && extentZ > extentY)
            axis = 2;

        std::stable_sort(groupOrder.begin() + begin, groupOrder.begin() + end,
            [&](uint32_t a, uint32_t b)
            {
                const Group& ga = clusters.m_Groups[a];
                const Group& gb = clusters.m_Groups[b];
                const float ca = axis == 0 ? ga.m_Metric.m_Sphere.x : axis == 1 ? ga.m_Metric.m_Sphere.y : ga.m_Metric.m_Sphere.z;
                const float cb = axis == 0 ? gb.m_Metric.m_Sphere.x : axis == 1 ? gb.m_Metric.m_Sphere.y : gb.m_Metric.m_Sphere.z;
                return ca < cb;
            });

        const uint32_t count = end - begin;
        const uint32_t childCount = std::min(std::max((count + 7) / 8, 2u), std::min(kMaxGroupChildren, count));

        const uint32_t nodeIndex = uint32_t(tempNodes.size());
        tempNodes.push_back(std::move(node));

        for (uint32_t c = 0; c < childCount; ++c)
        {
            const uint32_t childBegin = begin + count * c / childCount;
            const uint32_t childEnd = begin + count * (c + 1) / childCount;
            // The child index has to be materialised before touching tempNodes again: the recursion appends
            // nodes, which reallocates the vector this node lives in.
            const uint32_t childIndex = BuildTempTree(clusters, groupOrder, tempNodes, childBegin, childEnd);
            tempNodes[nodeIndex].m_Children.push_back(childIndex);
        }
        return nodeIndex;
    }

    // Lays the temporary tree out so every node's children occupy a contiguous index range.
    void FlattenTree(const std::vector<TempNode>& tempNodes, uint32_t tempRoot, PrimitiveClusters& outClusters)
    {
        std::vector<uint32_t> finalIndex(tempNodes.size(), 0xFFFFFFFFu);
        std::vector<uint32_t> queue;

        finalIndex[tempRoot] = 0;
        outClusters.m_Nodes.push_back(Node{});
        outClusters.m_NodeBounds.push_back(tempNodes[tempRoot].m_Bounds);
        queue.push_back(tempRoot);

        for (size_t head = 0; head < queue.size(); ++head)
        {
            const uint32_t temp = queue[head];
            const TempNode& node = tempNodes[temp];

            if (node.m_Group >= 0)
            {
                outClusters.m_Nodes[finalIndex[temp]] =
                    MakeLeafNode(uint32_t(node.m_Group), outClusters.m_Groups[node.m_Group].m_ClusterCount);
                continue;
            }

            const uint32_t childOffset = uint32_t(outClusters.m_Nodes.size());
            outClusters.m_Nodes[finalIndex[temp]] = MakeChildNode(childOffset, uint32_t(node.m_Children.size()));

            for (uint32_t child : node.m_Children)
            {
                finalIndex[child] = uint32_t(outClusters.m_Nodes.size());
                outClusters.m_Nodes.push_back(Node{});
                outClusters.m_NodeBounds.push_back(tempNodes[child].m_Bounds);
                queue.push_back(child);
            }
        }
    }

    // Per level: the largest error present, and the metric of the group with the smallest error (a conservative
    // sphere for "this level is detailed enough to be the coarsest drawn one").
    void FillLevelMetrics(PrimitiveClusters& clusters)
    {
        for (uint32_t level = 0; level < clusters.m_LODLevelCount; ++level)
        {
            float worstError = 0.0f;
            float bestError = FLT_MAX;
            TraversalMetric bestMetric;

            for (const Group& group : clusters.m_Groups)
            {
                if (group.m_LODLevel != level)
                    continue;
                worstError = std::max(worstError, group.m_Metric.m_MaxQuadricError);
                if (group.m_Metric.m_MaxQuadricError < bestError)
                {
                    bestError = group.m_Metric.m_MaxQuadricError;
                    bestMetric = group.m_Metric;
                }
            }

            clusters.m_LODMinMaxError[level] = worstError;
            clusters.m_LODMinSphere[level] = bestMetric;
        }
    }

    void BuildNodeTrees(PrimitiveClusters& clusters)
    {
        std::vector<TempNode> tempNodes;
        std::vector<uint32_t> levelRoots;

        for (uint32_t level = 0; level < clusters.m_LODLevelCount; ++level)
        {
            std::vector<uint32_t> groupOrder;
            for (uint32_t g = 0; g < uint32_t(clusters.m_Groups.size()); ++g)
            {
                if (clusters.m_Groups[g].m_LODLevel == level)
                    groupOrder.push_back(g);
            }
            if (groupOrder.empty())
                continue;

            levelRoots.push_back(BuildTempTree(clusters, groupOrder, tempNodes, 0, uint32_t(groupOrder.size())));
        }

        // Root node: one child per level, so traversal can seed from whatever level the instance needs.
        TempNode root;
        if (!levelRoots.empty())
        {
            root.m_Bounds = tempNodes[levelRoots[0]].m_Bounds;
            for (uint32_t i = 1; i < uint32_t(levelRoots.size()); ++i)
            {
                const Bounds& other = tempNodes[levelRoots[i]].m_Bounds;
                root.m_Bounds.m_Min = Vector3{ std::min(root.m_Bounds.m_Min.x, other.m_Min.x),
                    std::min(root.m_Bounds.m_Min.y, other.m_Min.y), std::min(root.m_Bounds.m_Min.z, other.m_Min.z) };
                root.m_Bounds.m_Max = Vector3{ std::max(root.m_Bounds.m_Max.x, other.m_Max.x),
                    std::max(root.m_Bounds.m_Max.y, other.m_Max.y), std::max(root.m_Bounds.m_Max.z, other.m_Max.z) };
            }
        }
        root.m_Children = levelRoots;
        tempNodes.push_back(std::move(root));

        FlattenTree(tempNodes, uint32_t(tempNodes.size() - 1), clusters);
    }

    void InflateGroupSpheres(PrimitiveClusters& clusters)
    {
        // A coarser group must never be nearer the camera than the groups it replaced, otherwise two levels of
        // the same region can both pass the metric test and the cut overlaps. Level order matters: a level's own
        // inflation is what the next level inherits.
        for (uint32_t level = 0; level + 1 < clusters.m_LODLevelCount; ++level)
        {
            for (const Group& group : clusters.m_Groups)
            {
                if (group.m_LODLevel != level)
                    continue;
                for (uint32_t i = 0; i < group.m_ClusterCount; ++i)
                {
                    const uint32_t replacement = clusters.m_GeneratingGroups[group.m_ClusterOffset + i];
                    if (replacement != kNoGroup)
                        MergeSphere(clusters.m_Groups[replacement].m_Metric, group.m_Metric);
                }
            }
        }
    }

}


bool Build(const BuildInput& input, const Config& config, PrimitiveClusters& outClusters, BuildStats& outStats)
{
    const auto startTime = std::chrono::steady_clock::now();

    outClusters.Clear();
    outStats = BuildStats{};

    if (input.m_Indices == nullptr || input.m_IndexCount < 3 || input.m_VertexCount == 0 || input.m_Positions == nullptr)
    {
        SDL_Log("[ClusterLod] Build: empty input");
        return false;
    }

    Deduplicated dedup;
    DeduplicateVertices(input, dedup);

    // Everything below works on the deduplicated vertex buffer, in its ids; the emitted clusters refer to it.
    BuildInput work = input;
    work.m_Positions = dedup.m_Positions.data();
    work.m_PositionStride = sizeof(float) * 3;
    work.m_Normals = nullptr;
    work.m_Uvs = nullptr;
    work.m_VertexCount = dedup.m_Positions.size() / 3;
    work.m_Indices = dedup.m_Indices.data();
    work.m_IndexCount = dedup.m_Indices.size();

    std::vector<WorkCluster> levelClusters;
    if (!ClusterizeIndices(work, config, dedup.m_Indices, levelClusters))
    {
        SDL_Log("[ClusterLod] Build: clusterisation produced no clusters");
        return false;
    }

    std::vector<uint32_t> levelClusterOffset(kMaxLODLevels, 0);
    std::vector<uint32_t> levelClusterCount(kMaxLODLevels, 0);
    std::vector<uint32_t> levelGroupOffset(kMaxLODLevels, 0);
    std::vector<uint32_t> levelGroupCount(kMaxLODLevels, 0);
    std::vector<uint8_t> locks;
    std::vector<uint32_t> groupIndices;
    std::vector<float> rawErrorPerGroup;   // parallel to outClusters.m_Groups

    uint64_t triangleCount = dedup.m_Indices.size() / 3;
    uint32_t levelCount = 0;

    for (uint32_t level = 0; level < config.m_MaxLODLevels; ++level)
    {
        const uint32_t groupCount = AssignGroups(work, config, levelClusters);
        const uint32_t clusterCount = uint32_t(levelClusters.size());
        if (groupCount == 0 || clusterCount == 0)
        {
            SDL_Log("[ClusterLod] Build: level %u has no clusters", level);
            return false;
        }

        std::vector<LevelGroup> groups(groupCount);
        for (uint32_t i = 0; i < clusterCount; ++i)
        {
            LevelGroup& group = groups[levelClusters[i].m_Group];
            if (group.m_ClusterCount == 0)
                group.m_ClusterOffset = i;
            group.m_ClusterCount++;
        }

        ComputeLocks(work, levelClusters, uint32_t(work.m_VertexCount), locks);

        levelClusterOffset[level] = uint32_t(outClusters.m_Clusters.size());
        levelClusterCount[level] = clusterCount;
        levelGroupOffset[level] = uint32_t(outClusters.m_Groups.size());
        levelGroupCount[level] = groupCount;

        for (uint32_t gi = 0; gi < groupCount; ++gi)
        {
            LevelGroup& group = groups[gi];
            ComputeGroupBounds(work, levelClusters, group);

            Group emitted;
            emitted.m_ClusterOffset = uint32_t(outClusters.m_Clusters.size());
            emitted.m_ClusterCount = group.m_ClusterCount;
            emitted.m_LODLevel = level;
            emitted.m_Bounds = group.m_Bounds;

            for (uint32_t i = 0; i < group.m_ClusterCount; ++i)
            {
                const WorkCluster& cluster = levelClusters[group.m_ClusterOffset + i];

                Cluster emittedCluster{};
                emittedCluster.m_TriangleCountMinusOne = uint32_t(cluster.m_Triangles.size() / 3) - 1;
                emittedCluster.m_VertexCountMinusOne = uint32_t(cluster.m_Vertices.size()) - 1;
                emittedCluster.m_LODLevel = level;
                emittedCluster.m_GroupChildIndex = i;
                emittedCluster.m_VertexOffset = uint32_t(outClusters.m_VertexIndices.size());
                emittedCluster.m_TriangleOffset = uint32_t(outClusters.m_Triangles.size());

                for (uint32_t v : cluster.m_Vertices)
                    outClusters.m_VertexIndices.push_back(dedup.m_Sources[v]);
                outClusters.m_Triangles.insert(outClusters.m_Triangles.end(),
                    cluster.m_Triangles.begin(), cluster.m_Triangles.end());

                outClusters.m_Clusters.push_back(emittedCluster);
                outClusters.m_GeneratingGroups.push_back(kNoGroup);
            }

            outClusters.m_Groups.push_back(emitted);
        }

        // Decimate every group to build the next level, and record how much error that costs.
        uint32_t stuckGroups = 0;
        for (uint32_t gi = 0; gi < groupCount; ++gi)
        {
            LevelGroup& group = groups[gi];

            groupIndices.clear();
            for (uint32_t i = 0; i < group.m_ClusterCount; ++i)
            {
                const WorkCluster& cluster = levelClusters[group.m_ClusterOffset + i];
                groupIndices.insert(groupIndices.end(), cluster.m_Indices.begin(), cluster.m_Indices.end());
            }

            const size_t target = std::max<size_t>(
                size_t(double(groupIndices.size()) * double(config.m_DecimationFactor)) / 3 * 3, 3);

            float rawError = 0.0f;
            SimplifyGroup(work, config, dedup.m_Attributes, locks, groupIndices, target, group.m_Simplified, rawError);

            // A group that could not be brought down to the threshold is stuck: every coarser level would have to
            // build on it, and each one costs error while buying almost no triangles, so it stays terminal and its
            // own clusters are what the cut draws for that region at any distance.
            if (group.m_Simplified.size() > size_t(double(groupIndices.size()) * double(config.m_SimplifyThreshold)))
            {
                group.m_Simplified.clear();
                stuckGroups++;
            }

            group.m_RawError = rawError;
            rawErrorPerGroup.push_back(rawError);
        }

        SDL_Log("[ClusterLod] level %u: %u of %u groups are stuck (kept above the simplify threshold)", level, stuckGroups,
            groupCount);

        // The metric of a level-L group is how much error level L carries against the original mesh: the merged
        // error of the level below, or the raw cost of the decimation that produced these clusters.
        for (uint32_t gi = 0; gi < groupCount; ++gi)
        {
            LevelGroup& group = groups[gi];

            float metricError = 0.0f;
            if (level > 0)
            {
                for (uint32_t i = 0; i < group.m_ClusterCount; ++i)
                {
                    const WorkCluster& cluster = levelClusters[group.m_ClusterOffset + i];
                    if (cluster.m_SourceGroup == kNoGroup)
                        continue;

                    const Group& source = outClusters.m_Groups[cluster.m_SourceGroup];
                    metricError = std::max(metricError, source.m_Metric.m_MaxQuadricError * config.m_ErrorMergePrev);
                    metricError = std::max(metricError, rawErrorPerGroup[cluster.m_SourceGroup]);
                }
            }

            group.m_Error = metricError;
            outClusters.m_Groups[levelGroupOffset[level] + gi].m_Metric =
                ComputeGroupMetric(work, levelClusters, group, metricError);
        }

        // Link the level below to the groups that replaced it. AssignGroups keeps a finer group's whole decimated
        // output in one coarser group, so this is a function: every cluster of a finer group points at the same
        // replacement, which is what lets the cut rule drop a finer group without leaving anything uncovered.
        if (level > 0)
        {
            const uint32_t previousGroupCount = levelGroupCount[level - 1];
            std::vector<float> bestError(previousGroupCount, FLT_MAX);
            std::vector<uint32_t> bestGroup(previousGroupCount, kNoGroup);
            std::vector<std::vector<uint32_t>> receivers(previousGroupCount);

            for (uint32_t i = 0; i < clusterCount; ++i)
            {
                const uint32_t source = levelClusters[i].m_SourceGroup;
                if (source == kNoGroup)
                    continue;

                const uint32_t localSource = source - levelGroupOffset[level - 1];
                const uint32_t groupGlobal = levelGroupOffset[level] + levelClusters[i].m_Group;
                const float error = outClusters.m_Groups[groupGlobal].m_Metric.m_MaxQuadricError;

                std::vector<uint32_t>& seen = receivers[localSource];
                if (std::find(seen.begin(), seen.end(), groupGlobal) == seen.end())
                    seen.push_back(groupGlobal);

                if (error < bestError[localSource])
                {
                    bestError[localSource] = error;
                    bestGroup[localSource] = groupGlobal;
                }
            }

            for (uint32_t localSource = 0; localSource < previousGroupCount; ++localSource)
            {
                // A stuck group produced no coarser clusters, so nothing replaced it: its clusters keep
                // kNoGroup and the cut draws them at any distance.
                if (bestGroup[localSource] == kNoGroup)
                    continue;

                SDL_assert(receivers[localSource].size() == 1);

                const Group& group = outClusters.m_Groups[levelGroupOffset[level - 1] + localSource];
                for (uint32_t c = 0; c < group.m_ClusterCount; ++c)
                    outClusters.m_GeneratingGroups[group.m_ClusterOffset + c] = bestGroup[localSource];
            }
        }

        // Build the next level from the decimated groups, one clusterisation per group so every new cluster knows
        // the group that produced it.
        std::vector<WorkCluster> nextClusters;
        uint64_t nextTriangleCount = 0;
        for (uint32_t gi = 0; gi < groupCount; ++gi)
        {
            if (groups[gi].m_Simplified.size() < 3)
                continue;

            std::vector<WorkCluster> produced;
            if (!ClusterizeIndices(work, config, groups[gi].m_Simplified, produced))
                continue;

            const uint32_t sourceGlobal = levelGroupOffset[level] + gi;
            for (WorkCluster& cluster : produced)
            {
                cluster.m_SourceGroup = sourceGlobal;
                nextTriangleCount += cluster.m_Triangles.size() / 3;
            }
            for (WorkCluster& cluster : produced)
                nextClusters.push_back(std::move(cluster));
        }

        uint64_t levelTriangleCount = 0;
        for (uint32_t i = levelClusterOffset[level]; i < levelClusterOffset[level] + clusterCount; ++i)
            levelTriangleCount += outClusters.m_Clusters[i].GetTriangleCount();

        float worstError = 0.0f;
        for (uint32_t gi = 0; gi < groupCount; ++gi)
            worstError = std::max(worstError, outClusters.m_Groups[levelGroupOffset[level] + gi].m_Metric.m_MaxQuadricError);

        outStats.m_TriangleCountPerLevel[level] = levelTriangleCount;
        outStats.m_WorstErrorPerLevel[level] = worstError;
        outStats.m_ClusterCount += clusterCount;
        outStats.m_GroupCount += groupCount;
        levelCount = level + 1;
        outClusters.m_LODLevelCount = levelCount;

        if (nextClusters.empty() || nextTriangleCount >= triangleCount || nextClusters.size() >= clusterCount)
        {
            // A chain that stops at level 0 leaves the cut nothing to choose and a forced LOD nothing to clamp to,
            // so say which budget ran out.
            SDL_Log("[ClusterLod] chain ends after level %u: next level would be %zu clusters / %llu triangles,"
                    " this level has %u / %llu",
                level, nextClusters.size(), static_cast<unsigned long long>(nextTriangleCount), clusterCount,
                static_cast<unsigned long long>(triangleCount));
            break;
        }

        triangleCount = nextTriangleCount;
        levelClusters = std::move(nextClusters);
    }

    if (levelCount == 0)
        return false;

    InflateGroupSpheres(outClusters);
    FillLevelMetrics(outClusters);
    BuildNodeTrees(outClusters);

    outStats.m_LODLevelCount = levelCount;
    const auto endTime = std::chrono::steady_clock::now();
    outStats.m_BuildMilliseconds = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    return true;
}

uint32_t PrimitiveClusters::GetClusterTriangleCount(uint32_t clusterIndex) const
{
    SDL_assert(clusterIndex < m_Clusters.size());
    return m_Clusters[clusterIndex].GetTriangleCount();
}

uint32_t PrimitiveClusters::GetClusterVertexCount(uint32_t clusterIndex) const
{
    SDL_assert(clusterIndex < m_Clusters.size());
    return m_Clusters[clusterIndex].GetVertexCount();
}

uint32_t PrimitiveClusters::GetLevelClusterCount(uint32_t level) const
{
    uint32_t count = 0;
    for (const Cluster& cluster : m_Clusters)
        count += cluster.m_LODLevel == level ? 1u : 0u;
    return count;
}

uint32_t PrimitiveClusters::GetLevelGroupCount(uint32_t level) const
{
    uint32_t count = 0;
    for (const Group& group : m_Groups)
        count += group.m_LODLevel == level ? 1u : 0u;
    return count;
}

uint64_t PrimitiveClusters::GetMemoryBytes() const
{
    return uint64_t(m_Nodes.size()) * sizeof(Node) + uint64_t(m_NodeBounds.size()) * sizeof(Bounds) +
        uint64_t(m_Groups.size()) * sizeof(Group) + uint64_t(m_Clusters.size()) * sizeof(Cluster) +
        uint64_t(m_GeneratingGroups.size()) * sizeof(uint32_t) + uint64_t(m_Triangles.size()) * sizeof(uint8_t) +
        uint64_t(m_VertexIndices.size()) * sizeof(uint32_t);
}

void PrimitiveClusters::Clear()
{
    m_Nodes.clear();
    m_NodeBounds.clear();
    m_Groups.clear();
    m_Clusters.clear();
    m_GeneratingGroups.clear();
    m_Triangles.clear();
    m_VertexIndices.clear();
    for (uint32_t level = 0; level < kMaxLODLevels; ++level)
    {
        m_LODMinSphere[level] = TraversalMetric{};
        m_LODMinMaxError[level] = 0.0f;
    }
    m_LODLevelCount = 0;
}

} // namespace ClusterLod
