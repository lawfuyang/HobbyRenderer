#pragma once

// Alpha-channel access for the opacity micromap bake (docs/implementation_plan_omm.md, T1.1).
//
// The bake needs the alpha channel of one texture mip as plain 8-bit values. Scene textures are
// block-compressed DDS -- Bistro and Sponza between them use BC1, BC3 and BC7 -- and the bytes are already
// mapped by the streaming texture, so the decode happens here on the CPU instead of round-tripping the
// texture through the GPU.

struct OmmAlphaMip
{
    const uint8_t* m_Data = nullptr;   // first byte of this mip inside the DDS payload
    size_t         m_Size = 0;         // byte length of this mip
    uint32_t       m_Width = 0;
    uint32_t       m_Height = 0;
};

// Byte length of one mip level, agreeing with TextureLoader::ComputeDDSMipOffsets: block-compressed formats
// round the dimensions up to whole blocks.
[[nodiscard]] size_t GetOmmMipByteSize(nvrhi::Format format, uint32_t width, uint32_t height);

// Decodes one mip into 8-bit alpha, width * height bytes, row-major with no padding.
//
// The formats that carry no alpha channel (BC4, BC5, BC6H) decode as fully opaque, which is how a mask
// material that uses them is drawn anyway. Returns false only for a format the reader does not handle, so a
// caller can skip a primitive instead of baking against invented data.
[[nodiscard]] bool DecodeAlphaMip(const OmmAlphaMip& mip, nvrhi::Format format, std::vector<uint8_t>& outAlpha);
