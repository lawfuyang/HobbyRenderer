#include "OmmBaker.h"

#include <meshoptimizer.h>

namespace
{
    // Special per-triangle indices after compaction: -1/-3 resolve to fully transparent, -2/-4 to fully opaque
    // (the -3/-4 forms are the 4-state variants whose "other" state was unknown).
    constexpr int32_t kSpecialTransparent2State = -1;
    constexpr int32_t kSpecialOpaque2State = -2;
    constexpr int32_t kSpecialTransparent4State = -3;
    constexpr int32_t kSpecialOpaque4State = -4;

    // meshopt_opacityMapRasterize bakes the cutoff in at 0.5, so the alpha is rescaled once such that the
    // material's cutoff lands there. Without this a material that cuts at 0.25 would lose every texel between
    // the two thresholds.
    std::vector<uint8_t> ScaleAlphaToHalfCutoff(const uint8_t* alpha, size_t texelCount, float cutoff)
    {
        SDL_assert(cutoff > 0.0f && cutoff <= 1.0f);

        const float scale = 0.5f / cutoff;
        std::vector<uint8_t> scaled(texelCount);
        for (size_t i = 0; i < texelCount; ++i)
        {
            const float value = float(alpha[i]) * scale + 0.5f;
            scaled[i] = uint8_t(std::min(255.0f, value));
        }
        return scaled;
    }

    void ClassifyTexels(const std::vector<uint8_t>& alpha, uint32_t width, uint32_t height, OmmDebugTexelStats& stats)
    {
        SDL_assert(alpha.size() == size_t(width) * height);

        stats.m_Width = width;
        stats.m_Height = height;
        stats.m_Class.assign(alpha.size(), 0);

        // Counters describe this texture only: the caller classifies one primitive''s texture and reports it, so
        // accumulating across calls would divide a multi-texture total by a single texture''s texel count.
        stats.m_OpaqueCount = 0;
        stats.m_TransparentCount = 0;
        stats.m_MixedCount = 0;

        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                // A 2x2 tap footprint: what a bilinear sample in this corner can reach.
                bool bAnyOpaque = false;
                bool bAnyTransparent = false;
                for (uint32_t dy = 0; dy < 2; ++dy)
                {
                    for (uint32_t dx = 0; dx < 2; ++dx)
                    {
                        const uint32_t sx = std::min(x + dx, width - 1);
                        const uint32_t sy = std::min(y + dy, height - 1);
                        if (alpha[size_t(sy) * width + sx] >= 128)
                            bAnyOpaque = true;
                        else
                            bAnyTransparent = true;
                    }
                }

                uint8_t texelClass = 0;
                if (bAnyOpaque && bAnyTransparent)
                {
                    texelClass = 2;
                    ++stats.m_MixedCount;
                }
                else if (bAnyTransparent)
                {
                    texelClass = 1;
                    ++stats.m_TransparentCount;
                }
                else
                {
                    ++stats.m_OpaqueCount;
                }

                stats.m_Class[size_t(y) * width + x] = texelClass;
            }
        }
    }
}

void ClassifyAlphaTexels(const uint8_t* alpha, uint32_t width, uint32_t height, float cutoff,
                         OmmDebugTexelStats& stats)
{
    const std::vector<uint8_t> scaled = ScaleAlphaToHalfCutoff(alpha, size_t(width) * height, cutoff);
    ClassifyTexels(scaled, width, height, stats);
}

void ReduceClassificationConservative(const OmmDebugTexelStats& src, OmmDebugTexelStats& dst)
{
    SDL_assert(src.m_Width > 0 && src.m_Height > 0);

    dst.m_Width = std::max(1u, src.m_Width / 2);
    dst.m_Height = std::max(1u, src.m_Height / 2);
    dst.m_Class.assign(size_t(dst.m_Width) * dst.m_Height, 0);
    dst.m_OpaqueCount = 0;
    dst.m_TransparentCount = 0;
    dst.m_MixedCount = 0;

    for (uint32_t y = 0; y < dst.m_Height; ++y)
    {
        for (uint32_t x = 0; x < dst.m_Width; ++x)
        {
            uint8_t merged = 0xFF;
            for (uint32_t dy = 0; dy < 2 && merged != 2; ++dy)
            {
                for (uint32_t dx = 0; dx < 2; ++dx)
                {
                    const uint32_t sx = std::min(x * 2 + dx, src.m_Width - 1);
                    const uint32_t sy = std::min(y * 2 + dy, src.m_Height - 1);
                    const uint8_t child = src.m_Class[size_t(sy) * src.m_Width + sx];

                    // Any mixed child, or two children that disagree, make the parent unknown. Only a block whose
                    // children all agree keeps a definitive state.
                    if (child == 2 || (merged != 0xFF && merged != child))
                    {
                        merged = 2;
                        break;
                    }

                    merged = child;
                }
            }

            dst.m_Class[size_t(y) * dst.m_Width + x] = merged;
            if (merged == 0)      ++dst.m_OpaqueCount;
            else if (merged == 1) ++dst.m_TransparentCount;
            else                  ++dst.m_MixedCount;
        }
    }
}

bool BakeLod(const OmmBakeInput& input, const OmmBakeParams& params, OmmLodData& out, OmmDebugTexelStats* pTexelStats)
{
    SDL_assert(input.m_Indices != nullptr);
    SDL_assert(input.m_IndexCount > 0 && (input.m_IndexCount % 3) == 0);
    SDL_assert(input.m_PackedUvs != nullptr && input.m_VertexCount > 0);
    SDL_assert(input.m_Alpha != nullptr && input.m_AlphaWidth > 0 && input.m_AlphaHeight > 0);
    SDL_assert(params.m_MaxLevel >= 1 && params.m_MaxLevel <= 12);

    const size_t triangleCount = input.m_IndexCount / 3;
    const int states = params.m_bFourState ? 4 : 2;

    // Dequantize the half2 UVs into the float2 array the baker wants.
    std::vector<float> uvs(input.m_VertexCount * 2);
    for (size_t i = 0; i < input.m_VertexCount; ++i)
    {
        const uint32_t packed = input.m_PackedUvs[i];
        uvs[i * 2 + 0] = meshopt_dequantizeHalf(uint16_t(packed & 0xFFFF));
        uvs[i * 2 + 1] = meshopt_dequantizeHalf(uint16_t(packed >> 16));
    }

    const std::vector<uint8_t> alpha = ScaleAlphaToHalfCutoff(input.m_Alpha, size_t(input.m_AlphaWidth) * input.m_AlphaHeight,
                                                              input.m_AlphaCutoff);

    if (pTexelStats != nullptr)
        ClassifyTexels(alpha, input.m_AlphaWidth, input.m_AlphaHeight, *pTexelStats);

    // Measure: one entry per distinct UV triangle, with an adaptive subdivision level each.
    std::vector<uint8_t> levels(triangleCount, 0);
    std::vector<uint32_t> sources(triangleCount, 0);
    std::vector<int32_t> ommIndices(triangleCount, 0);

    size_t ommCount = meshopt_opacityMapMeasure(levels.data(), sources.data(), ommIndices.data(), input.m_Indices,
                                                input.m_IndexCount, uvs.data(), input.m_VertexCount, sizeof(float) * 2,
                                                input.m_AlphaWidth, input.m_AlphaHeight, int(params.m_MaxLevel),
                                                params.m_TargetEdge);
    if (ommCount == 0)
        return false;

    // Layout: each entry's size follows from its level and the state count.
    std::vector<uint32_t> offsets(ommCount, 0);
    size_t dataSize = 0;
    for (size_t i = 0; i < ommCount; ++i)
    {
        offsets[i] = uint32_t(dataSize);
        dataSize += meshopt_opacityMapEntrySize(levels[i], states);
    }

    std::vector<uint8_t> data(dataSize, 0);

    // Sequential on purpose: BakeLod is one unit of work that callers hand to a parallel loop, so fanning out here
    // as well would oversubscribe the machine and put the same entries on every core twice.
    for (size_t i = 0; i < ommCount; ++i)
    {
        const uint32_t triangle = sources[i];
        SDL_assert(size_t(triangle) * 3 + 2 < input.m_IndexCount);

        const size_t i0 = size_t(input.m_Indices[triangle * 3 + 0]) * 2;
        const size_t i1 = size_t(input.m_Indices[triangle * 3 + 1]) * 2;
        const size_t i2 = size_t(input.m_Indices[triangle * 3 + 2]) * 2;

        // Corner order matters: the mask is stored relative to it, and it is only rotated to a canonical order
        // once, before the mesh is cooked, so that both the bake and the BLAS index buffer agree.
        meshopt_opacityMapRasterize(&data[offsets[i]], levels[i], states, &uvs[i0], &uvs[i1], &uvs[i2], alpha.data(),
                                    1, input.m_AlphaWidth, input.m_AlphaWidth, input.m_AlphaHeight);
    }

    // Compact: drop duplicate masks and fold uniform ones into the special indices.
    ommCount = meshopt_opacityMapCompact(data.data(), dataSize, levels.data(), offsets.data(), ommCount, ommIndices.data(),
                                         triangleCount, states);
    dataSize = (ommCount == 0) ? 0 : offsets[ommCount - 1] + meshopt_opacityMapEntrySize(levels[ommCount - 1], states);
    data.resize(dataSize);

    out.m_Data = std::move(data);
    out.m_SubdivisionLevel = params.m_MaxLevel;
    out.m_IndexCount = uint32_t(input.m_IndexCount);
    out.m_Counts.clear();
    out.m_PerOmmDescs.clear();
    out.m_Indices.clear();
    out.m_Indices16.clear();

    const nvrhi::rt::OpacityMicromapFormat format = params.m_bFourState ? nvrhi::rt::OpacityMicromapFormat::OC1_4_State
                                                                       : nvrhi::rt::OpacityMicromapFormat::OC1_2_State;

    OmmBakeStats& stats = out.m_Stats;
    stats = {};
    stats.m_MaskCount = uint32_t(ommCount);
    stats.m_DataBytes = out.m_Data.size();

    // The histogram the GPU build needs: how many masks exist per (subdivision level, format).
    std::vector<uint32_t> perLevel(params.m_MaxLevel + 1, 0);
    for (size_t i = 0; i < ommCount; ++i)
    {
        SDL_assert(levels[i] <= params.m_MaxLevel);
        ++perLevel[levels[i]];
    }
    for (uint32_t level = 0; level <= params.m_MaxLevel; ++level)
    {
        if (perLevel[level] == 0)
            continue;

        nvrhi::rt::OpacityMicromapUsageCount usage = {};
        usage.count = perLevel[level];
        usage.subdivisionLevel = level;
        usage.format = format;
        out.m_Counts.push_back(usage);
    }

    out.m_PerOmmDescs.resize(ommCount);
    for (size_t i = 0; i < ommCount; ++i)
    {
        out.m_PerOmmDescs[i].m_ByteOffset = offsets[i];
        out.m_PerOmmDescs[i].m_SubdivisionLevelAndFormat = (uint32_t(levels[i]) & 0xFFFFu) |
            ((uint32_t(format) & 0xFFFFu) << 16);
    }

    // 16-bit indices only work while every real index stays below the four reserved values.
    const bool bSixteenBit = ommCount <= 0xFFFB;
    stats.m_bSixteenBitIndices = bSixteenBit;

    if (bSixteenBit)
    {
        out.m_Indices16.resize(triangleCount);
        for (size_t i = 0; i < triangleCount; ++i)
        {
            const int32_t index = ommIndices[i];
            out.m_Indices16[i] = index >= 0 ? uint16_t(index) : uint16_t(0x10000 + index);
        }
        SDL_assert(out.m_Indices16.size() == triangleCount);
    }
    else
    {
        out.m_Indices = ommIndices;
        SDL_assert(out.m_Indices.size() == triangleCount);
    }

    uint64_t levelSum = 0;
    for (size_t i = 0; i < triangleCount; ++i)
    {
        const int32_t index = ommIndices[i];
        if (index == kSpecialTransparent2State || index == kSpecialTransparent4State)
            ++stats.m_TransparentTriangles;
        else if (index == kSpecialOpaque2State || index == kSpecialOpaque4State)
            ++stats.m_OpaqueTriangles;
        else
            ++stats.m_MaskedTriangles;
    }
    for (size_t i = 0; i < ommCount; ++i)
        levelSum += levels[i];
    stats.m_AverageLevel = ommCount != 0 ? float(double(levelSum) / double(ommCount)) : 0.0f;

    return true;
}
