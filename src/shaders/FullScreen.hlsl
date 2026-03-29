#include "Common.hlsli"

[numthreads(3, 1, 1)]
[outputtopology("triangle")]
void MSMain(
    uint gtid : SV_GroupThreadID,
    out vertices FullScreenVertexOut verts[3],
    out indices uint3 triangles[1]
)
{
    SetMeshOutputCounts(3, 1);

    if (gtid == 0)
    {
        triangles[0] = uint3(0, 1, 2);
    }

    // Fullscreen triangle trick:
    // UV: (0,0), (0,2), (2,0)
    // Pos: (-1,1), (-1,-3), (3,1)
    
    float2 uv = float2((gtid << 1) & 2, gtid & 2);
    float4 pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);

    verts[gtid].pos = pos;
    verts[gtid].uv = uv;
}
