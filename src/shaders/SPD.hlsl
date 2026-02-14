#include "ShaderShared.h"

#ifndef SPD_NUM_CHANNELS
#define SPD_NUM_CHANNELS 1
#endif

#if SPD_NUM_CHANNELS == 3
#define SPD_TYPE float3
#else
#define SPD_TYPE float
#endif

#define SPD_REDUCTION_MIN 0
#define SPD_REDUCTION_MAX 1
#define SPD_REDUCTION_AVERAGE 2

typedef uint   FfxUInt32;
typedef uint2  FfxUInt32x2;
typedef uint3  FfxUInt32x3;
typedef uint4  FfxUInt32x4;
typedef int    FfxInt32;
typedef int2   FfxInt32x2;
typedef float  FfxFloat32;
typedef float2 FfxFloat32x2;
typedef float3 FfxFloat32x3;
typedef float4 FfxFloat32x4;

FfxUInt32 ffxBitfieldExtract(FfxUInt32 src, FfxUInt32 off, FfxUInt32 bits)
{
    FfxUInt32 mask = (1u << bits) - 1;
    return (src >> off) & mask;
}

FfxUInt32 ffxBitfieldInsertMask(FfxUInt32 src, FfxUInt32 ins, FfxUInt32 bits)
{
    FfxUInt32 mask = (1u << bits) - 1;
    return (ins & mask) | (src & (~mask));
}

/// A helper function performing a remap 64x1 to 8x8 remapping which is necessary for 2D wave reductions.
///
/// The 64-wide lane indices to 8x8 remapping is performed as follows:
/// 
///     00 01 08 09 10 11 18 19
///     02 03 0a 0b 12 13 1a 1b
///     04 05 0c 0d 14 15 1c 1d
///     06 07 0e 0f 16 17 1e 1f
///     20 21 28 29 30 31 38 39
///     22 23 2a 2b 32 33 3a 3b
///     24 25 2c 2d 34 35 3c 3d
///     26 27 2e 2f 36 37 3e 3f
///
/// @param [in] a       The input 1D coordinate to remap.
/// 
/// @returns
/// The remapped 2D coordinates.
/// 
/// @ingroup GPUCore
FfxUInt32x2 ffxRemapForWaveReduction(FfxUInt32 a)
{
    return FfxUInt32x2(ffxBitfieldInsertMask(ffxBitfieldExtract(a, 2u, 3u), a, 1u), ffxBitfieldInsertMask(ffxBitfieldExtract(a, 3u, 3u), ffxBitfieldExtract(a, 1u, 2u), 2u));
}

PUSH_CONSTANT
SpdConstants g_SpdConstants;

// Resource boundaries
Texture2D<SPD_TYPE> g_Mip0 : register(t0);

// NVRHI/SPIR-V mapping
// We use a switch to handle multiple UAVs if bindless is not preferred for this specific task
RWTexture2D<SPD_TYPE> g_Out1 : register(u0);
RWTexture2D<SPD_TYPE> g_Out2 : register(u1);
RWTexture2D<SPD_TYPE> g_Out3 : register(u2);
RWTexture2D<SPD_TYPE> g_Out4 : register(u3);
RWTexture2D<SPD_TYPE> g_Out5 : register(u4);
RWTexture2D<SPD_TYPE> g_Out6 : register(u5);
RWTexture2D<SPD_TYPE> g_Out7 : register(u6);
RWTexture2D<SPD_TYPE> g_Out8 : register(u7);
RWTexture2D<SPD_TYPE> g_Out9 : register(u8);
RWTexture2D<SPD_TYPE> g_Out10 : register(u9);
RWTexture2D<SPD_TYPE> g_Out11 : register(u10);
RWTexture2D<SPD_TYPE> g_Out12 : register(u11);
RWStructuredBuffer<uint> g_AtomicCounter : register(u12);

groupshared FfxFloat32 spdIntermediateR[16][16];
groupshared FfxFloat32 spdIntermediateG[16][16];
groupshared FfxFloat32 spdIntermediateB[16][16];
groupshared FfxFloat32 spdIntermediateA[16][16];

// SPD Callbacks implementations
FfxFloat32x4 SpdLoadSourceImage(FfxInt32x2 tex, FfxUInt32 slice)
{
#if SPD_NUM_CHANNELS == 3
    return float4(g_Mip0.Load(int3(tex, 0)).xyz, 0);
#else
    return g_Mip0.Load(int3(tex, 0)).xxxx;
#endif
}

FfxFloat32x4 SpdLoad(FfxInt32x2 tex, FfxUInt32 slice)
{
    // SPD uses this to load from mip 5 when processing more than 6 mips
#if SPD_NUM_CHANNELS == 3
    return float4(g_Out6.Load(tex).xyz, 0);
#else
    return g_Out6.Load(tex).xxxx;
#endif
}

void SpdStore(FfxInt32x2 pix, FfxFloat32x4 v, FfxUInt32 mip, FfxUInt32 slice)
{
    SPD_TYPE val;
#if SPD_NUM_CHANNELS == 3
    val = v.xyz;
#else
    val = v.x;
#endif

    if (mip == 0) g_Out1[pix] = val;
    else if (mip == 1) g_Out2[pix] = val;
    else if (mip == 2) g_Out3[pix] = val;
    else if (mip == 3) g_Out4[pix] = val;
    else if (mip == 4) g_Out5[pix] = val;
    else if (mip == 5) g_Out6[pix] = val;
    else if (mip == 6) g_Out7[pix] = val;
    else if (mip == 7) g_Out8[pix] = val;
    else if (mip == 8) g_Out9[pix] = val;
    else if (mip == 9) g_Out10[pix] = val;
    else if (mip == 10) g_Out11[pix] = val;
    else if (mip == 11) g_Out12[pix] = val;
}

FfxFloat32x4 SpdLoadIntermediate(FfxUInt32 x, FfxUInt32 y)
{
    return FfxFloat32x4(spdIntermediateR[x][y], spdIntermediateG[x][y], spdIntermediateB[x][y], spdIntermediateA[x][y]);
}

void SpdStoreIntermediate(FfxUInt32 x, FfxUInt32 y, FfxFloat32x4 value)
{
    spdIntermediateR[x][y] = value.x;
    spdIntermediateG[x][y] = value.y;
    spdIntermediateB[x][y] = value.z;
    spdIntermediateA[x][y] = value.w;
}

FfxFloat32x4 SpdReduce4(FfxFloat32x4 v0, FfxFloat32x4 v1, FfxFloat32x4 v2, FfxFloat32x4 v3)
{
    if (g_SpdConstants.m_ReductionType == SPD_REDUCTION_MIN)
        return min(min(v0, v1), min(v2, v3));
    else if (g_SpdConstants.m_ReductionType == SPD_REDUCTION_MAX)
        return max(max(v0, v1), max(v2, v3));
    else // SPD_REDUCTION_AVERAGE
        return (v0 + v1 + v2 + v3) * 0.25;
}

void SpdIncreaseAtomicCounter(FfxUInt32 slice)
{
    uint originalValue;
    InterlockedAdd(g_AtomicCounter[0], 1, originalValue);
}

FfxUInt32 SpdGetAtomicCounter()
{
    return g_AtomicCounter[0];
}

void SpdResetAtomicCounter(FfxUInt32 slice)
{
    g_AtomicCounter[0] = 0;
}

#define FFX_GPU
#define FFX_GROUP_MEMORY_BARRIER GroupMemoryBarrierWithGroupSync()

// Disable wave operations as it does not produce correct results for some reason
// the depth mip results slowly shift "bottom right" and some mips will reduce to "max" instead of "min", despite SpdReduce4
// can't be bothered to debug
#define FFX_SPD_NO_WAVE_OPERATIONS
#include "ffx_spd.h"

[numthreads(256, 1, 1)]
void SPD_CSMain(uint3 workGroupId : SV_GroupID, uint localInvocationIndex : SV_GroupIndex)
{
    SpdDownsample(workGroupId.xy, localInvocationIndex, g_SpdConstants.m_Mips, g_SpdConstants.m_NumWorkGroups, 0, g_SpdConstants.m_WorkGroupOffset);
}
