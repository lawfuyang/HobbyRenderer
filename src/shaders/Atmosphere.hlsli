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

/*
<p>They use the following utility functions to avoid NaNs due to floating point
values slightly outside their theoretical bounds:
*/
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

/*
<h5>Distance to the top atmosphere boundary</h5>

<p>A point at distance $d$ from $\bp$ along $[\bp,\bi)$ has coordinates
$[d\sqrt{1-\mu^2}, r+d\mu]^\top$, whose squared norm is $d^2+2r\mu d+r^2$.
Thus, by definition of $\bi$, we have
$\Vert\bp\bi\Vert^2+2r\mu\Vert\bp\bi\Vert+r^2=r_{\mathrm{top}}^2$,
from which we deduce the length $\Vert\bp\bi\Vert$:
*/
float DistanceToTopAtmosphereBoundary(float r, float mu)
{
    float discriminant = r * r * (mu * mu - 1.0) + ATMOSPHERE.top_radius * ATMOSPHERE.top_radius;
    return ClampDistance(-r * mu + SafeSqrt(discriminant));
}

/*
<p>We will also need, in the other sections, the distance to the bottom
atmosphere boundary, which can be computed in a similar way (this code assumes
that $[\bp,\bi)$ intersects the ground):
*/
float DistanceToBottomAtmosphereBoundary(float r, float mu)
{
    float discriminant = r * r * (mu * mu - 1.0) + ATMOSPHERE.bottom_radius * ATMOSPHERE.bottom_radius;
    return ClampDistance(-r * mu - SafeSqrt(discriminant));
}

/*
<h5>Intersections with the ground</h5>

<p>The segment $[\bp,\bi]$ intersects the ground when
$d^2+2r\mu d+r^2=r_{\mathrm{bottom}}^2$ has a solution with $d \ge 0$. This
requires the discriminant $r^2(\mu^2-1)+r_{\mathrm{bottom}}^2$ to be positive,
from which we deduce the following function:
*/
bool RayIntersectsGround(float r, float mu)
{
    return mu < 0.0 && r * r * (mu * mu - 1.0) + ATMOSPHERE.bottom_radius * ATMOSPHERE.bottom_radius >= 0.0 * m2;
}

/*
<p>For this we need a mapping between the function parameters $(r,\mu)$ and the
texture coordinates $(u,v)$, and vice-versa, because these parameters do not
have the same units and range of values. And even if it was the case, storing a
function $f$ from the $[0,1]$ interval in a texture of size $n$ would sample the
function at $0.5/n$, $1.5/n$, ... $(n-0.5)/n$, because texture samples are at
the center of texels. Therefore, this texture would only give us extrapolated
function values at the domain boundaries ($0$ and $1$). To avoid this we need
to store $f(0)$ at the center of texel 0 and $f(1)$ at the center of texel
$n-1$. This can be done with the following mapping from values $x$ in $[0,1]$ to
texture coordinates $u$ in $[0.5/n,1-0.5/n]$ - and its inverse:
*/
float GetTextureCoordFromUnitRange(float x, int texture_size)
{
    return 0.5 / float(texture_size) + x * (1.0 - 1.0 / float(texture_size));
}

float GetUnitRangeFromTextureCoord(float u, int texture_size)
{
    return (u - 0.5 / float(texture_size)) / (1.0 - 1.0 / float(texture_size));
}

/*
<p>With these definitions, the mapping from $(r,\mu)$ to the texture coordinates
$(u,v)$ can be implemented as follows:
*/
float2 GetTransmittanceTextureUvFromRMu(float r, float mu)
{
    float rho = SafeSqrt(r * r - ATMOSPHERE.bottom_radius * ATMOSPHERE.bottom_radius);
    float d = DistanceToTopAtmosphereBoundary(r, mu);
    float H = SafeSqrt(ATMOSPHERE.top_radius * ATMOSPHERE.top_radius - ATMOSPHERE.bottom_radius * ATMOSPHERE.bottom_radius);
    float x_mu = GetTextureCoordFromUnitRange(d / (rho + H), TRANSMITTANCE_TEXTURE_WIDTH);
    float x_r = GetTextureCoordFromUnitRange(rho / H, TRANSMITTANCE_TEXTURE_HEIGHT);
    return float2(x_mu, x_r);
}

/*
<h4 id="transmittance_lookup">Lookup</h4>

<p>With the help of the above precomputed texture, we can now get the
transmittance between a point and the top atmosphere boundary with a single
texture lookup (assuming there is no intersection with the ground):
*/
float3 GetTransmittanceToTopAtmosphereBoundary(uint transmittance_texture_index, float r, float mu)
{
    float2 uv = GetTransmittanceTextureUvFromRMu(r, mu);
    return SampleBindlessTextureLevel(transmittance_texture_index, SAMPLER_LINEAR_CLAMP_INDEX, uv, 0).rgb;
}

/*
<p>Also, with $r_d=\Vert\bo\bq\Vert=\sqrt{d^2+2r\mu d+r^2}$ and $\mu_d=
\bo\bq\cdot\bp\bi/\Vert\bo\bq\Vert\Vert\bp\bi\Vert=(r\mu+d)/r_d$ the values of
$r$ and $\mu$ at $\bq$, we can get the transmittance between two arbitrary
points $\bp$ and $\bq$ inside the atmosphere with only two texture lookups
(recall that the transmittance between $\bp$ and $\bq$ is the transmittance
between $\bp$ and the top atmosphere boundary, divided by the transmittance
between $\bq$ and the top atmosphere boundary, or the reverse - we continue to
assume that the segment between the two points does not intersect the ground):
*/
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

/*
<h4 id="single_scattering_precomputation">Precomputation</h4>

<p>The <code>ComputeSingleScattering</code> function is quite costly to
evaluate, and a lot of evaluations are needed to compute multiple scattering.
We therefore want to precompute it in a texture, which requires a mapping from
the 4 function parameters to texture coordinates. Assuming for now that we have
4D textures, we need to define a mapping from $(r,\mu,\mu_s,\nu)$ to texture
coordinates $(u,v,w,z)$. The function below implements the mapping defined in
our <a href="https://hal.inria.fr/inria-00288758/en">paper</a>, with some small
improvements (refer to the paper and to the above figures for the notations):
<ul>
<li>the mapping for $\mu$ takes the minimal distance to the nearest atmosphere
boundary into account, to map $\mu$ to the full $[0,1]$ interval (the original
mapping was not covering the full $[0,1]$ interval).</li>
<li>the mapping for $\mu_s$ is more generic than in the paper (the original
mapping was using ad-hoc constants chosen for the Earth atmosphere case). It is
based on the distance to the top atmosphere boundary (for the sun rays), as for
the $\mu$ mapping, and uses only one ad-hoc (but configurable) parameter. Yet,
as the original definition, it provides an increased sampling rate near the
horizon.</li>
</ul>
*/
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

/*
<p>In the second case, the green and blue components of the single Mie
scattering are extrapolated as described in our
<a href="https://hal.inria.fr/inria-00288758/en">paper</a>, with the following
function:
*/
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

/*
<p>We can then retrieve all the scattering components (Rayleigh + multiple
scattering on one side, and single Mie scattering on the other side) with the
following function, based on
<a href="#single_scattering_lookup"><code>GetScattering</code></a> (we duplicate
some code here, instead of using two calls to <code>GetScattering</code>, to
make sure that the texture coordinates computation is shared between the lookups
in <code>scattering_texture</code> and
<code>single_mie_scattering_texture</code>):
*/
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

/*
<p>Note that we added the solar irradiance and the scattering coefficient terms
that we omitted in <code>ComputeSingleScatteringIntegrand</code>, but not the
phase function terms - they are added at <a href="#rendering">render time</a>
for better angular precision. We provide them here for completeness:
*/
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

/*
<h4 id="irradiance_precomputation">Precomputation</h4>

<p>In order to precompute the ground irradiance in a texture we need a mapping
from the ground irradiance parameters to texture coordinates. Since we
precompute the ground irradiance only for horizontal surfaces, this irradiance
depends only on $r$ and $\mu_s$, so we need a mapping from $(r,\mu_s)$ to
$(u,v)$ texture coordinates. The simplest, affine mapping is sufficient here,
because the ground irradiance function is very smooth:
*/
float2 GetIrradianceTextureUvFromRMuS(float r, float mu_s)
{
    float x_r = (r - ATMOSPHERE.bottom_radius) /
        (ATMOSPHERE.top_radius - ATMOSPHERE.bottom_radius);
    float x_mu_s = mu_s * 0.5 + 0.5;
    return float2(GetTextureCoordFromUnitRange(x_mu_s, IRRADIANCE_TEXTURE_WIDTH), GetTextureCoordFromUnitRange(x_r, IRRADIANCE_TEXTURE_HEIGHT));
}

/*
<h4 id="irradiance_lookup">Lookup</h4>

<p>Thanks to these precomputed textures, we can now get the ground irradiance
with a single texture lookup:
*/
float3 GetIrradiance(uint irradiance_texture_index, float r, float mu_s)
{
    float2 uv = GetIrradianceTextureUvFromRMuS(r, mu_s);
    return SampleBindlessTextureLevel(irradiance_texture_index, SAMPLER_LINEAR_CLAMP_INDEX, uv, 0).rgb;
}

/*
<p>Finally, we will also need the transmittance between a point in the
atmosphere and the Sun. The Sun is not a point light source, so this is an
integral of the transmittance over the Sun disc. Here we consider that the
transmittance is constant over this disc, except below the horizon, where the
transmittance is 0. As a consequence, the transmittance to the Sun can be
computed with <code>GetTransmittanceToTopAtmosphereBoundary</code>, times the
fraction of the Sun disc which is above the horizon.

<p>This fraction varies from 0 when the Sun zenith angle $\theta_s$ is larger
than the horizon zenith angle $\theta_h$ plus the Sun angular radius $\alpha_s$,
to 1 when $\theta_s$ is smaller than $\theta_h-\alpha_s$. Equivalently, it
varies from 0 when $\mu_s=\cos\theta_s$ is smaller than
$\cos(\theta_h+\alpha_s)\approx\cos\theta_h-\alpha_s\sin\theta_h$ to 1 when
$\mu_s$ is larger than
$\cos(\theta_h-\alpha_s)\approx\cos\theta_h+\alpha_s\sin\theta_h$. In between,
the visible Sun disc fraction varies approximately like a smoothstep (this can
be verified by plotting the area of <a
href="https://en.wikipedia.org/wiki/Circular_segment">circular segment</a> as a
function of its <a href="https://en.wikipedia.org/wiki/Sagitta_(geometry)"
>sagitta</a>). Therefore, since $\sin\theta_h=r_{\mathrm{bottom}}/r$, we can
approximate the transmittance to the Sun with the following function:
*/
float3 GetTransmittanceToSun(uint transmittance_texture_index, float r, float mu_s)
{
    float sin_theta_h = ATMOSPHERE.bottom_radius / r;
    float cos_theta_h = -sqrt(max(1.0 - sin_theta_h * sin_theta_h, 0.0));
    return GetTransmittanceToTopAtmosphereBoundary(transmittance_texture_index, r, mu_s) *
        smoothstep(-sin_theta_h * ATMOSPHERE.sun_angular_radius / rad, sin_theta_h * ATMOSPHERE.sun_angular_radius / rad, mu_s - cos_theta_h);
}

/*
<h4 id="rendering_ground">Ground</h4>

<p>To render the ground we need the irradiance received on the ground after 0 or
more bounce(s) in the atmosphere or on the ground. The direct irradiance can be
computed with a lookup in the transmittance texture,
via <code>GetTransmittanceToSun</code>, while the indirect irradiance is given
by a lookup in the precomputed irradiance texture (this texture only contains
the irradiance for horizontal surfaces; we use the approximation defined in our
<a href="https://hal.inria.fr/inria-00288758/en">paper</a> for the other cases).
The function below returns the direct and indirect irradiances separately:
*/
float3 GetSunAndSkyIrradiance(uint transmittance_texture_index, uint irradiance_texture_index, float3 p, float3 normal, float3 sun_direction, out float3 sky_irradiance)
{
    float r = length(p);
    float mu_s = dot(p, sun_direction) / r;

    // Indirect irradiance (approximated if the surface is not horizontal).
    sky_irradiance = GetIrradiance(irradiance_texture_index, r, mu_s) * (1.0 + dot(normal, p) / r) * 0.5;

    // Direct irradiance.
    return ATMOSPHERE.solar_irradiance * GetTransmittanceToSun(transmittance_texture_index, r, mu_s) * max(dot(normal, sun_direction), 0.0);
}

/*
<h4 id="rendering_sky">Sky</h4>

<p>To render the sky we simply need to display the sky radiance, which we can
get with a lookup in the precomputed scattering texture(s), multiplied by the
phase function terms that were omitted during precomputation. We can also return
the transmittance of the atmosphere (which we can get with a single lookup in
the precomputed transmittance texture), which is needed to correctly render the
objects in space (such as the Sun and the Moon). This leads to the following
function, where most of the computations are used to correctly handle the case
of viewers outside the atmosphere, and the case of light shafts:
*/
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

/*
<h4 id="rendering_aerial_perspective">Aerial perspective</h4>

<p>To render the aerial perspective we need the transmittance and the scattering
between two points (i.e. between the viewer and a point on the ground, which can
at an arbibrary altitude). We already have a function to compute the
transmittance between two points (using 2 lookups in a texture which only
contains the transmittance to the top of the atmosphere), but we don't have one
for the scattering between 2 points. Hopefully, the scattering between 2 points
can be computed from two lookups in a texture which contains the scattering to
the nearest atmosphere boundary, as for the transmittance (except that here the
two lookup results must be subtracted, instead of divided). This is what we
implement in the following function (the initial computations are used to
correctly handle the case of viewers outside the atmosphere):
*/
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

// Helper functions for easy atmospheric lighting access in shaders
float3 GetAtmospherePos(float3 worldPos)
{
    return (worldPos - kEarthCenter) / 1000.0; // convert to km and center on earth
}

float3 GetAtmosphereSunRadiance(float3 p_atmo, float3 sunDirection, float sunIntensity)
{
    float r = length(p_atmo);
    float mu_s = dot(p_atmo, sunDirection) / r;
    return ATMOSPHERE.solar_irradiance * GetTransmittanceToSun(BRUNETON_TRANSMITTANCE_TEXTURE, r, mu_s) * sunIntensity;
}

float3 GetAtmosphereSkyIrradiance(float3 p_atmo, float3 normal, float3 sunDirection, float sunIntensity)
{
    float3 skyIrradiance;
    GetSunAndSkyIrradiance(BRUNETON_TRANSMITTANCE_TEXTURE, BRUNETON_IRRADIANCE_TEXTURE, p_atmo, normal, sunDirection, skyIrradiance);
    return skyIrradiance * sunIntensity;
}

float3 GetAtmosphereSkyRadiance(float3 cameraPos, float3 viewRay, float3 sunDirection, float sunIntensity, bool bAddSunDisk = true)
{
    float3 cameraPos_atmo = GetAtmospherePos(cameraPos);
    float3 transmittance;
    float3 skyRadiance = GetSkyRadiance(BRUNETON_TRANSMITTANCE_TEXTURE, BRUNETON_SCATTERING_TEXTURE, cameraPos_atmo, viewRay, 0.0, sunDirection, transmittance);

    // Add sun disk
    if (bAddSunDisk)
    {
        float nu = dot(viewRay, sunDirection);
        float sunAngularRadius = ATMOSPHERE.sun_angular_radius;
        if (nu > cos(sunAngularRadius))
        {
            float3 sunDiskRadiance = ATMOSPHERE.solar_irradiance / (PI * sunAngularRadius * sunAngularRadius);
            skyRadiance += sunDiskRadiance * transmittance;
        }
    }
    return skyRadiance * sunIntensity;
}

#endif // ATMOSPHERE_HLSLI
