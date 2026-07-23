#include "srrhi/hlsl/Common.hlsli"

#if SPD_ARRAY
    #include "srrhi/hlsl/SPD_Array.hlsli"
#else
    #include "srrhi/hlsl/SPD.hlsli"
#endif

#ifndef SPD_NUM_CHANNELS
    #define SPD_NUM_CHANNELS 1
#endif

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

// Use srrhi-generated constants - directly access cbuffer member
#if SPD_ARRAY
static const srrhi::SpdArrayConstants g_SpdConstants = srrhi::SpdArrayInputs::GetSpdConstants();
#else
static const srrhi::SpdConstants g_SpdConstants = srrhi::SpdInputs::GetSpdConstants();
#endif

#if SPD_ARRAY
    #define SPD_LOAD(tex, slice) Load(int4(tex, slice, 0))
    #define SPD_IDX(pix, slice)  uint3(pix, slice)
#else
    #define SPD_LOAD(tex, slice) Load(int3(tex, 0))
    #define SPD_IDX(pix, slice)  pix
#endif

// Resource boundaries
#if SPD_ARRAY
    static Texture2DArray<SPD_TYPE>    g_Mip0  = srrhi::SpdArrayInputs::GetMip0();
    static RWTexture2DArray<SPD_TYPE> g_Out1  = srrhi::SpdArrayInputs::GetOut1();
    static RWTexture2DArray<SPD_TYPE> g_Out2  = srrhi::SpdArrayInputs::GetOut2();
    static RWTexture2DArray<SPD_TYPE> g_Out3  = srrhi::SpdArrayInputs::GetOut3();
    static RWTexture2DArray<SPD_TYPE> g_Out4  = srrhi::SpdArrayInputs::GetOut4();
    static RWTexture2DArray<SPD_TYPE> g_Out5  = srrhi::SpdArrayInputs::GetOut5();
    static RWTexture2DArray<SPD_TYPE> g_Out6  = srrhi::SpdArrayInputs::GetOut6();
    static RWTexture2DArray<SPD_TYPE> g_Out7  = srrhi::SpdArrayInputs::GetOut7();
    static RWTexture2DArray<SPD_TYPE> g_Out8  = srrhi::SpdArrayInputs::GetOut8();
    static RWTexture2DArray<SPD_TYPE> g_Out9  = srrhi::SpdArrayInputs::GetOut9();
    static RWTexture2DArray<SPD_TYPE> g_Out10 = srrhi::SpdArrayInputs::GetOut10();
    static RWTexture2DArray<SPD_TYPE> g_Out11 = srrhi::SpdArrayInputs::GetOut11();
    static RWTexture2DArray<SPD_TYPE> g_Out12 = srrhi::SpdArrayInputs::GetOut12();
    static RWStructuredBuffer<uint> g_AtomicCounter = srrhi::SpdArrayInputs::GetAtomicCounter();
#else
    static Texture2D<SPD_TYPE>    g_Mip0  = srrhi::SpdInputs::GetMip0();
    static RWTexture2D<SPD_TYPE> g_Out1  = srrhi::SpdInputs::GetOut1();
    static RWTexture2D<SPD_TYPE> g_Out2  = srrhi::SpdInputs::GetOut2();
    static RWTexture2D<SPD_TYPE> g_Out3  = srrhi::SpdInputs::GetOut3();
    static RWTexture2D<SPD_TYPE> g_Out4  = srrhi::SpdInputs::GetOut4();
    static RWTexture2D<SPD_TYPE> g_Out5  = srrhi::SpdInputs::GetOut5();
    static RWTexture2D<SPD_TYPE> g_Out6  = srrhi::SpdInputs::GetOut6();
    static RWTexture2D<SPD_TYPE> g_Out7  = srrhi::SpdInputs::GetOut7();
    static RWTexture2D<SPD_TYPE> g_Out8  = srrhi::SpdInputs::GetOut8();
    static RWTexture2D<SPD_TYPE> g_Out9  = srrhi::SpdInputs::GetOut9();
    static RWTexture2D<SPD_TYPE> g_Out10 = srrhi::SpdInputs::GetOut10();
    static RWTexture2D<SPD_TYPE> g_Out11 = srrhi::SpdInputs::GetOut11();
    static RWTexture2D<SPD_TYPE> g_Out12 = srrhi::SpdInputs::GetOut12();
    static RWStructuredBuffer<uint> g_AtomicCounter = srrhi::SpdInputs::GetAtomicCounter();
#endif

groupshared FfxFloat32 spdIntermediateR[16][16];
groupshared FfxFloat32 spdIntermediateG[16][16];
groupshared FfxFloat32 spdIntermediateB[16][16];
groupshared FfxFloat32 spdIntermediateA[16][16];

FfxFloat32x4 SpdLoadSourceImage(FfxInt32x2 tex, FfxUInt32 slice)
{
#if SPD_NUM_CHANNELS == 3
    return float4(g_Mip0.SPD_LOAD(tex, slice).xyz, 0);
#else
    return g_Mip0.SPD_LOAD(tex, slice).xxxx;
#endif
}

FfxFloat32x4 SpdLoad(FfxInt32x2 tex, FfxUInt32 slice)
{
    // SPD uses this to load from mip 5 when processing more than 6 mips
#if SPD_NUM_CHANNELS == 3
    return float4(g_Out6[SPD_IDX(tex, slice)].xyz, 0);
#else
    return g_Out6[SPD_IDX(tex, slice)].xxxx;
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

    if      (mip == 0)  g_Out1[SPD_IDX(pix, slice)]  = val;
    else if (mip == 1)  g_Out2[SPD_IDX(pix, slice)]  = val;
    else if (mip == 2)  g_Out3[SPD_IDX(pix, slice)]  = val;
    else if (mip == 3)  g_Out4[SPD_IDX(pix, slice)]  = val;
    else if (mip == 4)  g_Out5[SPD_IDX(pix, slice)]  = val;
    else if (mip == 5)  g_Out6[SPD_IDX(pix, slice)]  = val;
    else if (mip == 6)  g_Out7[SPD_IDX(pix, slice)]  = val;
    else if (mip == 7)  g_Out8[SPD_IDX(pix, slice)]  = val;
    else if (mip == 8)  g_Out9[SPD_IDX(pix, slice)]  = val;
    else if (mip == 9)  g_Out10[SPD_IDX(pix, slice)] = val;
    else if (mip == 10) g_Out11[SPD_IDX(pix, slice)] = val;
    else if (mip == 11) g_Out12[SPD_IDX(pix, slice)] = val;
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
    if (g_SpdConstants.m_ReductionType == srrhi::CommonConsts::SPD_REDUCTION_MIN)
        return min(min(v0, v1), min(v2, v3));
    else if (g_SpdConstants.m_ReductionType == srrhi::CommonConsts::SPD_REDUCTION_MAX)
        return max(max(v0, v1), max(v2, v3));
    else // srrhi::CommonConsts::SPD_REDUCTION_AVERAGE
        return (v0 + v1 + v2 + v3) * 0.25;
}

void SpdIncreaseAtomicCounter(FfxUInt32 slice)
{
    uint originalValue;
#if SPD_ARRAY
    InterlockedAdd(g_AtomicCounter[slice], 1, originalValue);
#else
    InterlockedAdd(g_AtomicCounter[0], 1, originalValue);
#endif
}

#if SPD_ARRAY
// groupshared used to communicate the current slice index to SpdGetAtomicCounter(),
// which has no slice parameter in the ffx_spd.h interface.
groupshared uint g_CurrentSlice;
#endif

FfxUInt32 SpdGetAtomicCounter()
{
    // In SPD_ARRAY mode the counter for the current slice is read in SpdExitWorkgroup,
    // which calls SpdIncreaseAtomicCounter(slice) then SpdGetAtomicCounter().
    // ffx_spd.h stores the last-incremented slice in the local variable, but
    // SpdGetAtomicCounter has no slice argument. We use a groupshared to bridge this.
#if SPD_ARRAY
    return g_AtomicCounter[g_CurrentSlice];
#else
    return g_AtomicCounter[0];
#endif
}

void SpdResetAtomicCounter(FfxUInt32 slice)
{
#if SPD_ARRAY
    g_AtomicCounter[slice] = 0;
#else
    g_AtomicCounter[0] = 0;
#endif
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
#if SPD_ARRAY
    // Process each array slice sequentially within the same dispatch.
    // Each slice has its own atomic counter element so the "last workgroup" logic
    // works independently per slice.
    for (uint slice = 0; slice < g_SpdConstants.m_NumSlices; ++slice)
    {
        if (localInvocationIndex == 0)
            g_CurrentSlice = slice;
        GroupMemoryBarrierWithGroupSync();
        SpdDownsample(workGroupId.xy, localInvocationIndex, g_SpdConstants.m_Mips, g_SpdConstants.m_NumWorkGroups, slice, g_SpdConstants.m_WorkGroupOffset);
        GroupMemoryBarrierWithGroupSync();
    }
#else
    SpdDownsample(workGroupId.xy, localInvocationIndex, g_SpdConstants.m_Mips, g_SpdConstants.m_NumWorkGroups, 0, g_SpdConstants.m_WorkGroupOffset);
#endif
}
