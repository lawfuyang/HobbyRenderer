/*
 * GenerateViewZ.hlsl
 *
 * Converts the raw depth buffer (non-linear, [0,1]) to linear view-space depth
 * and writes it to a float texture.  The result is consumed by:
 *   - PostprocessGBuffer.hlsl  (t1 = LinearZ for bilateral normal filtering)
 *   - NRD denoiser             (IN_VIEWZ)
 */

#include <Rtxdi/DI/ReSTIRDIParameters.h>
#include <Rtxdi/GI/ReSTIRGIParameters.h>
#include <Rtxdi/ReGIR/ReGIRParameters.h>
#include <Rtxdi/LightSampling/RISBufferSegmentParameters.h>
#include "srrhi/hlsl/RTXDI.hlsli"

#define g_Const         srrhi::GenerateViewZInputs::GetConst()
#define t_Depth         srrhi::GenerateViewZInputs::GetDepth()
#define u_LinearDepth   srrhi::GenerateViewZInputs::GetLinearDepth()
#define RTXDI_SCREEN_SPACE_GROUP_SIZE srrhi::RTXDIConstants::RTXDI_SCREEN_SPACE_GROUP_SIZE

[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, RTXDI_SCREEN_SPACE_GROUP_SIZE, 1)]
void main(uint2 pixelPos : SV_DispatchThreadID)
{
    float rawDepth = t_Depth[pixelPos];

    // Reconstruct linear view-space depth from the projection matrix.
    // The depth buffer is written with the jittered projection, so we must
    // use the matching jittered clip-to-view inverse.
    float4 clipPos = float4(0.0, 0.0, rawDepth, 1.0);
    float4 viewPos = mul(clipPos, g_Const.view.m_MatClipToView);
    float linearZ  = viewPos.z / viewPos.w;

    u_LinearDepth[pixelPos] = linearZ;
}
