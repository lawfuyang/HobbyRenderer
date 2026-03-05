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

// Enable presampling: allows Power-RIS tile-based light selection and compact
// light info caching in the RIS buffer for improved GPU cache coherence.
#define RTXDI_ENABLE_PRESAMPLING 1

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

Buffer<float2>                                  g_RTXDI_NeighborOffsets     : register(t0);
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
Texture2D                                       g_RTXDI_LocalLightPDFTexture: register(t11);
Texture2D<float>                                g_DepthHistory              : register(t17);
Texture2D<float2>                               g_GBufferNormalsHistory     : register(t18);
Texture2D                                       g_RTXDI_EnvLightPDFTexture  : register(t19); // environment-light PDF mip chain (Texture2D = float4 default, matches RTXDI_TEX2D)

// Scene geometry/material buffers — used by GetFinalVisibility for alpha testing.
StructuredBuffer<PerInstanceData>               g_RTXDI_Instances           : register(t12);
StructuredBuffer<MaterialConstants>             g_RTXDI_Materials           : register(t13);
StructuredBuffer<VertexQuantized>               g_RTXDI_Vertices            : register(t14);
StructuredBuffer<MeshData>                      g_RTXDI_MeshData            : register(t15);
StructuredBuffer<uint>                          g_RTXDI_Indices             : register(t16);

RWStructuredBuffer<uint2>                       g_RTXDI_RISBuffer           : register(u0);
RWStructuredBuffer<RTXDI_PackedDIReservoir>     g_RTXDI_LightReservoirBuffer: register(u1);

VK_IMAGE_FORMAT_UNKNOWN
RWTexture2D<float4>                             g_RTXDIDIOutput             : register(u2);

// Compact light data buffer: 3 × uint4 per RIS tile entry.
//   slot 0 (uint4): position.xyz (f32), lightType (uint)
//   slot 1 (uint4): radiance.xyz (f32), rangeLike (f32)  -- range for point/spot, cosSunAngularRadius for directional
//   slot 2 (uint4): direction.xyz (f32), packHalf2x16(spotInnerCos, spotOuterCos)
RWStructuredBuffer<uint4>                       g_RTXDI_RISLightDataBuffer  : register(u3);

// ---- RELAX denoising outputs (u5/u6/u7 — bound only when RTXDI_ENABLE_RELAX_DENOISING=1) ---------
// u4 is reserved for g_PDFMip0 (BuildLocalLightPDF pass) so we start at u5.
#if RTXDI_ENABLE_RELAX_DENOISING
VK_IMAGE_FORMAT_UNKNOWN
RWTexture2D<float4> g_RTXDIDiffuseOutput  : register(u5); // RELAX IN/OUT_DIFF_RADIANCE_HITDIST
VK_IMAGE_FORMAT_UNKNOWN
RWTexture2D<float4> g_RTXDISpecularOutput : register(u6); // RELAX IN/OUT_SPEC_RADIANCE_HITDIST
RWTexture2D<float>  g_RTXDILinearDepth    : register(u7); // RELAX IN_VIEWZ (written by GenerateViewZ pass)
#endif

// Hook up the RTXDI SDK macro names to our resources
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER   g_RTXDI_NeighborOffsets
#define RTXDI_RIS_BUFFER                g_RTXDI_RISBuffer
#define RTXDI_LIGHT_RESERVOIR_BUFFER    g_RTXDI_LightReservoirBuffer

float2 directionToEquirectUV(float3 normalizedDirection)
{
    float elevation = asin(normalizedDirection.y);
    float azimuth = 0;
    if (abs(normalizedDirection.y) < 1.0)
        azimuth = atan2(normalizedDirection.z, normalizedDirection.x);

    float2 uv;
    uv.x = azimuth / (2.0 * PI) - 0.25;
    uv.y = 0.5 - elevation / PI;

    return uv;
}

float3 equirectUVToDirection(float2 uv, out float cosElevation)
{
    float azimuth = (uv.x + 0.25) * (2.0 * PI);
    float elevation = (0.5 - uv.y) * PI;
    cosElevation = cos(elevation);

    return float3(
        cos(azimuth) * cosElevation,
        sin(elevation),
        sin(azimuth) * cosElevation
    );
}

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

    // Load depth from the correct frame's depth buffer.
    // For previousFrame=true, use the history depth so world-position reconstruction
    // uses last frame's view matrix with last frame's depth — not the current frame's depth.
    float depth = previousFrame
        ? g_DepthHistory.Load(int3(samplePos, 0))
        : g_Depth.Load(int3(samplePos, 0));

    // Sky / background pixel — no surface (reverse-Z: 0.0 = far plane)
    if (depth == 0.0)
        return RAB_EmptySurface();

    RAB_Surface s;
    s.worldPos    = ReconstructWorldPos(uint2(samplePos), depth, view);
    // Load normals from the correct frame's buffer.
    s.normal      = previousFrame
        ? DecodeNormal(g_GBufferNormalsHistory.Load(int3(samplePos, 0)))
        : DecodeNormal(g_GBufferNormals.Load(int3(samplePos, 0)));

    // Camera world position: 4th row of ViewToWorld matrix (row-vector convention).
    float3 camPos = float3(
        view.m_MatViewToWorld._41,
        view.m_MatViewToWorld._42,
        view.m_MatViewToWorld._43);

    // Camera forward direction: 3rd row of ViewToWorld (points into the scene for LH DirectX).
    float3 camForward = float3(
        view.m_MatViewToWorld._31,
        view.m_MatViewToWorld._32,
        view.m_MatViewToWorld._33);

    // Use view-space depth (projection of worldPos onto camera forward) so that
    // linearDepth is consistent with screenSpaceMotion.z (which is prevClipW - curClipW,
    // i.e., the change in view-space Z).
    s.linearDepth = dot(s.worldPos - camPos, camForward);
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
    // Guard: zero solidAnglePdf means an invalid/degenerate sample — reject it.
    if (lightSample.solidAnglePdf <= 0.0)
        return 0.0;

    // Target pdf = reflected luminance / solidAnglePdf.
    // This matches the FullSample: RAB_GetReflectedLuminanceForSurface / lightSample.solidAnglePdf.
    // The solidAnglePdf (sr^-1) normalises the importance weight so that all light types
    // (local, directional, environment) are on the same scale in the reservoir.
    float3 N = surface.normal;
    float3 V = surface.viewDir;
    float3 L = normalize(lightSample.position - surface.worldPos);

    float NdotL = max(0.0, dot(N, L));
    if (NdotL <= 0.0)
        return 0.0;

    float3 H    = normalize(V + L);
    float NdotH = max(0.0, dot(N, H));
    float NdotV = max(0.0, dot(N, V));
    float VdotH = max(0.0, dot(V, H));
    float3 F0 = surface.material.specularF0;
    float3 F  = F_Schlick(F0, VdotH);
    float  kD = 1.0 - Luminance(F0); // simplified metallic proxy

    // Use DisneyBurleyDiffuse to match the actual BRDF used in shading.
    // DisneyBurleyDiffuse already includes NdotL / PI.
    float  burley   = DisneyBurleyDiffuse(NdotL, NdotV, dot(L, H), surface.roughness);
    float3 diffuse  = kD * surface.material.diffuseAlbedo * burley;
    float3 specular = ComputeSpecularBRDF(F, NdotH, NdotV, NdotL, surface.roughness) * NdotL;
    float3 reflectedRadiance = lightSample.radiance * (diffuse + specular);
    return Luminance(reflectedRadiance) / lightSample.solidAnglePdf;
}

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample lightSample,
    out float3 lightDir, out float lightDistance)
{
    lightDir      = lightSample.direction;
    lightDistance = lightSample.distance;
}

// ============================================================================
// Environment map helpers (Bruneton procedural sky → equirectangular PDF)
// ============================================================================

// Convert world direction → equirectangular [0,1]² UV
float2 RAB_GetEnvironmentMapRandXYFromDir(float3 direction)
{
    float elevation = asin(direction.y);
    float azimuth = 0;
    if (abs(direction.y) < 1.0)
        azimuth = atan2(direction.z, direction.x);

    float2 uv;
    uv.x = azimuth / (2.0 * PI) - 0.25;
    uv.y = 0.5 - elevation / PI;
    return frac(uv); // stay in [0,1]
}

// Evaluate the PDF of sampling direction `L` from the env RIS segment.
// Returns the DISCRETE probability (dimensionless, in [0,1]) of selecting this texel.
// This matches the FullSample: texelValue / sum.
// NOTE: RAB_SamplePolymorphicLight for env lights uses the SOLID-ANGLE PDF
// (texels per steradian = W*H / (2*pi*pi*cosElevation)), which is a different quantity.
// RAB_EvaluateEnvironmentMapSamplingPdf is only used by the RTXDI SDK internally
// for MIS weight computation in RTXDI_SampleLightsForSurface.
float RAB_EvaluateEnvironmentMapSamplingPdf(float3 L)
{
    if (g_RTXDIConst.m_EnvLightPresent == 0u)
        return 1.0;

    float2 uv = RAB_GetEnvironmentMapRandXYFromDir(L);
    uint2  pdfSize     = g_RTXDIConst.m_EnvPDFTextureSize;
    uint2  texelPos    = uint2(float2(pdfSize) * uv);
    float  texelValue  = g_RTXDI_EnvLightPDFTexture[texelPos].r;

    // The last mip is 1×1 and holds the average of all mip-0 texels (padded to square).
    int lastMip = max(0, int(floor(log2(float(max(pdfSize.x, pdfSize.y))))));
    float averageValue = g_RTXDI_EnvLightPDFTexture.mips[lastMip][uint2(0, 0)].r;

    // Sum of all texels in the square PDF texture (same formula as FullSample)
    float squareSide = float(1u << uint(lastMip));
    float sum   = averageValue * squareSide * squareSide;

    if (sum <= 0.0)
        return 1.0;

    return texelValue / sum;
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
    // Virtual env-light index: not backed by a real GPULight entry
    if (g_RTXDIConst.m_EnvLightPresent != 0u && lightIndex == g_RTXDIConst.m_EnvLightIndex)
    {
        RAB_LightInfo li = RAB_EmptyLightInfo();
        li.lightType = 3u; // Environment
        li.radiance  = float3(1.0, 1.0, 1.0); // Will be evaluated per-UV in SamplePolymorphicLight
        return li;
    }

    GPULight gl = g_Lights[lightIndex];
    RAB_LightInfo li;
    li.position             = gl.m_Position;

    // Direction convention note:
    // gl.m_Direction is derived from QuaternionToDirection(-Z), i.e. the emission
    // direction pointing FROM the light INTO the scene.
    // g_RTXDIConst.m_SunDirection is derived from Scene::Update() which transforms
    // the +Z axis — giving the TOWARD-SUN direction (same as inputs.sunDirection in
    // the deferred path). For light index 0 (the directional sun light) we use
    // m_SunDirection so the direction convention matches the deferred path.
    // For other directional lights gl.m_Direction points away from the sun,
    // which is the physically correct emission direction for RAB_SamplePolymorphicLight.
    li.direction            = (lightIndex == 0 && gl.m_Type == 0u)
                            ? g_RTXDIConst.m_SunDirection
                            : normalize(gl.m_Direction);
    li.range                = gl.m_Range;
    li.spotInnerCos         = cos(gl.m_SpotInnerConeAngle);
    li.spotOuterCos         = cos(gl.m_SpotOuterConeAngle);
    li.cosSunAngularRadius  = gl.m_CosSunAngularRadius;
    li.lightType            = gl.m_Type;

    // Compute radiance upfront
    // RTXDI's shading formula: finalContrib = lightSample.radiance * invPdf / solidAnglePdf
    // After RIS normalisation for a single infinite light: invPdf ≈ numInfiniteLights = 1,
    // so  finalContrib = radiance * solidAngle.
    // For the result to equal irradiance (the quantity the scene stores in color*intensity),
    // we need: li.radiance = irradiance / solidAngle  (W/m²/sr).
    // This matches the FullSample's ConvertLight: radiance = color * irradiance / solidAngle.
    if (li.lightType == 0u) // Directional sun light (type 0 — infinite/directional)
    {
        // Sun solid angle = 2π(1 - cos(halfAngle))
        float solidAngle = 2.0 * PI * (1.0 - li.cosSunAngularRadius);

        if (lightIndex == 0 && g_RTXDIConst.m_EnableSky != 0)
        {
            // Sky enabled: use atmosphere-computed irradiance.
            // GetAtmosphereSunRadiance returns solar IRRADIANCE (W/m²): solar_irradiance * transmittance * intensity
            float3 p_atmo = GetAtmospherePos(float3(0.0, 0.0, 0.0));
            float3 solarIrradiance = GetAtmosphereSunRadiance(p_atmo, li.direction, gl.m_Intensity);
            // Guard against degenerate (point-like) sun with zero solid angle
            li.radiance = (solidAngle > 1e-10) ? (solarIrradiance / solidAngle) : solarIrradiance;
        }
        else
        {
            // Sky disabled: use scene light's color * intensity as irradiance (W/m²) and
            // convert to per-steradian radiance (W/m²/sr) by dividing by solid angle —
            // exactly what FullSample's ConvertLight does for DirectionalLight.
            // Without this division the sun contribution is multiplied by solidAngle in shading
            // (≈ 6.5e-5 sr for a realistic sun disk) and appears black.
            li.radiance = (solidAngle > 1e-10) ? (gl.m_Color * gl.m_Intensity / solidAngle)
                                               : (gl.m_Color * gl.m_Intensity);
        }
    }
    else
    {
        // Point / spot lights: color * intensity is already in the units expected by
        // RAB_SamplePolymorphicLight (attenuation and 1/r² are applied there).
        li.radiance = gl.m_Color * gl.m_Intensity;
    }

    return li;
}

// ============================================================================
// Compact light info pack/unpack
// Layout: 3 × uint4 per RIS entry, indexed as linearIndex * 3 + {0,1,2}
//
//   slot0.xyz = position (f32)          slot0.w = lightType (uint)
//   slot1.xyz = radiance (f32)          slot1.w = rangeLike (f32)
//                                         rangeLike = range        for point/spot (type 1/2)
//                                         rangeLike = cosSunAngRad for directional (type 0)
//   slot2.xyz = direction (f32)         slot2.w = packHalf2x16(spotInnerCos, spotOuterCos)
// ============================================================================

bool RAB_StoreCompactLightInfo(uint linearIndex, RAB_LightInfo lightInfo)
{
    uint base = linearIndex * 3u;

    // Slot 0: position + lightType
    uint4 slot0;
    slot0.x = asuint(lightInfo.position.x);
    slot0.y = asuint(lightInfo.position.y);
    slot0.z = asuint(lightInfo.position.z);
    slot0.w = lightInfo.lightType;
    g_RTXDI_RISLightDataBuffer[base + 0u] = slot0;

    // Slot 1: radiance + rangeLike (type-multiplexed)
    float rangeLike = (lightInfo.lightType == 0u)
                    ? lightInfo.cosSunAngularRadius   // directional: store sun disk param
                    : lightInfo.range;                // point/spot: store range
    uint4 slot1;
    slot1.x = asuint(lightInfo.radiance.x);
    slot1.y = asuint(lightInfo.radiance.y);
    slot1.z = asuint(lightInfo.radiance.z);
    slot1.w = asuint(rangeLike);
    g_RTXDI_RISLightDataBuffer[base + 1u] = slot1;

    // Slot 2: direction + packed spot cone cosines (f16 x2)
    uint4 slot2;
    slot2.x = asuint(lightInfo.direction.x);
    slot2.y = asuint(lightInfo.direction.y);
    slot2.z = asuint(lightInfo.direction.z);
    slot2.w = f32tof16(lightInfo.spotInnerCos) | (f32tof16(lightInfo.spotOuterCos) << 16u);
    g_RTXDI_RISLightDataBuffer[base + 2u] = slot2;

    return true;  // all analytic light types can always be compacted
}

RAB_LightInfo RAB_LoadCompactLightInfo(uint linearIndex)
{
    uint base = linearIndex * 3u;

    uint4 slot0 = g_RTXDI_RISLightDataBuffer[base + 0u];
    uint4 slot1 = g_RTXDI_RISLightDataBuffer[base + 1u];
    uint4 slot2 = g_RTXDI_RISLightDataBuffer[base + 2u];

    RAB_LightInfo li;
    li.position    = float3(asfloat(slot0.x), asfloat(slot0.y), asfloat(slot0.z));
    li.lightType   = slot0.w;
    li.radiance    = float3(asfloat(slot1.x), asfloat(slot1.y), asfloat(slot1.z));
    li.direction   = float3(asfloat(slot2.x), asfloat(slot2.y), asfloat(slot2.z));

    float rangeLike           = asfloat(slot1.w);
    li.range                  = (li.lightType != 0u) ? rangeLike : 0.0;
    li.cosSunAngularRadius    = (li.lightType == 0u) ? rangeLike : 1.0;

    float2 spotCosines  = float2(f16tof32(slot2.w & 0xFFFFu), f16tof32(slot2.w >> 16u));
    li.spotInnerCos     = spotCosines.x;
    li.spotOuterCos     = spotCosines.y;

    return li;
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
    // If environment sampling is enabled, we only want sky and emissive lights.
    // Local lights (point, spot, etc.) from the global light buffer are filtered out.
    if (g_RTXDIConst.m_EnvSamplingMode != 0u)
        return 0.0;

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

// Samples a polymorphic light relative to the given receiver surface.
// For most light types, the "uv" parameter is just a pair of uniform random numbers, originally
// produced by the RAB_GetNextRandom function and then stored in light reservoirs.
// For importance sampled environment lights, the "uv" parameter has the texture coordinates
// in the PDF texture, normalized to the (0..1) range.
RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo lightInfo, RAB_Surface surface, float2 uv)
{
    RAB_LightSample s = RAB_EmptyLightSample();

    if (lightInfo.lightType == 3u) // Environment (Bruneton procedural sky)
    {
        float2 directionUV = uv;
        
        float cosElevation;
        float3 dir = equirectUVToDirection(directionUV, cosElevation);

        // Radiance WITHOUT the sun disk — the sun is already handled as a separate
        // infinite light (index 0). Including the sun disk here would:
        //   1. Double-count the sun contribution.
        //   2. Cause extreme firefly spikes: sun disk radiance ~22,000 W/m²/sr vs
        //      sky dome ~1-10 W/m²/sr, so any RIS sample landing on the disk
        //      produces a reservoir weight thousands of times too large.
        float3 sampleRadiance = GetAtmosphereSkyRadiance(float3(0.0, 0.0, 0.0), dir, g_RTXDIConst.m_SunDirection, g_RTXDIConst.m_SunIntensity, false);

        // Solid-angle PDF: inverse of the solid angle of one texel in the equirectangular map.
        // This matches FullSample's EnvironmentLight::calcSample (importanceSampled branch):
        //   solidAnglePdf = (W * H) / (2 * pi * pi * cosElevation)
        // Units: sr^-1 (inverse steradians). Must match what RAB_GetLightSampleTargetPdfForSurface divides by.
        uint2 pdfSize = g_RTXDIConst.m_EnvPDFTextureSize;
        s.solidAnglePdf = float(pdfSize.x * pdfSize.y) / (2.0 * PI * PI * max(cosElevation, 1e-4));

        s.direction = dir;
        s.position  = surface.worldPos + dir * 10000.0; // DISTANT_LIGHT_DISTANCE
        s.radiance  = sampleRadiance;
        return s;
    }
    else if (lightInfo.lightType == 0) // Directional / sun
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
        s.position = lightInfo.position;
        s.direction = float3(0.0, 0.0, 1.0);
        s.distance = 1e-5;
        s.radiance = lightInfo.radiance;
        s.solidAnglePdf = 1.0;

        float3 toLight = lightInfo.position - surface.worldPos;
        float  dist    = length(toLight);
        if (dist < 1e-5) return s;

        float3 L = toLight / dist;
        s.direction = L;
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
            float cosTheta  = dot(L, lightInfo.direction);
            float spotFactor = smoothstep(lightInfo.spotOuterCos, lightInfo.spotInnerCos, cosTheta);
            attenuation *= spotFactor;
        }

        s.radiance = lightInfo.radiance * attenuation;
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
    float  burley   = DisneyBurleyDiffuse(NdotL, NdotV, VdotH, surface.roughness); // VdotH == LdotH when H = normalize(V+L)
    float3 diffuse  = kD * surface.material.diffuseAlbedo * burley;

    return (diffuse + specular * NdotL);
}

// ---- Separated BRDF components for NRD denoising -----------------------------------------------
// These mirror RAB_EvaluateBrdf but return only the diffuse or specular lobe, respectively.
// Both include the NdotL cosine factor (matching RAB_EvaluateBrdf convention).

float3 RAB_EvaluateBrdfDiffuseOnly(RAB_Surface surface, float3 L)
{
    // Demodulated diffuse for NRD RELAX: Disney Burley term only, no albedo or kD.
    // Matches the DisneyBurleyDiffuse used in the non-RTXDI deferred lighting path.
    // The compositing pass re-modulates by multiplying with diffuseAlbedo.
    float3 N    = surface.normal;
    float3 V    = surface.viewDir;
    float3 H    = normalize(V + L);
    float NdotL = max(0.0, dot(N, L));
    float NdotV = max(0.0, dot(N, V));
    float LdotH = max(0.0, dot(L, H));
    float burley = DisneyBurleyDiffuse(NdotL, NdotV, LdotH, surface.roughness);
    return float3(burley, burley, burley);
}

float3 RAB_EvaluateBrdfSpecularOnly(RAB_Surface surface, float3 L, float3 V)
{
    // Demodulated specular for NRD RELAX: divide out specularF0 so the denoiser
    // sees a signal normalised to [0,1] range. The compositing pass re-modulates
    // by multiplying with specularF0.
    float3 N  = surface.normal;
    float3 H  = normalize(V + L);
    float NdotL = max(0.0, dot(N, L));
    float NdotV = max(0.0, dot(N, V));
    float NdotH = max(0.0, dot(N, H));
    float VdotH = max(0.0, dot(V, H));
    float3 F0 = surface.material.specularF0;
    float3 F  = F_Schlick(F0, VdotH);
    float3 specular = ComputeSpecularBRDF(F, NdotH, NdotV, NdotL, surface.roughness) * NdotL;
    // Demodulate: divide by max(F0, 0.01) per-channel to avoid division by zero on black metals.
    return specular / max(F0, float3(0.01, 0.01, 0.01));
}

// ============================================================================
// Visibility testing
// ============================================================================

// Build a shadow ray from originWorldPos toward samplePosition.
// Matches FullSample's setupVisibilityRay: NO normal bias, just a small TMin
// to avoid self-intersection. Using a large normal bias (e.g. N*0.1) causes:
//   - Decals (coplanar geometry) to appear overly bright: the bias pushes the
//     origin past the background surface, which then falls within TMin and is
//     skipped, so the decal always sees the sun unobstructed.
//   - Shadows to disappear for geometry closer than the bias distance.
// The 'offset' parameter is the TMin (and half the TMax shrink):
//   - Conservative visibility: 0.001 (default)
//   - Final shading visibility: 0.01
RayDesc SetupShadowRay(float3 originWorldPos, float3 surfaceNormal, float3 samplePosition, float offset = 0.001)
{
    float3 L    = samplePosition - originWorldPos;
    float  dist = length(L);

    RayDesc ray;
    ray.Origin    = originWorldPos;
    ray.Direction = L / max(dist, 1e-6);
    ray.TMin      = offset;
    ray.TMax      = max(offset, dist - offset * 2.0f);
    return ray;
}

// Conservative visibility — used by the RTXDI SDK during resampling passes
// (initial sampling, temporal, spatial resampling).
// RAY_FLAG_CULL_NON_OPAQUE intentionally skips alpha-tested / non-opaque
// geometry to avoid noisy contributions from surfaces
// that may or may not occlude depending on the alpha channel.
// This matches the reference sample design: conservative means "assume visible
// if unsure", so resampling keeps potentially good samples alive.
bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample lightSample)
{
    RayDesc ray = SetupShadowRay(surface.worldPos, surface.normal, lightSample.position);

    RayQuery<RAY_FLAG_CULL_NON_OPAQUE |
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(g_SceneAS, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();

    return q.CommittedStatus() == COMMITTED_NOTHING;
}

bool RAB_GetConservativeVisibility(RAB_Surface surface, float3 samplePosition)
{
    RayDesc ray = SetupShadowRay(surface.worldPos, surface.normal, samplePosition);

    RayQuery<RAY_FLAG_CULL_NON_OPAQUE |
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(g_SceneAS, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();

    return q.CommittedStatus() == COMMITTED_NOTHING;
}

// Final-shading visibility — used in the shading pass only.
// Unlike conservative visibility, properly handles ALPHA_MODE_MASK geometry by evaluating the albedo alpha channel
// with ray-space UV gradients, matching CalculateRTShadow / AlphaTestGrad.
// Uses offset=0.01 (matching FullSample's GetFinalVisibility) — larger than the conservative
// 0.001 to reduce self-intersection noise at the cost of slightly softer contact shadows.
bool GetFinalVisibility(RaytracingAccelerationStructure accelStruct, float3 originWorldPos, float3 surfaceNormal, float3 samplePosition)
{
    RayDesc ray = SetupShadowRay(originWorldPos, surfaceNormal, samplePosition, 0.01);

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(accelStruct, RAY_FLAG_NONE, 0xFF, ray);

    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            uint instanceIndex  = q.CandidateInstanceIndex();
            uint primitiveIndex = q.CandidatePrimitiveIndex();
            float2 bary         = q.CandidateTriangleBarycentrics();

            PerInstanceData   inst = g_RTXDI_Instances[instanceIndex];
            MeshData          mesh = g_RTXDI_MeshData[inst.m_MeshDataIndex];
            MaterialConstants mat  = g_RTXDI_Materials[inst.m_MaterialIndex];

            if (mat.m_AlphaMode == ALPHA_MODE_MASK)
            {
                TriangleVertices tv   = GetTriangleVertices(primitiveIndex, mesh, g_RTXDI_Indices, g_RTXDI_Vertices);
                RayGradients     grad = GetShadowRayGradients(tv, bary, ray.Origin, inst.m_World);

                if (AlphaTestGrad(grad.uv, grad.ddx, grad.ddy, mat))
                    q.CommitNonOpaqueTriangleHit(); // opaque: blocks light
                // else: transparent texel — do not commit, continue traversal
            }
            else
            {
                // ALPHA_MODE_OPAQUE non-opaque BLAS entry (shouldn't normally
                // happen) or ALPHA_MODE_BLEND: treat as a full occluder for
                // shadow purposes (blend surfaces are rare in shadow paths).
                q.CommitNonOpaqueTriangleHit();
            }
        }
    }

    return q.CommittedStatus() == COMMITTED_NOTHING;
}

// Previous-frame helpers (temporal resampling)
bool RAB_GetConservativeVisibilityPrevious(RAB_Surface surface, RAB_LightSample lightSample)
{
    RayDesc ray = SetupShadowRay(surface.worldPos, surface.normal, lightSample.position);

    RayQuery<RAY_FLAG_CULL_NON_OPAQUE |
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
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
    RayDesc ray = SetupShadowRay(previousSurface.worldPos, previousSurface.normal, samplePosition);

    RayQuery<RAY_FLAG_CULL_NON_OPAQUE |
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(g_SceneASHistory, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();

    return q.CommittedStatus() == COMMITTED_NOTHING;
}

int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious)
{
    return int(lightIndex); // Lights are stable permanently
}

int2 RAB_ClampSamplePositionIntoView(int2 pixelPosition, bool previousFrame)
{
    int width  = int(g_RTXDIConst.m_ViewportSize.x);
    int height = int(g_RTXDIConst.m_ViewportSize.y);

    // Reflect across screen edges instead of clamping.
    // Clamping causes many neighbours to collapse onto the same edge pixel,
    // producing streaks/blobs near screen borders.
    if (pixelPosition.x < 0)        pixelPosition.x = -pixelPosition.x;
    if (pixelPosition.y < 0)        pixelPosition.y = -pixelPosition.y;
    if (pixelPosition.x >= width)   pixelPosition.x = 2 * width  - pixelPosition.x - 1;
    if (pixelPosition.y >= height)  pixelPosition.y = 2 * height - pixelPosition.y - 1;

    return pixelPosition;
}

float RAB_GetBoilingFilterStrength() { return 0.25; }

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
