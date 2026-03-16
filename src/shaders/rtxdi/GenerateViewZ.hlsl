/*
 * GenerateViewZ.hlsl
 *
 * Converts the raw depth buffer (non-linear, [0,1]) to linear view-space depth
 * and writes it to a float texture.  The result is consumed by:
 *   - PostprocessGBuffer.hlsl  (t1 = LinearZ for bilateral normal filtering)
 *   - NRD denoiser             (IN_VIEWZ)
 */

#include "SharedShaderInclude/ShaderParameters.h"

Texture2D<float>    t_Depth       : register(t1);   // raw depth [0,1]
RWTexture2D<float>  u_LinearDepth : register(u0);   // linear view-space Z (positive)

ConstantBuffer<ResamplingConstants> g_Const : register(b0);

[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, RTXDI_SCREEN_SPACE_GROUP_SIZE, 1)]
void main(uint2 pixelPos : SV_DispatchThreadID)
{
    float rawDepth = t_Depth[pixelPos];

    // Reconstruct linear view-space depth from the projection matrix.
    // g_Const.view.matClipToView[3][2] = near * far / (near - far)  (row-major)
    // g_Const.view.matClipToView[2][2] = far / (far - near)
    // For a standard reversed-Z projection:
    //   linearZ = near * far / (far - rawDepth * (far - near))
    // We use the clip-to-view matrix directly to stay projection-agnostic.
    float4 clipPos = float4(0.0, 0.0, rawDepth, 1.0);
    float4 viewPos = mul(clipPos, g_Const.view.m_MatClipToView);
    float linearZ  = viewPos.z / viewPos.w;

    u_LinearDepth[pixelPos] = linearZ;
}
