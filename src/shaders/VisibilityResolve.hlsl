#include "Common.hlsli"
#include "srrhi/hlsl/VisibilityResolve.hlsli"

/*
	Id visualisation.

	The point of this pass today is to make the ids the raster wrote readable before the resolve that turns them
	back into G-buffer values exists: one hash colour per id, black where the raster found no surface, so the
	cluster boundaries and the empty pixels are both visible. It is checked against a numeric dump of the same
	target, never against the reference image - the other five G-buffer targets are not being written yet.

	The real resolve replaces this body: it decodes the id, fetches the cluster's three vertices and writes all
	six targets with the material sampled from them.
*/

static const srrhi::VisibilityResolveConstants g_Constants = srrhi::VisibilityResolveInputs::GetResolveConstants();
static const Texture2D<uint> g_VisibilityBuffer = srrhi::VisibilityResolveInputs::GetVisibilityBuffer();

float4 PSMain(FullScreenVertexOut input) : SV_Target
{
    // SV_Position is the pixel centre, so truncating it lands on this pixel; the clamp only guards the last row
    // or column, and the size comes from the pass rather than a GetDimensions per pixel.
    const uint2 pixel = min(uint2(input.pos.xy), uint2(g_Constants.m_Width - 1u, g_Constants.m_Height - 1u));
    const uint id = g_VisibilityBuffer.Load(int3(pixel, 0));

    return float4(id == 0u ? float3(0.0f, 0.0f, 0.0f) : HashColor(id), 1.0f);
}
