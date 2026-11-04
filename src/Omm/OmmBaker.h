#pragma once

#include "OmmTypes.h"

// Input for one (primitive, LOD) bake. The UVs are handed over in the renderer's own quantized form so the
// caller does not have to materialise a float array for every vertex of the scene.
struct OmmBakeInput
{
    const uint32_t* m_Indices = nullptr;    // 3 per triangle, indexing the vertex arrays
    size_t          m_IndexCount = 0;       // multiple of 3
    const uint32_t* m_PackedUvs = nullptr;  // one half2 per vertex, low half = u, high half = v
    size_t          m_VertexCount = 0;
    const uint8_t*  m_Alpha = nullptr;      // one byte per texel, row-major, no padding
    uint32_t        m_AlphaWidth = 0;
    uint32_t        m_AlphaHeight = 0;
    float           m_AlphaCutoff = 0.5f;   // material cutoff, 0..1
};

// Subdivides and rasterizes one LOD's triangles against the alpha texture, compacts the result, and fills the
// structures the GPU build consumes. When pTexelStats is given it also classifies the bake mip's texels for the
// debug view. Returns false when there is nothing to bake.
[[nodiscard]] bool BakeLod(const OmmBakeInput& input, const OmmBakeParams& params, OmmLodData& out,
                           OmmDebugTexelStats* pTexelStats = nullptr);
