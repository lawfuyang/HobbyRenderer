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
// debug view. Returns false when there is nothing to bake, which the caller sees as an empty out.m_Data.
bool BakeLod(const OmmBakeInput& input, const OmmBakeParams& params, OmmLodData& out,
             OmmDebugTexelStats* pTexelStats = nullptr);

// Classifies one alpha mip for the debug view: 0 opaque, 1 transparent, 2 mixed. The cutoff is rescaled the same
// way the bake does, so the view shows the states the rasterizer sees rather than the raw texel alpha.
void ClassifyAlphaTexels(const uint8_t* alpha, uint32_t width, uint32_t height, float cutoff,
                         OmmDebugTexelStats& stats);

// Halves a classification conservatively: opaque only when all four children are opaque, transparent only when all
// four are transparent, mixed otherwise. Averaging a mip chain and re-thresholding does the opposite (a block that
// is 60% opaque averages above the cutoff and turns opaque), which is what makes coarse levels claim a state the
// fine level disagrees with.
void ReduceClassificationConservative(const OmmDebugTexelStats& src, OmmDebugTexelStats& dst);
