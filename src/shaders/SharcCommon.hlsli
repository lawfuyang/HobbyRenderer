#ifndef SHARC_COMMON_HLSLI
#define SHARC_COMMON_HLSLI

#include "SharcCommon.h"
#include "CommonLighting.hlsli"
#include "Atmosphere.hlsli"

SharcParameters BuildSharcParameters(float3 cameraPosition, float sceneScale, uint capacity,
                                     RWStructuredBuffer<uint64_t> hashEntriesBuffer,
                                     RWStructuredBuffer<SharcAccumulationData> accumulationBuffer,
                                     RWStructuredBuffer<SharcPackedData> resolvedBuffer)
{
    SharcParameters sp;
    sp.hashGridParameters.cameraPosition = cameraPosition;
    sp.hashGridParameters.sceneScale     = sceneScale;
    sp.hashGridParameters.logarithmBase  = SHARC_GRID_LOGARITHM_BASE;
    sp.hashGridParameters.levelBias      = SHARC_GRID_LEVEL_BIAS;

    sp.hashGridData.capacity             = capacity;
    sp.hashGridData.hashEntriesBuffer    = hashEntriesBuffer;

    sp.accumulationBuffer                = accumulationBuffer;
    sp.resolvedBuffer                    = resolvedBuffer;
    sp.radianceScale                     = SHARC_RADIANCE_SCALE;

    return sp;
}

// ─── Shared Lighting Helpers ──────────────────────────────────────────────────
//
// Both SharcQuery and SharcUpdate perform the same direct-lighting setup
// and BRDF importance sampling. These helpers eliminate the lines of
// duplicate code between the two passes.

// Fills a LightingInputs struct with the common setup shared by Query and
// Update passes. The caller must call PrepareLightingByproducts() afterwards
// to populate F, F0, and other derived fields.
LightingInputs FillSharcLightingInputs(
    float3 N, float3 V, float3 worldPos,
    PBRAttributes pbr, srrhi::MaterialConstants mat,
    RaytracingAccelerationStructure sceneAS,
    StructuredBuffer<srrhi::PerInstanceData> instances,
    StructuredBuffer<srrhi::MeshData> meshData,
    StructuredBuffer<srrhi::MaterialConstants> materials,
    StructuredBuffer<uint> indices,
    StructuredBuffer<srrhi::VertexQuantized> vertices,
    StructuredBuffer<srrhi::GPULight> lights,
    float3 sunDirection)
{
    LightingInputs inputs;
    inputs.N                = N;
    inputs.V                = V;
    inputs.L                = float3(0, 0, 0);
    inputs.worldPos         = worldPos;
    inputs.baseColor        = pbr.baseColor;
    inputs.roughness        = pbr.roughness;
    inputs.metallic         = pbr.metallic;
    inputs.ior              = mat.m_IOR;
    inputs.radianceMipCount = 0;
    inputs.enableRTShadows  = true;
    inputs.sceneAS          = sceneAS;
    inputs.instances        = instances;
    inputs.meshData         = meshData;
    inputs.materials        = materials;
    inputs.indices          = indices;
    inputs.vertices         = vertices;
    inputs.lights           = lights;
    inputs.sunRadiance      = GetAtmosphereSunRadiance(GetAtmospherePos(worldPos), sunDirection, lights[0].m_Intensity);
    inputs.sunDirection     = sunDirection;
    inputs.useSunRadiance   = true;
    inputs.sunShadow        = 0.0f; // unused by RNG variant — casts its own shadow rays
    return inputs;
}

// BRDF importance sampling shared between Query and Update.
// Uses the specProb → GGX VNDF / Cosine branching formula.
// Returns false if the sampled direction faces away from the surface
// (caller should break the bounce loop).
bool SampleSharcDirection(
    float3 N, float3 V,
    PBRAttributes pbr,
    float3 F, float3 F0,
    inout RNG rng,
    out float3 outDirection,
    out float3 outBrdfWeight,
    out bool outIsSpecular)
{
    outDirection = float3(0, 0, 0);
    outBrdfWeight = float3(0, 0, 0);
    outIsSpecular = false;

    float specProb = clamp(lerp(F.r * 0.5f + 0.5f * pbr.metallic, 1.0f, pbr.metallic), 0.1f, 0.9f);
    if (NextFloat(rng) < specProb)
    {
        // Specular: GGX VNDF sample
        float3 H = SampleGGX_VNDF(NextFloat2(rng), N, V, pbr.roughness);
        outDirection = reflect(-V, H);
        if (dot(N, outDirection) <= 0.0f) return false;
        outBrdfWeight = EvalGGX_VNDF_Weight(F0, N, V, outDirection, H, pbr.roughness) / specProb;
        outIsSpecular = true;
    }
    else
    {
        // Diffuse: cosine-weighted hemisphere sample
        outDirection = SampleHemisphereCosine(NextFloat2(rng), N);
        if (dot(N, outDirection) <= 0.0f) return false;
        outBrdfWeight = pbr.baseColor * (1.0f - pbr.metallic) / (1.0f - specProb);
        outIsSpecular = false;
    }
    return true;
}

#endif // SHARC_COMMON_HLSLI
