#include "Common.hlsli"
#include "srrhi/hlsl/StampRaster.hlsli"

/*
	The stamp replay (docs/visibility_buffer_implementation_plan.md, phase 7).

	ClusterComputeRaster.hlsl decided which samples its triangles cover; this pass hands those samples to the
	hardware as one-pixel points, so the depth test, the depth write and the stencil replace stay the rasteriser's
	own. That is what makes the software path authoritative for depth without writing the depth buffer from compute
	(D24S8 is not a UAV, and it carries the base pass' stencil coverage mask).

	Two stamps can name the same pixel - two routed triangles overlapping there - and the depth test resolves them
	in either order, which is why the compute pass needs no ordering of its own.
*/

static const srrhi::StampConstants g_StampsCB = srrhi::StampRasterInputs::GetConstants();
static const StructuredBuffer<uint4> g_Stamps = srrhi::StampRasterInputs::GetStamps();

struct StampVertexOut
{
    float4 m_Position : SV_POSITION;
    uint m_Id : TEXCOORD0;
};

StampVertexOut VSMain(uint instanceIndex : SV_InstanceID)
{
    const uint4 stamp = g_Stamps[instanceIndex];
    const float2 targetSize = float2(g_StampsCB.m_Width, g_StampsCB.m_Height);

    StampVertexOut output;
    // The pixel centre, in the clip space SV_Position expects, at the depth the software raster interpolated.
    output.m_Position = float4((float(stamp.x) + 0.5f) / targetSize.x * 2.0f - 1.0f,
                               1.0f - (float(stamp.y) + 0.5f) / targetSize.y * 2.0f,
                               asfloat(stamp.z),
                               1.0f);
    output.m_Id = stamp.w;
    return output;
}

uint PSMain(StampVertexOut input) : SV_TARGET
{
    return input.m_Id;
}
