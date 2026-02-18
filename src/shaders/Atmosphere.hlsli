#ifndef ATMOSPHERE_HLSLI
#define ATMOSPHERE_HLSLI

#include "ShaderShared.h"
#include "Bindless.hlsli"

struct DensityProfileLayer
{
    float width;
    float exp_term;
    float exp_scale;
    float linear_term;
    float constant_term;
};

struct DensityProfile
{
    DensityProfileLayer layers[2];
};

struct AtmosphereParameters
{
    float3 solar_irradiance;
    float sun_angular_radius;
    float bottom_radius;
    float top_radius;
    float mie_phase_function_g;
    float mu_s_min;
    DensityProfile rayleigh_density;
    float3 rayleigh_scattering;
    DensityProfile mie_density;
    float3 mie_scattering;
    float3 mie_extinction;
    DensityProfile absorption_density;
    float3 absorption_extinction;
    float3 ground_albedo;
};

static const AtmosphereParameters ATMOSPHERE = 
{
    float3(1.474000, 1.850400, 1.911980), // solar_irradiance
    0.00935 / 2.0, // sun_angular_radius
    6360.0, // bottom_radius
    6420.0, // top_radius
    0.8, // mie_phase_function_g
    -0.207912, // mu_s_min
    // rayleigh_density
    {
        {
            { 0.0, 0.0, 0.0, 0.0, 0.0 },
            { 0.0, 1.0, -1.0 / 8.0, 0.0, 0.0 }
        }
    },
    float3(0.005802, 0.013558, 0.033100), // rayleigh_scattering
    // mie_density
    {
        {
            { 0.0, 0.0, 0.0, 0.0, 0.0 },
            { 0.0, 1.0, -1.0 / 1.2, 0.0, 0.0 }
        }
    },
    float3(0.003996, 0.003996, 0.003996), // mie_scattering
    float3(0.004440, 0.004440, 0.004440), // mie_extinction
    // absorption_density
    {
        {
            { 25.0, 0.0, 0.0, 1.0 / 15.0, -2.0 / 3.0 },
            { 0.0, 0.0, 0.0, -1.0 / 15.0, 8.0 / 3.0 }
        }
    },
    float3(0.000650, 0.001881, 0.000085), // absorption_extinction
    float3(0.1, 0.1, 0.1) // ground_albedo
};

static const float m = 1.0;
static const float nm = 1.0;
static const float rad = 1.0;
static const float sr = 1.0;
static const float watt = 1.0;
static const float lm = 1.0;
static const float km = 1000.0 * m;
static const float m2 = m * m;
static const float m3 = m * m * m;
static const float deg = PI / 180.0;
static const float watt_per_square_meter = watt / m2;
static const float watt_per_square_meter_per_sr = watt / (m2 * sr);
static const float watt_per_square_meter_per_nm = watt / (m2 * nm);
static const float watt_per_square_meter_per_sr_per_nm = watt / (m2 * sr * nm);
static const float watt_per_cubic_meter_per_sr_per_nm = watt / (m3 * sr * nm);
static const float cd = lm / sr;
static const float kcd = 1000.0 * cd;
static const float cd_per_square_meter = cd / m2;
static const float kcd_per_square_meter = kcd / m2;

static const int TRANSMITTANCE_TEXTURE_WIDTH = 256;
static const int TRANSMITTANCE_TEXTURE_HEIGHT = 64;
static const int SCATTERING_TEXTURE_R_SIZE = 32;
static const int SCATTERING_TEXTURE_MU_SIZE = 128;
static const int SCATTERING_TEXTURE_MU_S_SIZE = 32;
static const int SCATTERING_TEXTURE_NU_SIZE = 8;
static const int SCATTERING_TEXTURE_WIDTH = SCATTERING_TEXTURE_NU_SIZE * SCATTERING_TEXTURE_MU_S_SIZE;
static const int SCATTERING_TEXTURE_HEIGHT = SCATTERING_TEXTURE_MU_SIZE;
static const int SCATTERING_TEXTURE_DEPTH = SCATTERING_TEXTURE_R_SIZE;
static const int IRRADIANCE_TEXTURE_WIDTH = 64;
static const int IRRADIANCE_TEXTURE_HEIGHT = 16;

float ClampCosine(float mu)
{
    return clamp(mu, -1.0, 1.0);
}

float ClampDistance(float d)
{
    return max(d, 0.0 * m);
}

float ClampRadius(float r)
{
    return clamp(r, ATMOSPHERE.bottom_radius, ATMOSPHERE.top_radius);
}

float SafeSqrt(float a)
{
    return sqrt(max(a, 0.0 * m2));
}

float DistanceToTopAtmosphereBoundary(float r, float mu)
{
    float discriminant = r * r * (mu * mu - 1.0) + ATMOSPHERE.top_radius * ATMOSPHERE.top_radius;
    return ClampDistance(-r * mu + SafeSqrt(discriminant));
}

float DistanceToBottomAtmosphereBoundary(float r, float mu)
{
    float discriminant = r * r * (mu * mu - 1.0) + ATMOSPHERE.bottom_radius * ATMOSPHERE.bottom_radius;
    return ClampDistance(-r * mu - SafeSqrt(discriminant));
}

bool RayIntersectsGround(float r, float mu)
{
    return mu < 0.0 && r * r * (mu * mu - 1.0) + ATMOSPHERE.bottom_radius * ATMOSPHERE.bottom_radius >= 0.0 * m2;
}

float GetTextureCoordFromUnitRange(float x, int texture_size)
{
    return 0.5 / float(texture_size) + x * (1.0 - 1.0 / float(texture_size));
}

float GetUnitRangeFromTextureCoord(float u, int texture_size)
{
    return (u - 0.5 / float(texture_size)) / (1.0 - 1.0 / float(texture_size));
}

float2 GetTransmittanceTextureUvFromRMu(float r, float mu)
{
    float rho = SafeSqrt(r * r - ATMOSPHERE.bottom_radius * ATMOSPHERE.bottom_radius);
    float d = DistanceToTopAtmosphereBoundary(r, mu);
    float H = SafeSqrt(ATMOSPHERE.top_radius * ATMOSPHERE.top_radius - ATMOSPHERE.bottom_radius * ATMOSPHERE.bottom_radius);
    float x_mu = GetTextureCoordFromUnitRange(d / (rho + H), TRANSMITTANCE_TEXTURE_WIDTH);
    float x_r = GetTextureCoordFromUnitRange(rho / H, TRANSMITTANCE_TEXTURE_HEIGHT);
    return float2(x_mu, x_r);
}

float3 GetTransmittanceToTopAtmosphereBoundary(uint transmittance_texture_index, float r, float mu)
{
    float2 uv = GetTransmittanceTextureUvFromRMu(r, mu);
    return SampleBindlessTextureLevel(transmittance_texture_index, SAMPLER_LINEAR_CLAMP_INDEX, uv, 0).rgb;
}

float3 GetTransmittance(uint transmittance_texture_index, float r, float mu, float d, bool ray_r_mu_intersects_ground)
{
    float r_d = ClampRadius(sqrt(d * d + 2.0 * r * mu * d + r * r));
    float mu_d = ClampCosine((r * mu + d) / r_d);

    if (ray_r_mu_intersects_ground)
    {
        return min(
            GetTransmittanceToTopAtmosphereBoundary(transmittance_texture_index, r_d, -mu_d) /
            GetTransmittanceToTopAtmosphereBoundary(transmittance_texture_index, r, -mu),
            float3(1.0, 1.0, 1.0));
    }
    else
    {
        return min(
            GetTransmittanceToTopAtmosphereBoundary(transmittance_texture_index, r, mu) /
            GetTransmittanceToTopAtmosphereBoundary(transmittance_texture_index, r_d, mu_d),
            float3(1.0, 1.0, 1.0));
    }
}

float4 GetScatteringTextureUvwzFromRMuMuSNu(float r, float mu, float mu_s, float nu, bool ray_r_mu_intersects_ground)
{
    float H = sqrt(ATMOSPHERE.top_radius * ATMOSPHERE.top_radius - ATMOSPHERE.bottom_radius * ATMOSPHERE.bottom_radius);
    float rho = SafeSqrt(r * r - ATMOSPHERE.bottom_radius * ATMOSPHERE.bottom_radius);
    float u_r = GetTextureCoordFromUnitRange(rho / H, SCATTERING_TEXTURE_R_SIZE);

    float r_mu = r * mu;
    float discriminant = r_mu * r_mu - r * r + ATMOSPHERE.bottom_radius * ATMOSPHERE.bottom_radius;
    float u_mu;
    if (ray_r_mu_intersects_ground)
    {
        float d = -r_mu - SafeSqrt(discriminant);
        float d_min = r - ATMOSPHERE.bottom_radius;
        float d_max = rho;
        u_mu = 0.5 - 0.5 * GetTextureCoordFromUnitRange(d_max == d_min ? 0.0 : (d - d_min) / (d_max - d_min), SCATTERING_TEXTURE_MU_SIZE / 2);
    }
    else
    {
        float d = -r_mu + SafeSqrt(discriminant + H * H);
        float d_min = ATMOSPHERE.top_radius - r;
        float d_max = rho + H;
        u_mu = 0.5 + 0.5 * GetTextureCoordFromUnitRange((d - d_min) / (d_max - d_min), SCATTERING_TEXTURE_MU_SIZE / 2);
    }

    float d = DistanceToTopAtmosphereBoundary(ATMOSPHERE.bottom_radius, mu_s);
    float d_min = ATMOSPHERE.top_radius - ATMOSPHERE.bottom_radius;
    float d_max = H;
    float a = (d - d_min) / (d_max - d_min);
    float D = DistanceToTopAtmosphereBoundary(ATMOSPHERE.bottom_radius, ATMOSPHERE.mu_s_min);
    float A = (D - d_min) / (d_max - d_min);
    float u_mu_s = GetTextureCoordFromUnitRange(max(1.0 - a / A, 0.0) / (1.0 + a), SCATTERING_TEXTURE_MU_S_SIZE);

    float u_nu = (nu + 1.0) / 2.0;
    return float4(u_nu, u_mu_s, u_mu, u_r);
}

float3 GetExtrapolatedSingleMieScattering(float4 scattering)
{
    if (scattering.r <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    return scattering.rgb * scattering.a / scattering.r *
        (ATMOSPHERE.rayleigh_scattering.r / ATMOSPHERE.mie_scattering.r) *
        (ATMOSPHERE.mie_scattering / ATMOSPHERE.rayleigh_scattering);
}

float3 GetCombinedScattering(uint scattering_texture_index, float r, float mu, float mu_s, float nu, bool ray_r_mu_intersects_ground, out float3 single_mie_scattering)
{
    float4 uvwz = GetScatteringTextureUvwzFromRMuMuSNu(r, mu, mu_s, nu, ray_r_mu_intersects_ground);
    float tex_coord_x = uvwz.x * float(SCATTERING_TEXTURE_NU_SIZE - 1);
    float tex_x = floor(tex_coord_x);
    float lerp_val = tex_coord_x - tex_x;
    float3 uvw0 = float3((tex_x + uvwz.y) / float(SCATTERING_TEXTURE_NU_SIZE), uvwz.z, uvwz.w);
    float3 uvw1 = float3((tex_x + 1.0 + uvwz.y) / float(SCATTERING_TEXTURE_NU_SIZE), uvwz.z, uvwz.w);

    float4 combined_scattering = SampleBindlessTexture3DLevel(scattering_texture_index, SAMPLER_LINEAR_CLAMP_INDEX, uvw0, 0) * (1.0 - lerp_val) +
        SampleBindlessTexture3DLevel(scattering_texture_index, SAMPLER_LINEAR_CLAMP_INDEX, uvw1, 0) * lerp_val;

    float3 scattering = combined_scattering.rgb;
    single_mie_scattering = GetExtrapolatedSingleMieScattering(combined_scattering);
    return scattering;
}

float RayleighPhaseFunction(float nu)
{
    float k = 3.0 / (16.0 * PI * sr);
    return k * (1.0 + nu * nu);
}

float MiePhaseFunction(float g, float nu)
{
    float k = 3.0 / (8.0 * PI * sr) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + nu * nu) / pow(max(1.0 + g * g - 2.0 * g * nu, 0.0001), 1.5);
}

float2 GetIrradianceTextureUvFromRMuS(float r, float mu_s)
{
    float x_r = (r - ATMOSPHERE.bottom_radius) /
        (ATMOSPHERE.top_radius - ATMOSPHERE.bottom_radius);
    float x_mu_s = mu_s * 0.5 + 0.5;
    return float2(GetTextureCoordFromUnitRange(x_mu_s, IRRADIANCE_TEXTURE_WIDTH), GetTextureCoordFromUnitRange(x_r, IRRADIANCE_TEXTURE_HEIGHT));
}

float3 GetIrradiance(uint irradiance_texture_index, float r, float mu_s)
{
    float2 uv = GetIrradianceTextureUvFromRMuS(r, mu_s);
    return SampleBindlessTextureLevel(irradiance_texture_index, SAMPLER_LINEAR_CLAMP_INDEX, uv, 0).rgb;
}

float3 GetTransmittanceToSun(uint transmittance_texture_index, float r, float mu_s)
{
    float sin_theta_h = ATMOSPHERE.bottom_radius / r;
    float cos_theta_h = -sqrt(max(1.0 - sin_theta_h * sin_theta_h, 0.0));
    return GetTransmittanceToTopAtmosphereBoundary(transmittance_texture_index, r, mu_s) *
        smoothstep(-sin_theta_h * ATMOSPHERE.sun_angular_radius / rad, sin_theta_h * ATMOSPHERE.sun_angular_radius / rad, mu_s - cos_theta_h);
}

float3 GetSunAndSkyIrradiance(uint transmittance_texture_index, uint irradiance_texture_index, float3 p, float3 normal, float3 sun_direction, out float3 sky_irradiance)
{
    float r = length(p);
    float mu_s = dot(p, sun_direction) / r;

    // Indirect irradiance (approximated if the surface is not horizontal).
    sky_irradiance = GetIrradiance(irradiance_texture_index, r, mu_s) * (1.0 + dot(normal, p) / r) * 0.5;

    // Direct irradiance.
    return ATMOSPHERE.solar_irradiance * GetTransmittanceToSun(transmittance_texture_index, r, mu_s) * max(dot(normal, sun_direction), 0.0);
}

float3 GetSkyRadiance(uint transmittance_texture_index, uint scattering_texture_index, float3 camera, float3 view_ray, float shadow_length, float3 sun_direction, out float3 transmittance)
{
    float r = length(camera);
    float rmu = dot(camera, view_ray);
    float distance_to_top_atmosphere_boundary = -rmu - SafeSqrt(rmu * rmu - r * r + ATMOSPHERE.top_radius * ATMOSPHERE.top_radius);
    if (distance_to_top_atmosphere_boundary > 0.0 * m)
    {
        camera = camera + view_ray * distance_to_top_atmosphere_boundary;
        r = ATMOSPHERE.top_radius;
        rmu += distance_to_top_atmosphere_boundary;
    }
    else if (r > ATMOSPHERE.top_radius)
    {
        transmittance = float3(1.0, 1.0, 1.0);
        return float3(0.0, 0.0, 0.0);
    }

    float mu = rmu / r;
    float mu_s = dot(camera, sun_direction) / r;
    float nu = dot(view_ray, sun_direction);
    bool ray_r_mu_intersects_ground = RayIntersectsGround(r, mu);

    transmittance = ray_r_mu_intersects_ground ? float3(0.0, 0.0, 0.0) : GetTransmittanceToTopAtmosphereBoundary(transmittance_texture_index, r, mu);

    float3 single_mie_scattering;
    float3 scattering;
    if (shadow_length == 0.0 * m)
    {
        scattering = GetCombinedScattering(scattering_texture_index, r, mu, mu_s, nu, ray_r_mu_intersects_ground, single_mie_scattering);
    }
    else
    {
        float d = shadow_length;
        float r_p = ClampRadius(sqrt(d * d + 2.0 * r * mu * d + r * r));
        float mu_p = (r * mu + d) / r_p;
        float mu_s_p = (r * mu_s + d * nu) / r_p;
        scattering = GetCombinedScattering(scattering_texture_index, r_p, mu_p, mu_s_p, nu, ray_r_mu_intersects_ground, single_mie_scattering);
        float3 shadow_transmittance = GetTransmittance(transmittance_texture_index, r, mu, shadow_length, ray_r_mu_intersects_ground);
        scattering = scattering * shadow_transmittance;
        single_mie_scattering = single_mie_scattering * shadow_transmittance;
    }
    return scattering * RayleighPhaseFunction(nu) + single_mie_scattering * MiePhaseFunction(ATMOSPHERE.mie_phase_function_g, nu);
}

float3 GetSkyRadianceToPoint(uint transmittance_texture_index, uint scattering_texture_index, float3 camera, float3 p, float shadow_length, float3 sun_direction, out float3 transmittance)
{
    float3 view_ray = normalize(p - camera);
    float r = length(camera);
    float rmu = dot(camera, view_ray);
    float distance_to_top_atmosphere_boundary = -rmu - SafeSqrt(rmu * rmu - r * r + ATMOSPHERE.top_radius * ATMOSPHERE.top_radius);
    if (distance_to_top_atmosphere_boundary > 0.0 * m)
    {
        camera = camera + view_ray * distance_to_top_atmosphere_boundary;
        r = ATMOSPHERE.top_radius;
        rmu += distance_to_top_atmosphere_boundary;
    }

    float mu = rmu / r;
    float mu_s = dot(camera, sun_direction) / r;
    float nu = dot(view_ray, sun_direction);
    float d = length(p - camera);
    bool ray_r_mu_intersects_ground = RayIntersectsGround(r, mu);

    transmittance = GetTransmittance(transmittance_texture_index, r, mu, d, ray_r_mu_intersects_ground);

    float3 single_mie_scattering;
    float3 scattering = GetCombinedScattering(scattering_texture_index, r, mu, mu_s, nu, ray_r_mu_intersects_ground, single_mie_scattering);

    float r_p = ClampRadius(sqrt(d * d + 2.0 * r * mu * d + r * r));
    float mu_p = (r * mu + d) / r_p;
    float mu_s_p = (r * mu_s + d * nu) / r_p;

    float3 single_mie_scattering_p;
    float3 scattering_p = GetCombinedScattering(scattering_texture_index, r_p, mu_p, mu_s_p, nu, ray_r_mu_intersects_ground, single_mie_scattering_p);

    float3 shadow_transmittance = transmittance;
    if (shadow_length > 0.0 * m)
    {
        shadow_transmittance = shadow_transmittance * GetTransmittance(transmittance_texture_index, r, mu, shadow_length, ray_r_mu_intersects_ground);
    }

    scattering = scattering - shadow_transmittance * scattering_p;
    single_mie_scattering = single_mie_scattering - shadow_transmittance * single_mie_scattering_p;

    // Single Mie scattering is always >= 0.0.
    single_mie_scattering = max(single_mie_scattering, float3(0.0, 0.0, 0.0));

    return scattering * RayleighPhaseFunction(nu) + single_mie_scattering * MiePhaseFunction(ATMOSPHERE.mie_phase_function_g, nu);
}

#endif // ATMOSPHERE_HLSLI
