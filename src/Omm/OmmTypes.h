#pragma once

// Opacity micromap data for one (primitive, LOD). The CPU-side vectors are the bake result; the handles are
// filled by the GPU upload, which rebuilds them every session and never caches them.

// One entry of the per-OMM descriptor array the GPU build reads. This mirrors
// D3D12_RAYTRACING_OPACITY_MICROMAP_DESC: a byte offset into the OMM data, then the subdivision level in the
// low half of the second word and the format (1 = 2-state, 2 = 4-state) in the high half.
struct OmmPerOmmDesc
{
    uint32_t m_ByteOffset = 0;
    uint32_t m_SubdivisionLevelAndFormat = 0;
};

static_assert(sizeof(OmmPerOmmDesc) == 8);

struct OmmBakeParams
{
    uint32_t m_MaxLevel = 6;     // a mask has 4^m_MaxLevel micro-triangles per triangle edge
    float    m_TargetEdge = 3.0f; // adaptive subdivision targets a target_edge^2 texel area per micro-triangle
    uint32_t m_MipLevel = 0;     // alpha mip the masks are baked from
    bool     m_bFourState = true;
};

struct OmmBakeStats
{
    uint32_t m_MaskCount = 0;             // distinct mask entries after compaction
    uint32_t m_TransparentTriangles = 0;  // triangles resolved to a special "fully transparent" index
    uint32_t m_OpaqueTriangles = 0;       // triangles resolved to a special "fully opaque" index
    uint32_t m_MaskedTriangles = 0;       // triangles that keep a real mask
    float    m_AverageLevel = 0.0f;
    size_t   m_DataBytes = 0;
    bool     m_bSixteenBitIndices = false;
};

struct OmmDebugTexelStats
{
    // Per texel of the bake mip: 0 opaque, 1 transparent, 2 mixed. "Mixed" means a 2x2 tap footprint straddles
    // the alpha cutoff, which is where bilinear sampling makes the result ambiguous.
    std::vector<uint8_t> m_Class;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    uint32_t m_OpaqueCount = 0;
    uint32_t m_TransparentCount = 0;
    uint32_t m_MixedCount = 0;
};

struct OmmLodData
{
    std::vector<nvrhi::rt::OpacityMicromapUsageCount> m_Counts; // histogram, required by the GPU build
    std::vector<OmmPerOmmDesc> m_PerOmmDescs;                   // one per mask entry
    std::vector<int32_t>  m_Indices;                            // 32-bit path: one per triangle
    std::vector<uint16_t> m_Indices16;                          // 16-bit path, used instead of m_Indices
    std::vector<uint8_t>  m_Data;                               // compacted micro-triangle states
    uint32_t m_SubdivisionLevel = 0;
    uint32_t m_IndexCount = 0;
    OmmBakeStats m_Stats;

    nvrhi::BufferHandle m_DataBuffer;
    nvrhi::BufferHandle m_DescBuffer;
    nvrhi::BufferHandle m_IndexBuffer;
    nvrhi::rt::OpacityMicromapHandle m_Array;
};
