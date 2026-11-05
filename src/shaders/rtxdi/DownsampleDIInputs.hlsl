#pragma pack_matrix(row_major)

#include <Rtxdi/DI/ReSTIRDIParameters.h>
#include <Rtxdi/GI/ReSTIRGIParameters.h>
#include <Rtxdi/ReGIR/ReGIRParameters.h>
#include <Rtxdi/LightSampling/RISBufferSegmentParameters.h>
#include "SharcCommon.h"
#include "srrhi/hlsl/RTXDI.hlsli"

#define g_Const                       srrhi::DownsampleDIInputs::GetConst()
#define t_ViewDepth                   srrhi::DownsampleDIInputs::GetViewDepth()
#define t_MotionVectors               srrhi::DownsampleDIInputs::GetMotionVectors()
#define t_NormalRoughness             srrhi::DownsampleDIInputs::GetNormalRoughness()
#define u_HalfViewDepth               srrhi::DownsampleDIInputs::GetHalfViewDepth()
#define u_HalfMotionVectors           srrhi::DownsampleDIInputs::GetHalfMotionVectors()
#define u_HalfNormalRoughness         srrhi::DownsampleDIInputs::GetHalfNormalRoughness()

// Builds the reduced-resolution inputs of the NRD chain from the full-resolution
// screen-space buffers. Only runs when the DI chain is below the render resolution.
//
// Each value is point sampled from the top-left texel of the block the destination
// pixel stands for: view depth, motion vectors and packed normal+roughness all
// describe one surface, so blending them across a geometry edge would corrupt the
// denoiser's reprojection rather than smooth it.
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const int2 src = int2(pixel) * int(g_Const.gbufferScale);

    u_HalfViewDepth[pixel]       = t_ViewDepth[src];
    u_HalfMotionVectors[pixel]   = t_MotionVectors[src];
    u_HalfNormalRoughness[pixel] = t_NormalRoughness[src];
}
