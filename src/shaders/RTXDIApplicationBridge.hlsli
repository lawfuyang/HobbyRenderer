/*
 * RTXDIApplicationBridge.hlsli
 *
 * Full implementation of the RTXDI Application Bridge (RAB) interface.
 * Connects the RTXDI resampling algorithms to the application's G-buffer,
 * material model and analytic light buffer.
 *
 * Resource binding layout used by all three RTXDI compute shaders:
 *   b1  – RTXDIConstants constant buffer
 *   t0  – g_RTXDI_NeighborOffsets  (Buffer<float2>)
 *   t1  – g_Depth            (Texture2D<float>)
 *   t2  – g_GBufferNormals   (Texture2D<float2>)
 *   t3  – g_GBufferAlbedo    (Texture2D<float4>)
 *   t4  – g_GBufferORM       (Texture2D<float2>)
 *   t5  – g_GBufferMV        (Texture2D<float2>)
 *   t6  – g_Lights           (StructuredBuffer<GPULight>)
 *   t7  – g_SceneAS          (RaytracingAccelerationStructure)
 *   u0  – g_RTXDI_RISBuffer              (RWBuffer<uint2>)
 *   u1  – g_RTXDI_LightReservoirBuffer   (RWStructuredBuffer<RTXDI_PackedDIReservoir>)
 *   u2  – g_RTXDIDIOutput               (RWTexture2D<float4>)
 */

#pragma once

// ============================================================================
// RTXDI feature flags — must be set before including RTXDI headers
// ============================================================================
#define RTXDI_ENABLE_RESTIR_DI 1

// Disable presampling: we have no pre-tiled RIS for Phase 1 analytic lights.
// This selects the simpler uniform / infinite light samplers in InitialSampling.hlsli.
#define RTXDI_ENABLE_PRESAMPLING 0

// ============================================================================
// Application includes
// ============================================================================
#include "ShaderShared.h"
#include "CommonLighting.hlsli"

// Pull in RTXDI parameter structs (defines RTXDI_PackedDIReservoir etc.)
// This must come before any resource declaration that uses RTXDI types.
#include "Rtxdi/RtxdiParameters.h"

// ============================================================================
// Resource declarations
// ============================================================================
cbuffer RTXDICBuf : register(b1)
{
    RTXDIConstants g_RTXDIConst;
};

StructuredBuffer<float2>                        g_RTXDI_NeighborOffsets     : register(t0);
Texture2D<float>                                g_Depth                     : register(t1);
Texture2D<float2>                               g_GBufferNormals            : register(t2);
Texture2D<float4>                               g_GBufferAlbedo             : register(t3);
Texture2D<float2>                               g_GBufferORM                : register(t4);
Texture2D<float2>                               g_GBufferMV                 : register(t5);
StructuredBuffer<GPULight>                      g_Lights                    : register(t6);
RaytracingAccelerationStructure                 g_SceneAS                   : register(t7);

RWStructuredBuffer<uint2>                       g_RTXDI_RISBuffer           : register(u0);
RWStructuredBuffer<RTXDI_PackedDIReservoir>     g_RTXDI_LightReservoirBuffer: register(u1);
RWTexture2D<float4>                             g_RTXDIDIOutput             : register(u2);

// Hook up the RTXDI SDK macro names to our resources
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER   g_RTXDI_NeighborOffsets
#define RTXDI_RIS_BUFFER                g_RTXDI_RISBuffer
#define RTXDI_LIGHT_RESERVOIR_BUFFER    g_RTXDI_LightReservoirBuffer

// ============================================================================
// RAB_RandomSamplerState — robust PCG-based RNG
// ============================================================================
struct RAB_RandomSamplerState
{
    uint seed;
    uint index;
};

RAB_RandomSamplerState RAB_InitRandomSampler(uint2 pixelIndex, uint inPass)
{
    RAB_RandomSamplerState rng;
    // PCG seed: mix pixel position, pass, and frame index
    uint h = pixelIndex.x * 1973u + pixelIndex.y * 9277u
           + inPass * 26699u + g_RTXDIConst.m_FrameIndex * 2699u;
    h ^= h >> 16u;
    h *= 0x45d9f3bu;
    h ^= h >> 16u;
    rng.seed  = h;
    rng.index = 1u;
    return rng;
}

float RAB_GetNextRandom(inout RAB_RandomSamplerState rng)
{
    // PCG hash
    uint state = rng.seed * 747796405u + rng.index * 2891336453u;
    rng.index++;
    uint word   = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    uint result = (word >> 22u) ^ word;
    return float(result) * (1.0 / 4294967296.0);
}

// ============================================================================
// RAB_Material
// ============================================================================
struct RAB_Material
{
    float3 diffuseAlbedo;
    float3 specularF0;
    float  roughness;
};

RAB_Material RAB_EmptyMaterial()
{
    RAB_Material m;
    m.diffuseAlbedo = float3(0.5, 0.5, 0.5);
    m.specularF0    = float3(0.04, 0.04, 0.04);
    m.roughness     = 0.5;
    return m;
}

float3 RAB_GetDiffuseAlbedo(RAB_Material material) { return material.diffuseAlbedo; }
float3 RAB_GetSpecularF0(RAB_Material material)     { return material.specularF0; }
float  RAB_GetRoughness(RAB_Material material)      { return material.roughness; }

RAB_Material RAB_GetGBufferMaterial(int2 pixelPosition, bool previousFrame)
{
    return RAB_EmptyMaterial();
}

bool RAB_AreMaterialsSimilar(RAB_Material a, RAB_Material b)
{
    return true;
}

bool RAB_MaterialWithoutSpecularity(RAB_Material material)
{
    return all(material.specularF0 < float3(0.05, 0.05, 0.05));
}

// ============================================================================
// RAB_Surface
// ============================================================================
struct RAB_Surface
{
    float3       worldPos;
    float3       normal;
    float        linearDepth;
    RAB_Material material;
    float3       viewDir;
    float        roughness;
};

RAB_Surface RAB_EmptySurface()
{
    RAB_Surface s;
    s.worldPos    = float3(0.0, 0.0, 0.0);
    s.normal      = float3(0.0, 1.0, 0.0);
    s.linearDepth = 1e10;
    s.material    = RAB_EmptyMaterial();
    s.viewDir     = float3(0.0, 0.0, 1.0);
    s.roughness   = 0.5;
    return s;
}

bool   RAB_IsSurfaceValid(RAB_Surface s)          { return s.linearDepth < 1e9; }
float3 RAB_GetSurfaceWorldPos(RAB_Surface s)       { return s.worldPos; }
float3 RAB_GetSurfaceNormal(RAB_Surface s)         { return s.normal; }
float3 RAB_GetSurfaceViewDirection(RAB_Surface s)  { return s.viewDir; }
float  RAB_GetSurfaceLinearDepth(RAB_Surface s)    { return s.linearDepth; }
float  RAB_GetSurfaceAlpha(RAB_Surface s)          { return 1.0; }
RAB_Material RAB_GetMaterial(RAB_Surface s)        { return s.material; }

// ---- World-position reconstruction from depth + view constants -------------
float3 ReconstructWorldPos(uint2 pixelPos, float depth, PlanarViewConstants view)
{
    float2 uv = (float2(pixelPos) + 0.5) / float2(g_RTXDIConst.m_ViewportSize);
    float4 clipPos = float4(UVToClipXY(uv), depth, 1.0);
    float4 worldPosFour = MatrixMultiply(clipPos, view.m_MatClipToWorldNoOffset);
    return worldPosFour.xyz / worldPosFour.w;
}

RAB_Surface RAB_GetGBufferSurface(int2 pixelPosition, bool previousFrame)
{
    PlanarViewConstants view;
    if (previousFrame)
        view = g_RTXDIConst.m_PrevView;
    else
        view = g_RTXDIConst.m_View;

    // For the previous frame position apply motion-vector reprojection
    int2 samplePos = pixelPosition;
    if (previousFrame)
    {
        float2 mv     = g_GBufferMV.Load(int3(pixelPosition, 0)).xy;
        float2 vpSize = float2(g_RTXDIConst.m_ViewportSize);
        // Motion vectors are stored as per-pixel displacement in NDC; convert to pixel offset
        samplePos = clamp(
            pixelPosition - int2(mv * vpSize * float2(0.5, -0.5)),
            int2(0, 0),
            int2(g_RTXDIConst.m_ViewportSize) - int2(1, 1));
    }

    float depth = g_Depth.Load(int3(samplePos, 0));

    // Sky / background pixel — no surface
    if (depth == 0.0)
        return RAB_EmptySurface();

    RAB_Surface s;
    s.worldPos    = ReconstructWorldPos(uint2(samplePos), depth, view);
    s.normal      = DecodeNormal(g_GBufferNormals.Load(int3(samplePos, 0)));

    // Camera world position: last column of ViewToWorld matrix
    float3 camPos = float3(
        view.m_MatViewToWorld._41,
        view.m_MatViewToWorld._42,
        view.m_MatViewToWorld._43);

    s.linearDepth = length(s.worldPos - camPos);
    s.viewDir     = normalize(camPos - s.worldPos);

    float4 albedoSample = g_GBufferAlbedo.Load(int3(samplePos, 0));
    float2 orm          = g_GBufferORM.Load(int3(samplePos, 0));
    float  roughness    = orm.r;
    float  metallic     = orm.g;
    float3 baseColor    = albedoSample.rgb;
    float3 F0           = ComputeF0(baseColor, metallic, 1.5);

    s.material.diffuseAlbedo = baseColor * (1.0 - metallic);
    s.material.specularF0    = F0;
    s.material.roughness     = roughness;
    s.roughness              = roughness;

    return s;
}

bool RAB_GetSurfaceBrdfSample(RAB_Surface surface, inout RAB_RandomSamplerState rng, out float3 direction)
{
    float r1 = RAB_GetNextRandom(rng);
    float r2 = RAB_GetNextRandom(rng);
    direction = SampleHemisphereCosine(float2(r1, r2), surface.normal);
    return true;
}

float RAB_GetSurfaceBrdfPdf(RAB_Surface surface, float3 direction)
{
    float NdotL = max(0.0, dot(surface.normal, direction));
    return NdotL / PI;
}

float RAB_GetBrdfSampleTargetPdfForSurface(RAB_Surface surface, float3 direction)
{
    float NdotL = max(0.0, dot(surface.normal, direction));
    return NdotL / PI;
}

float3 RAB_SampleDiffuseDirection(RAB_Surface surface, inout RAB_RandomSamplerState rng)
{
    float r1 = RAB_GetNextRandom(rng);
    float r2 = RAB_GetNextRandom(rng);
    return SampleHemisphereCosine(float2(r1, r2), surface.normal);
}

// ============================================================================
// RAB_LightSample
// ============================================================================
struct RAB_LightSample
{
    float3 position;
    float3 radiance;
    float3 direction;   // unit vector FROM surface TO light
    float  distance;    // 1e10 for directional
    float  solidAnglePdf;
};

RAB_LightSample RAB_EmptyLightSample()
{
    RAB_LightSample s;
    s.position      = float3(0.0, 0.0, 0.0);
    s.radiance      = float3(0.0, 0.0, 0.0);
    s.direction     = float3(0.0, 1.0, 0.0);
    s.distance      = 1e10;
    s.solidAnglePdf = 0.0;
    return s;
}

bool  RAB_IsAnalyticLightSample(RAB_LightSample s)   { return true; }
float RAB_LightSampleSolidAnglePdf(RAB_LightSample s) { return s.solidAnglePdf; }

float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample lightSample, RAB_Surface surface)
{
    float NdotL = max(0.0, dot(surface.normal, lightSample.direction));
    float lum   = dot(lightSample.radiance, float3(0.2126, 0.7152, 0.0722));
    return lum * NdotL;
}

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample lightSample,
    out float3 lightDir, out float lightDistance)
{
    lightDir      = lightSample.direction;
    lightDistance = lightSample.distance;
}

float RAB_GetLightRayLength(RAB_Surface surface, RAB_LightSample sample)
{
    return sample.distance;
}

// ============================================================================
// RAB_LightInfo
// ============================================================================
struct RAB_LightInfo
{
    float3 position;
    float3 direction;
    float3 color;
    float  intensity;
    float  range;
    float  spotInnerCos;
    float  spotOuterCos;
    uint   lightType; // 0=directional, 1=point, 2=spot
};

struct RAB_RayPayload
{
    float  hitDistance;
    float3 throughput;
};

RAB_LightInfo RAB_EmptyLightInfo()
{
    RAB_LightInfo li;
    li.position     = float3(0.0, 0.0, 0.0);
    li.direction    = float3(0.0, 1.0, 0.0);
    li.color        = float3(1.0, 1.0, 1.0);
    li.intensity    = 0.0;
    li.range        = 0.0;
    li.spotInnerCos = 0.0;
    li.spotOuterCos = 0.0;
    li.lightType    = 0;
    return li;
}

RAB_LightInfo RAB_LoadLightInfo(uint lightIndex, bool previousFrame)
{
    GPULight gl = g_Lights[lightIndex];
    RAB_LightInfo li;
    li.position     = gl.m_Position;
    li.direction    = normalize(gl.m_Direction);
    li.color        = gl.m_Color;
    li.intensity    = gl.m_Intensity;
    li.range        = gl.m_Range;
    li.spotInnerCos = cos(gl.m_SpotInnerConeAngle);
    li.spotOuterCos = cos(gl.m_SpotOuterConeAngle);
    li.lightType    = gl.m_Type;
    return li;
}

RAB_LightInfo RAB_LoadCompactLightInfo(uint linearIndex)
{
    return RAB_LoadLightInfo(linearIndex, false);
}

bool RAB_StoreCompactLightInfo(uint linearIndex, RAB_LightInfo lightInfo)
{
    return true;
}

float RAB_GetLightTargetPdfForVolume(RAB_LightInfo lightInfo, float3 volumeCenter, float volumeRadius)
{
    return 1.0 / (4.0 * PI);
}

bool RAB_IsLocalLight(RAB_LightInfo lightInfo)
{
    return lightInfo.lightType != 0;
}

float RAB_EvaluateLocalLightSourcePdf(uint lightIndex)
{
    uint total = g_RTXDIConst.m_LocalLightCount;
    return total > 0 ? 1.0 / float(total) : 0.0;
}

float RAB_GetLightSolidAnglePdf(uint lightIndex)
{
    return RAB_EvaluateLocalLightSourcePdf(lightIndex);
}

// ============================================================================
// RAB_SamplePolymorphicLight
// ============================================================================
RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo lightInfo, RAB_Surface surface, float2 uv)
{
    RAB_LightSample s = RAB_EmptyLightSample();

    if (lightInfo.lightType == 0) // Directional / sun
    {
        s.direction    = lightInfo.direction;
        s.distance     = 1e10;
        s.radiance     = lightInfo.color * lightInfo.intensity;
        s.solidAnglePdf = 1.0;
        s.position     = surface.worldPos + lightInfo.direction * 1e6;
    }
    else // Point (1) or Spot (2)
    {
        float3 toLight = lightInfo.position - surface.worldPos;
        float  dist    = length(toLight);
        if (dist < 1e-5) return s;

        float3 L = toLight / dist;
        s.direction = L;
        s.position  = lightInfo.position;
        s.distance  = dist;

        // Inverse-square attenuation with optional range falloff
        float attenuation = 1.0 / max(dist * dist, 0.0001);
        if (lightInfo.range > 0.0)
        {
            float t = saturate(1.0 - (dist / lightInfo.range));
            attenuation *= t * t;
        }

        // Spot cone falloff
        if (lightInfo.lightType == 2)
        {
            float cosTheta  = dot(-L, lightInfo.direction);
            float spotFactor = smoothstep(lightInfo.spotOuterCos, lightInfo.spotInnerCos, cosTheta);
            attenuation *= spotFactor;
        }

        s.radiance      = lightInfo.color * lightInfo.intensity * attenuation;
        s.solidAnglePdf = 1.0;
    }

    return s;
}

// ============================================================================
// BRDF evaluation
// ============================================================================

// Luminance helper (forward declaration — used in RAB_EvaluateBrdf below)
float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float3 RAB_EvaluateBrdf(RAB_Surface surface, float3 inDirection, float3 outDirection)
{
    float3 N  = surface.normal;
    float3 L  = inDirection;
    float3 V  = outDirection;
    float3 H  = normalize(V + L);

    float NdotL = max(0.0, dot(N, L));
    float NdotV = max(0.0, dot(N, V));
    float NdotH = max(0.0, dot(N, H));
    float VdotH = max(0.0, dot(V, H));

    float3 F0 = surface.material.specularF0;
    float3 F  = F_Schlick(F0, VdotH);
    float  D  = DistributionGGX(NdotH, surface.roughness);
    float  G  = GeometrySmith(NdotV, NdotL, surface.roughness);

    float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.0001);
    float  kD       = 1.0 - luminance(F0); // simplified metallic proxy
    float3 diffuse  = kD * surface.material.diffuseAlbedo / PI;

    return (diffuse + specular) * NdotL;
}

// ============================================================================
// Visibility testing
// ============================================================================
bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample lightSample)
{
    float3 origin    = surface.worldPos + surface.normal * 0.005;
    float3 direction = lightSample.direction;
    float  maxDist   = lightSample.distance - 0.01;

    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = direction;
    ray.TMin      = 0.001;
    ray.TMax      = max(maxDist, 0.01);

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
             RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> q;
    q.TraceRayInline(g_SceneAS, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();

    return q.CommittedStatus() == COMMITTED_NOTHING;
}

bool RAB_GetConservativeVisibility(RAB_Surface surface, float3 samplePosition)
{
    float3 toSample = samplePosition - surface.worldPos;
    float  dist     = length(toSample);
    RAB_LightSample ls = RAB_EmptyLightSample();
    ls.direction = toSample / max(dist, 1e-5);
    ls.distance  = dist;
    return RAB_GetConservativeVisibility(surface, ls);
}

bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface,
    RAB_LightSample lightSample)
{
    return RAB_GetConservativeVisibility(currentSurface, lightSample);
}

bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface,
    float3 samplePosition)
{
    return RAB_GetConservativeVisibility(currentSurface, samplePosition);
}

// ============================================================================
// Scene light counts & index translation
// ============================================================================
uint RAB_GetLightCount()     { return g_RTXDIConst.m_LightCount; }
int  RAB_GetLightIndexCount(){ return int(g_RTXDIConst.m_LightCount); }

int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious)
{
    return int(lightIndex); // Lights are stable in Phase 1
}

// ============================================================================
// Viewport helpers
// ============================================================================
float RAB_GetViewportWidth()  { return float(g_RTXDIConst.m_ViewportSize.x); }
float RAB_GetViewportHeight() { return float(g_RTXDIConst.m_ViewportSize.y); }

int2 RAB_ClampSamplePositionIntoView(int2 samplePosition, bool previousFrame)
{
    return clamp(samplePosition, int2(0, 0), int2(g_RTXDIConst.m_ViewportSize) - int2(1, 1));
}

// ============================================================================
// Neighbor validity
// ============================================================================
bool RAB_IsValidNeighborForResampling(RAB_Surface centerSurface, RAB_Surface neighborSurface,
    float maxDepthThreshold, float maxNormalThreshold)
{
    float depthDiff = abs(RAB_GetSurfaceLinearDepth(centerSurface) - RAB_GetSurfaceLinearDepth(neighborSurface));
    if (depthDiff > maxDepthThreshold * RAB_GetSurfaceLinearDepth(centerSurface))
        return false;
    float normalDot = dot(RAB_GetSurfaceNormal(centerSurface), RAB_GetSurfaceNormal(neighborSurface));
    if (normalDot < maxNormalThreshold)
        return false;
    return true;
}

float RAB_GetBoilingFilterStrength() { return 0.25; }

// ============================================================================
// Environment map stubs (no env map in Phase 1)
// ============================================================================
float2 RAB_GetEnvironmentMapRandXYFromDir(float3 direction)
{
    float2 uv;
    uv.x = atan2(direction.z, direction.x) / (2.0 * PI) + 0.5;
    uv.y = acos(direction.y) / PI;
    return uv;
}
float3 RAB_GetEnvironmentMapValueFromUV(float2 uv)            { return float3(0.0, 0.0, 0.0); }
float3 RAB_SampleEnvironmentMap(float2 uv)                    { return float3(0.0, 0.0, 0.0); }
float  RAB_EvaluateEnvironmentMapSamplingPdf(float3 direction) { return 0.0; }

// ============================================================================
// Ray tracing helper stubs
// ============================================================================
RAB_RayPayload RAB_EmptyRayPayload()
{
    RAB_RayPayload p;
    p.hitDistance = 0.0;
    p.throughput  = float3(1.0, 1.0, 1.0);
    return p;
}

RAB_RayPayload RAB_TraceRayForVisibility(float3 origin, float3 direction, float maxDistance)
{
    RAB_RayPayload p;
    p.throughput  = float3(1.0, 1.0, 1.0);
    p.hitDistance = maxDistance;
    return p;
}

bool RAB_TraceRayForLocalLight(float3 origin, float3 direction, float tMin, float tMax,
    out uint o_lightIndex, out float2 o_randXY)
{
    o_lightIndex = 0;
    o_randXY     = float2(0.5, 0.5);
    return false;
}

// ============================================================================
// Previous-frame surface helpers
// ============================================================================
RAB_Surface RAB_GetPreviousSurface(int2 pixelPosition, out float2 motionVector)
{
    motionVector = float2(0.0, 0.0);
    return RAB_GetGBufferSurface(pixelPosition, true);
}

float3 RAB_GetSurfaceWorldPosPrev(RAB_Surface surface) { return RAB_GetSurfaceWorldPos(surface); }

RAB_Surface RAB_CreateSurfaceFromGBuffer(int2 pixelPosition)
{
    return RAB_GetGBufferSurface(pixelPosition, false);
}

RAB_Surface RAB_CreateEmptySurfaceAtPosition(float3 worldPos, float3 normal)
{
    RAB_Surface s = RAB_EmptySurface();
    s.worldPos    = worldPos;
    s.normal      = normal;
    s.linearDepth = length(worldPos);
    return s;
}

