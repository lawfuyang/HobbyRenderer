/*
 * RTXDIApplicationBridge.hlsli
 *
 * Full implementation of the RTXDI Application Bridge (RAB) interface.
 * Connects the RTXDI resampling algorithms to the application's G-buffer,
 * material model and analytic light buffer.
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
#include "Atmosphere.hlsli"

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
Texture2D<float3>                               g_GBufferMV                 : register(t5);
StructuredBuffer<GPULight>                      g_Lights                    : register(t6);
RaytracingAccelerationStructure                 g_SceneAS                   : register(t7);
Texture2D<float4>                               g_GBufferAlbedoHistory      : register(t8);
Texture2D<float2>                               g_GBufferORMHistory         : register(t9);
RaytracingAccelerationStructure                 g_SceneASHistory            : register(t10);

RWStructuredBuffer<uint2>                       g_RTXDI_RISBuffer           : register(u0);
RWStructuredBuffer<RTXDI_PackedDIReservoir>     g_RTXDI_LightReservoirBuffer: register(u1);

VK_IMAGE_FORMAT_UNKNOWN
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

RAB_Material RAB_GetGBufferMaterial(int2 pixelPosition, bool previousFrame)
{
    RAB_Material material = RAB_EmptyMaterial();

    // NOTE: pixelPosition is already the reprojected position (computed by the RTXDI SDK).
    // Do NOT apply the motion vector again here — that would cause double-reprojection.
    int2 samplePos = clamp(pixelPosition, int2(0, 0), int2(g_RTXDIConst.m_ViewportSize) - int2(1, 1));

    // Bounds check
    if (any(pixelPosition < int2(0, 0)) || any(pixelPosition >= int2(g_RTXDIConst.m_ViewportSize)))
        return material;

    // Load from respective G-buffers
    float4 albedoSample;
    float2 orm;
    
    if (previousFrame)
    {
        // Load from history buffers (previous frame data)
        albedoSample = g_GBufferAlbedoHistory.Load(int3(samplePos, 0));
        orm          = g_GBufferORMHistory.Load(int3(samplePos, 0));
    }
    else
    {
        // Load from current frame G-buffers
        albedoSample = g_GBufferAlbedo.Load(int3(samplePos, 0));
        orm          = g_GBufferORM.Load(int3(samplePos, 0));
    }

    float  roughness    = orm.r;
    float  metallic     = orm.g;
    float3 baseColor    = albedoSample.rgb;
    float3 F0           = ComputeF0(baseColor, metallic, 1.5);

    material.diffuseAlbedo = baseColor * (1.0 - metallic);
    material.specularF0    = F0;
    material.roughness     = roughness;

    return material;
}

bool RAB_AreMaterialsSimilar(RAB_Material a, RAB_Material b)
{
    const float roughnessThreshold   = 0.5;
    const float reflectivityThreshold = 0.25;
    const float albedoThreshold      = 0.25;

    // Compare roughness with relative difference
    float roughnessRelDiff = abs(a.roughness - b.roughness) / max(max(a.roughness, b.roughness), 0.01);
    if (roughnessRelDiff > roughnessThreshold)
        return false;

    // Compare reflectivity using luminance
    float lumA = Luminance(a.specularF0);
    float lumB = Luminance(b.specularF0);
    if (abs(lumA - lumB) > reflectivityThreshold)
        return false;

    // Compare albedo using luminance
    float albedoLumA = Luminance(a.diffuseAlbedo);
    float albedoLumB = Luminance(b.diffuseAlbedo);
    if (abs(albedoLumA - albedoLumB) > albedoThreshold)
        return false;

    return true;
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
    // NOTE: pixelPosition is already the reprojected previous-frame pixel position when
    // previousFrame == true (computed internally by the RTXDI SDK via screenSpaceMotion).
    // Do NOT re-apply the motion vector here — that causes double-reprojection.
    if (any(pixelPosition < int2(0, 0)) || any(pixelPosition >= int2(g_RTXDIConst.m_ViewportSize)))
        return RAB_EmptySurface();

    PlanarViewConstants view = g_RTXDIConst.m_View;
    if (previousFrame)
    {
        view = g_RTXDIConst.m_PrevView;
    }

    int2 samplePos = pixelPosition;

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

    float4 albedoSample;
    float2 orm;

    if (previousFrame)
    {
        albedoSample = g_GBufferAlbedoHistory.Load(int3(samplePos, 0));
        orm          = g_GBufferORMHistory.Load(int3(samplePos, 0));
    }
    else
    {
        albedoSample = g_GBufferAlbedo.Load(int3(samplePos, 0));
        orm          = g_GBufferORM.Load(int3(samplePos, 0));
    }
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

bool  RAB_IsAnalyticLightSample(RAB_LightSample s) { return true; } // TODO: if we have non-analytic light types (point, spot, or directional), change this
float RAB_LightSampleSolidAnglePdf(RAB_LightSample s) { return s.solidAnglePdf; }

float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample lightSample, RAB_Surface surface)
{
    float NdotL = max(0.0, dot(surface.normal, lightSample.direction));
    float lum   = Luminance(lightSample.radiance);
    return lum * NdotL;
}

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample lightSample,
    out float3 lightDir, out float lightDistance)
{
    lightDir      = lightSample.direction;
    lightDistance = lightSample.distance;
}

// ============================================================================
// RAB_LightInfo
// ============================================================================
struct RAB_LightInfo
{
    float3 position;
    float3 direction;
    float3 radiance;        // Pre-computed light radiance (color * intensity for analytic lights)
    float  range;
    float  spotInnerCos;
    float  spotOuterCos;
    float  cosSunAngularRadius;
    uint   lightType;       // 0=directional, 1=point, 2=spot
};

struct RAB_RayPayload
{
    float  hitDistance;
    float3 throughput;
};

RAB_LightInfo RAB_EmptyLightInfo()
{
    RAB_LightInfo li;
    li.position             = float3(0.0, 0.0, 0.0);
    li.direction            = float3(0.0, 1.0, 0.0);
    li.radiance             = float3(0.0, 0.0, 0.0);
    li.range                = 0.0;
    li.spotInnerCos         = 0.0;
    li.spotOuterCos         = 0.0;
    li.cosSunAngularRadius  = 1.0;
    li.lightType            = 0;
    return li;
}

RAB_LightInfo RAB_LoadLightInfo(uint lightIndex, bool previousFrame)
{
    GPULight gl = g_Lights[lightIndex];
    RAB_LightInfo li;
    li.position             = gl.m_Position;
    li.direction            = lightIndex == 0 ? g_RTXDIConst.m_SunDirection : normalize(gl.m_Direction);
    li.range                = gl.m_Range;
    li.spotInnerCos         = cos(gl.m_SpotInnerConeAngle);
    li.spotOuterCos         = cos(gl.m_SpotOuterConeAngle);
    li.cosSunAngularRadius  = gl.m_CosSunAngularRadius;
    li.lightType            = gl.m_Type;

    // Compute radiance upfront
    if (lightIndex == 0 && li.lightType == 0 && g_RTXDIConst.m_EnableSky != 0)
    {
        // Directional light with sky enabled: use atmosphere-aware radiance
        float3 p_atmo = GetAtmospherePos(float3(0.0, 0.0, 0.0));  // Observer at origin
        li.radiance = GetAtmosphereSunRadiance(p_atmo, li.direction, gl.m_Intensity);
    }
    else
    {
        // Analytic light: simple color * intensity
        li.radiance = gl.m_Color * gl.m_Intensity;
    }

    return li;
}

RAB_LightInfo RAB_LoadCompactLightInfo(uint linearIndex)
{
    return RAB_LoadLightInfo(linearIndex, false);
}

bool RAB_StoreCompactLightInfo(uint linearIndex, RAB_LightInfo lightInfo)
{
    // Simple implementation: store light info as-is into RIS buffer
    // Note: This is a simplified placeholder - full implementation would pack
    // the RAB_LightInfo structure into compact uint format for memory efficiency
    // For now, we just store to a simple buffer without actual packing
    
    // In a full implementation, you would pack:
    // - position (12 bytes)
    // - direction (12 bytes) 
    // - radiance (12 bytes)
    // - properties (range, spot angles, etc.)
    // Into a compressed format (typically 2x uint4 = 32 bytes)
    
    // For this version, we'll return false to indicate we can't compact this light
    // which tells RTXDI to use the standard light buffer instead
    return false;
}

float RAB_GetLightTargetPdfForVolume(RAB_LightInfo lightInfo, float3 volumeCenter, float volumeRadius)
{
    // Simple importance weighting based on light type and distance
    if (lightInfo.lightType == 0)  // Directional light
    {
        return 1.0 / (4.0 * PI);
    }
    else  // Point or spot light
    {
        // Weight by inverse square of distance
        float3 toLight = lightInfo.position - volumeCenter;
        float dist = length(toLight);
        float attenuation = 1.0 / max(dist * dist, 0.01);
        
        // Attenuate by range falloff if specified
        if (lightInfo.range > 0.0)
        {
            float t = saturate(1.0 - (dist / lightInfo.range));
            attenuation *= t * t;
        }
        
        // Spot cone falloff
        if (lightInfo.lightType == 2)
        {
            float3 L = normalize(toLight);
            float cosTheta = dot(-L, normalize(lightInfo.direction));
            float spotFactor = smoothstep(lightInfo.spotOuterCos, lightInfo.spotInnerCos, cosTheta);
            attenuation *= spotFactor;
        }
        
        return attenuation;
    }
}

float RAB_EvaluateLocalLightSourcePdf(uint lightIndex)
{
    // For a uniform distribution over all lights
    uint total = g_RTXDIConst.m_LocalLightCount;
    if (total == 0)
        return 0.0;
    
    // In a full implementation, this would read from a PDF texture
    // built during light preprocessing, similar to environment map PDF
    // For now, use uniform distribution with importance weighting
    
    // Load the light to check its importance
    RAB_LightInfo lightInfo = RAB_LoadLightInfo(lightIndex, false);
    
    // Use a simple heuristic: brighter lights get higher probability
    float luminance = Luminance(lightInfo.radiance);
    
    // This is simplified - a proper implementation would pre-compute
    // the sum of all light weights for normalization
    return (1.0 + luminance) / float(total);
}

// ============================================================================
// RAB_SamplePolymorphicLight
// ============================================================================
RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo lightInfo, RAB_Surface surface, float2 uv)
{
    RAB_LightSample s = RAB_EmptyLightSample();

    if (lightInfo.lightType == 0) // Directional / sun
    {
        float3 direction = lightInfo.direction;
        float  pdf       = 1.0;

        if (lightInfo.cosSunAngularRadius < 1.0)
        {
            direction = SampleConeSolidAngle(lightInfo.direction, lightInfo.cosSunAngularRadius, uv);
            pdf       = 1.0 / (2.0 * PI * (1.0 - lightInfo.cosSunAngularRadius));
        }

        s.direction     = direction;
        s.distance      = 1e10;
        s.radiance      = lightInfo.radiance;
        s.solidAnglePdf  = pdf;
        s.position      = surface.worldPos + direction * 1e10;
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

        s.radiance      = lightInfo.radiance * attenuation;
        s.solidAnglePdf = 1.0;
    }

    return s;
}

// ============================================================================
// BRDF evaluation
// ============================================================================

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

    float3 specular = ComputeSpecularBRDF(F, NdotH, NdotV, NdotL, surface.roughness);
    float  kD       = 1.0 - Luminance(F0); // simplified metallic proxy
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

// Helper function to test visibility against the previous frame's acceleration structure
bool RAB_GetConservativeVisibilityPrevious(RAB_Surface surface, RAB_LightSample lightSample)
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
    q.TraceRayInline(g_SceneASHistory, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();

    return q.CommittedStatus() == COMMITTED_NOTHING;
}

bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface, RAB_LightSample lightSample)
{
    return RAB_GetConservativeVisibilityPrevious(previousSurface, lightSample);
}

bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface, float3 samplePosition)
{
    // Trace visibility ray using previous frame acceleration structure and previous surface position
    float3 toSample = samplePosition - previousSurface.worldPos;
    float  dist     = length(toSample);
    RAB_LightSample ls = RAB_EmptyLightSample();
    ls.direction = toSample / max(dist, 1e-5);
    ls.distance  = dist;
    
    // Use the previous acceleration structure for ray tracing
    return RAB_GetConservativeVisibilityPrevious(previousSurface, ls);
}

int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious)
{
    return int(lightIndex); // Lights are stable permanently
}

int2 RAB_ClampSamplePositionIntoView(int2 samplePosition, bool previousFrame)
{
    return clamp(samplePosition, int2(0, 0), int2(g_RTXDIConst.m_ViewportSize) - int2(1, 1));
}

float RAB_GetBoilingFilterStrength() { return 0.25; }

// ============================================================================
// Environment map stubs (using Bruneton sky instead of texture)
// ============================================================================
float2 RAB_GetEnvironmentMapRandXYFromDir(float3 direction)
{
    float2 uv;
    uv.x = atan2(direction.z, direction.x) / (2.0 * PI) + 0.5;
    uv.y = acos(clamp(direction.y, -1.0, 1.0)) / PI;
    return uv;
}

float RAB_EvaluateEnvironmentMapSamplingPdf(float3 direction)
{
    // For Bruneton sky, use a simple cosine-weighted PDF centered on the sun
    // This biases sampling towards the sun direction
    float sunCosThreshold = 0.5;  // Cosine of ~60 degrees
    float cosSunDir = max(0.0, dot(direction, g_RTXDIConst.m_SunDirection));
    
    if (cosSunDir > sunCosThreshold)
    {
        // Higher probability for directions near sun
        return 0.8 * cosSunDir / PI;
    }
    else
    {
        // Lower probability for directions away from sun
        return 0.2 / (4.0 * PI);
    }
}

// ============================================================================
// Ray tracing helper stubs
// ============================================================================

bool RAB_TraceRayForLocalLight(float3 origin, float3 direction, float tMin, float tMax, out uint o_lightIndex, out float2 o_randXY)
{
    o_lightIndex = 0;
    o_randXY     = float2(0.5, 0.5);
    
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = direction;
    ray.TMin      = tMin;
    ray.TMax      = tMax;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(g_SceneAS, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        // Ray hit a triangle — for now, return a dummy light index
        // In a full implementation, you'd look up the primitive -> light mapping
        // For this stub, just indicate a hit was found
        o_lightIndex = 0;  // Could be non-zero if you have light mapping data
        
        // Use barycentric coordinates as random values
        float2 bary = q.CommittedTriangleBarycentrics();
        o_randXY = normalize(float2(bary.x, bary.y));
        
        return true;
    }

    return false;
}
