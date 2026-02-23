#include "ShaderShared.h"

Texture2D<float> g_DepthInput : register(t0);

float DepthCopy_PSMain(FullScreenVertexOut input) : SV_Target
{
    uint2 pixelCoord = uint2(input.pos.xy);
    return g_DepthInput.Load(uint3(pixelCoord, 0));
}
