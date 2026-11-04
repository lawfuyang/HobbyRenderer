#include "AlphaTextureReader.h"

namespace
{
    // Block geometry per nvrhi format. Block-compressed formats are 4x4; the uncompressed ones are per texel.
    struct BlockInfo
    {
        uint32_t m_BlockDim = 1;
        uint32_t m_BytesPerBlock = 1;
        bool     m_bKnown = false;
    };

    BlockInfo GetBlockInfo(nvrhi::Format format)
    {
        switch (format)
        {
        case nvrhi::Format::R8_UNORM:            return { 1, 1, true };
        case nvrhi::Format::RGBA8_UNORM:
        case nvrhi::Format::SRGBA8_UNORM:
        case nvrhi::Format::BGRA8_UNORM:         return { 1, 4, true };
        case nvrhi::Format::BC1_UNORM:
        case nvrhi::Format::BC1_UNORM_SRGB:      return { 4, 8, true };
        case nvrhi::Format::BC2_UNORM:
        case nvrhi::Format::BC2_UNORM_SRGB:      return { 4, 16, true };
        case nvrhi::Format::BC3_UNORM:
        case nvrhi::Format::BC3_UNORM_SRGB:      return { 4, 16, true };
        case nvrhi::Format::BC4_UNORM:
        case nvrhi::Format::BC4_SNORM:           return { 4, 8, true };
        case nvrhi::Format::BC5_UNORM:
        case nvrhi::Format::BC5_SNORM:           return { 4, 16, true };
        case nvrhi::Format::BC6H_UFLOAT:
        case nvrhi::Format::BC6H_SFLOAT:         return { 4, 16, true };
        case nvrhi::Format::BC7_UNORM:
        case nvrhi::Format::BC7_UNORM_SRGB:      return { 4, 16, true };
        default:                                 return {};
        }
    }

    inline uint32_t ReadU16(const uint8_t* p)
    {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8);
    }

    // BC7 packs bit fields least-significant-bit first across the 16-byte block, which is two 64-bit words
    // that reads spill between.
    struct BitStream
    {
        uint64_t m_Low = 0;
        uint64_t m_High = 0;

        uint32_t Read(uint32_t count)
        {
            SDL_assert(count >= 1 && count <= 32);
            const uint64_t mask = (uint64_t(1) << count) - 1;

            const uint32_t bits = uint32_t(m_Low & mask);
            m_Low >>= count;
            m_Low |= (m_High & mask) << (64 - count);
            m_High >>= count;
            return bits;
        }

        uint32_t ReadBit() { return Read(1); }
    };

    inline uint64_t ReadU64(const uint8_t* p)
    {
        uint64_t value = 0;
        for (uint32_t i = 0; i < 8; ++i)
            value |= uint64_t(p[i]) << (8 * i);
        return value;
    }

    // Weights are in 1/64ths, as the BC7 interpolator specifies.
    constexpr uint8_t kWeights2[4] = { 0, 21, 43, 64 };
    constexpr uint8_t kWeights3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
    constexpr uint8_t kWeights4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

    inline uint8_t Interpolate(uint8_t e0, uint8_t e1, uint32_t weight)
    {
        SDL_assert(weight <= 64);
        return uint8_t(((64 - weight) * e0 + weight * e1 + 32) >> 6);
    }

    // Decodes the alpha of one BC7 block. Only the alpha channel is kept, but the block still has to be parsed
    // in full: modes 4 and 5 can rotate a colour channel into the alpha slot, and their alpha indices are a
    // separate set that follows the colour indices rather than sitting beside each texel's colour index.
    void DecodeBc7Alpha(const uint8_t* block, uint8_t outAlpha[16])
    {
        // The mode is a unary code: one bit per mode, ended by the first set bit. Nothing set at all is the
        // reserved mode 8, which the format defines as an all-zero, fully transparent block.
        BitStream bits{ ReadU64(block), ReadU64(block + 8) };

        uint32_t mode = 0;
        while (mode < 8 && bits.ReadBit() == 0)
            ++mode;

        if (mode >= 8)
        {
            for (int i = 0; i < 16; ++i)
                outAlpha[i] = 0;
            return;
        }

        // Modes 0-3 and 7 carry no alpha channel: alpha decodes to 1 for every texel of the block.
        if (mode < 4 || mode == 7)
        {
            for (int i = 0; i < 16; ++i)
                outAlpha[i] = 255;
            return;
        }

        // Among the modes that have alpha, only mode 6 encodes a p-bit, and it is one p-bit per endpoint that
        // applies to all four channels of that endpoint.
        const uint32_t colorBits = mode == 4 ? 5 : 7;
        const uint32_t alphaBits = mode == 4 ? 6 : (mode == 5 ? 8 : 7);
        const bool bHasPBits = mode == 6;

        uint32_t rotation = 0;
        uint32_t indexSelection = 0;
        if (mode == 4 || mode == 5)
        {
            rotation = bits.Read(2);
            if (mode == 4)
                indexSelection = bits.Read(1);
        }

        // Endpoints are stored channel by channel: both reds, both greens, both blues, then both alphas.
        uint32_t endpoints[2][4] = {};
        for (uint32_t channel = 0; channel < 3; ++channel)
        {
            for (uint32_t endpoint = 0; endpoint < 2; ++endpoint)
                endpoints[endpoint][channel] = bits.Read(colorBits);
        }
        for (uint32_t endpoint = 0; endpoint < 2; ++endpoint)
            endpoints[endpoint][3] = bits.Read(alphaBits);

        if (bHasPBits)
        {
            // The p-bit becomes the new least significant bit of every channel of its endpoint.
            for (uint32_t endpoint = 0; endpoint < 2; ++endpoint)
            {
                for (uint32_t channel = 0; channel < 4; ++channel)
                    endpoints[endpoint][channel] <<= 1;

                const uint32_t pBit = bits.ReadBit();
                for (uint32_t channel = 0; channel < 4; ++channel)
                    endpoints[endpoint][channel] |= pBit;
            }
        }

        // Each endpoint is expanded to 8 bits by replicating its most significant bits into the vacated low ones.
        for (uint32_t endpoint = 0; endpoint < 2; ++endpoint)
        {
            const uint32_t colorPrecision = colorBits + (bHasPBits ? 1u : 0u);
            for (uint32_t channel = 0; channel < 3; ++channel)
            {
                endpoints[endpoint][channel] <<= (8 - colorPrecision);
                endpoints[endpoint][channel] |= endpoints[endpoint][channel] >> colorPrecision;
            }

            const uint32_t alphaPrecision = alphaBits + (bHasPBits ? 1u : 0u);
            endpoints[endpoint][3] <<= (8 - alphaPrecision);
            endpoints[endpoint][3] |= endpoints[endpoint][3] >> alphaPrecision;
        }

        // Mode 4 stores a 2-bit primary and a 3-bit secondary index set, mode 5 two 2-bit sets, and mode 6 a
        // single 4-bit set shared by colour and alpha.
        const uint32_t primaryBits = mode == 6 ? 4 : 2;
        const uint32_t secondaryBits = mode == 4 ? 3 : (mode == 5 ? 2 : 0);
        const uint8_t* primaryWeights = mode == 6 ? kWeights4 : kWeights2;
        const uint8_t* secondaryWeights = mode == 4 ? kWeights3 : kWeights2;

        // The first texel of a block is the fix-up index and is stored with one bit less. That is part of the
        // format, not an optimisation, and getting it wrong shifts every index that follows.
        uint32_t primaryIndex[16] = {};
        for (uint32_t texel = 0; texel < 16; ++texel)
            primaryIndex[texel] = bits.Read(texel == 0 ? primaryBits - 1 : primaryBits);

        for (uint32_t texel = 0; texel < 16; ++texel)
        {
            if (secondaryBits == 0)
            {
                outAlpha[texel] = Interpolate(uint8_t(endpoints[0][3]), uint8_t(endpoints[1][3]), primaryWeights[primaryIndex[texel]]);
                continue;
            }

            const uint32_t secondaryIndex = bits.Read(texel == 0 ? secondaryBits - 1 : secondaryBits);

            // The selection bit says which of the two sets belongs to the vector (colour) channels; the alpha
            // channel always takes the other one.
            const uint32_t alphaWeight = indexSelection != 0
                ? primaryWeights[primaryIndex[texel]]
                : secondaryWeights[secondaryIndex];
            const uint32_t colorWeight = indexSelection != 0
                ? secondaryWeights[secondaryIndex]
                : primaryWeights[primaryIndex[texel]];

            const uint8_t decodedAlpha = Interpolate(uint8_t(endpoints[0][3]), uint8_t(endpoints[1][3]), alphaWeight);

            // A rotation moves one colour channel into the alpha slot, so the real alpha is that channel's
            // interpolation and the decoded alpha belongs to the colour.
            outAlpha[texel] = rotation == 0
                ? decodedAlpha
                : Interpolate(uint8_t(endpoints[0][rotation - 1]), uint8_t(endpoints[1][rotation - 1]), colorWeight);
        }
    }

    // Writes one decoded 4x4 block into the destination, clipping at the mip edges.
    void StoreBlock(uint8_t* destination, uint32_t width, uint32_t height, uint32_t baseX, uint32_t baseY, const uint8_t block[16])
    {
        for (uint32_t y = 0; y < 4; ++y)
        {
            const uint32_t targetY = baseY + y;
            if (targetY >= height)
                break;

            for (uint32_t x = 0; x < 4; ++x)
            {
                const uint32_t targetX = baseX + x;
                if (targetX >= width)
                    break;

                destination[size_t(targetY) * width + targetX] = block[y * 4 + x];
            }
        }
    }

    void DecodeBc1BlockAlpha(const uint8_t* block, uint8_t outAlpha[16])
    {
        const uint32_t color0 = ReadU16(block);
        const uint32_t color1 = ReadU16(block + 2);

        for (int i = 0; i < 16; ++i)
            outAlpha[i] = 255;

        // Only the punch-through variant (color0 <= color1) reserves index 3 for transparent black.
        if (color0 > color1)
            return;

        uint32_t indices = uint32_t(block[4]) | (uint32_t(block[5]) << 8) | (uint32_t(block[6]) << 16) | (uint32_t(block[7]) << 24);
        for (int i = 0; i < 16; ++i)
        {
            if ((indices & 3u) == 3u)
                outAlpha[i] = 0;
            indices >>= 2;
        }
    }

    void DecodeBc2BlockAlpha(const uint8_t* block, uint8_t outAlpha[16])
    {
        // Four bits per texel, the block's first eight bytes, expanded by replication (n * 17).
        for (int i = 0; i < 16; ++i)
        {
            const uint32_t nibble = (i & 1) != 0 ? (block[i >> 1] >> 4) : (block[i >> 1] & 0xFu);
            outAlpha[i] = uint8_t(nibble * 17u);
        }
    }

    void DecodeBc3BlockAlpha(const uint8_t* block, uint8_t outAlpha[16])
    {
        const uint32_t a0 = block[0];
        const uint32_t a1 = block[1];

        uint8_t palette[8] = {};
        palette[0] = uint8_t(a0);
        palette[1] = uint8_t(a1);
        if (a0 > a1)
        {
            for (uint32_t i = 0; i < 6; ++i)
                palette[2 + i] = uint8_t(((6 - i) * a0 + (1 + i) * a1 + 3) / 7);
        }
        else
        {
            for (uint32_t i = 0; i < 4; ++i)
                palette[2 + i] = uint8_t(((4 - i) * a0 + (1 + i) * a1 + 2) / 5);
            palette[6] = 0;
            palette[7] = 255;
        }

        uint64_t indices = 0;
        for (uint32_t i = 0; i < 6; ++i)
            indices |= uint64_t(block[2 + i]) << (8 * i);

        for (int i = 0; i < 16; ++i)
        {
            outAlpha[i] = palette[indices & 7u];
            indices >>= 3;
        }
    }
}

size_t GetOmmMipByteSize(nvrhi::Format format, uint32_t width, uint32_t height)
{
    const BlockInfo info = GetBlockInfo(format);
    if (!info.m_bKnown)
        return 0;

    const uint32_t blocksW = (width + info.m_BlockDim - 1) / info.m_BlockDim;
    const uint32_t blocksH = (height + info.m_BlockDim - 1) / info.m_BlockDim;
    return size_t(std::max(1u, blocksW)) * std::max(1u, blocksH) * info.m_BytesPerBlock;
}

bool DecodeAlphaMip(const OmmAlphaMip& mip, nvrhi::Format format, std::vector<uint8_t>& outAlpha)
{
    SDL_assert(mip.m_Data != nullptr);
    SDL_assert(mip.m_Width > 0 && mip.m_Height > 0);
    SDL_assert(mip.m_Size == GetOmmMipByteSize(format, mip.m_Width, mip.m_Height));

    const BlockInfo info = GetBlockInfo(format);
    if (!info.m_bKnown)
        return false;

    outAlpha.assign(size_t(mip.m_Width) * mip.m_Height, 255);

    switch (format)
    {
    case nvrhi::Format::R8_UNORM:
        std::memcpy(outAlpha.data(), mip.m_Data, outAlpha.size());
        return true;

    case nvrhi::Format::RGBA8_UNORM:
    case nvrhi::Format::SRGBA8_UNORM:
    case nvrhi::Format::BGRA8_UNORM:
        // Alpha is the fourth byte in both channel orders.
        for (size_t i = 0; i < outAlpha.size(); ++i)
            outAlpha[i] = mip.m_Data[i * 4 + 3];
        return true;

    case nvrhi::Format::BC4_UNORM:
    case nvrhi::Format::BC4_SNORM:
    case nvrhi::Format::BC5_UNORM:
    case nvrhi::Format::BC5_SNORM:
    case nvrhi::Format::BC6H_UFLOAT:
    case nvrhi::Format::BC6H_SFLOAT:
        // No alpha channel: the decoded alpha of such a texture is 1, and the mask is opaque.
        return true;

    default:
        break;
    }

    const uint32_t blocksW = (mip.m_Width + 3) / 4;
    const uint32_t blocksH = (mip.m_Height + 3) / 4;

    for (uint32_t blockY = 0; blockY < blocksH; ++blockY)
    {
        for (uint32_t blockX = 0; blockX < blocksW; ++blockX)
        {
            const uint8_t* block = mip.m_Data + (size_t(blockY) * blocksW + blockX) * info.m_BytesPerBlock;
            uint8_t texels[16] = {};

            switch (format)
            {
            case nvrhi::Format::BC1_UNORM:
            case nvrhi::Format::BC1_UNORM_SRGB:
                DecodeBc1BlockAlpha(block, texels);
                break;
            case nvrhi::Format::BC2_UNORM:
            case nvrhi::Format::BC2_UNORM_SRGB:
                DecodeBc2BlockAlpha(block, texels);
                break;
            case nvrhi::Format::BC3_UNORM:
            case nvrhi::Format::BC3_UNORM_SRGB:
                DecodeBc3BlockAlpha(block, texels);
                break;
            case nvrhi::Format::BC7_UNORM:
            case nvrhi::Format::BC7_UNORM_SRGB:
                DecodeBc7Alpha(block, texels);
                break;
            default:
                SDL_assert(false); // GetBlockInfo accepted a format this switch does not decode
                return false;
            }

            StoreBlock(outAlpha.data(), mip.m_Width, mip.m_Height, blockX * 4, blockY * 4, texels);
        }
    }

    return true;
}
