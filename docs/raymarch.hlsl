// Copyright (c) 2023 Tomasz Stachowiak
//
// This contribution is dual licensed under EITHER OF
// 
//     Apache License, Version 2.0, (http://www.apache.org/licenses/LICENSE-2.0)
//     MIT license (http://opensource.org/licenses/MIT)
// 
// at your option.

#include "/inc/frame_constants.hlsl"
#include "/inc/uv.hlsl"
#include "/inc/samplers.hlsl"

struct RootFinderInput {
    /// Distance to the surface of which a root we're trying to find
    float distance;

    /// Whether to consider this sample valid for intersection.
    /// Mostly relevant for allowing the ray marcher to travel behind surfaces,
    /// as it will mark surfaces it travels under as invalid.
    bool valid;
};

template<typename Point>
struct HybridRootFinder {
    uint linear_steps;
    uint bisection_steps;
    bool use_secant;
    float linear_march_exponent;

    float jitter;
    float min_t;
    float max_t;

    static HybridRootFinder new_with_linear_steps(uint v) {
        HybridRootFinder res;
        res.linear_steps = v;
        res.bisection_steps = 0;
        res.use_secant = false;
        res.linear_march_exponent = 1;
        res.jitter = 1;
        res.min_t = 0;
        res.max_t = 1;
        return res;
    }

    HybridRootFinder with_bisection_steps(uint v) {
        HybridRootFinder res = this;
        res.bisection_steps = v;
        return res;
    }

    HybridRootFinder with_use_secant(bool v) {
        HybridRootFinder res = this;
        res.use_secant = v;
        return res;
    }

    HybridRootFinder with_linear_march_exponent(float v) {
        HybridRootFinder res = this;
        res.linear_march_exponent = v;
        return res;
    }

    HybridRootFinder with_jitter(float v) {
        HybridRootFinder res = this;
        res.jitter = v;
        return res;
    }

    HybridRootFinder with_min_t(uint v) {
        HybridRootFinder res = this;
        res.min_t = v;
        return res;
    }

    HybridRootFinder with_max_t(uint v) {
        HybridRootFinder res = this;
        res.max_t = v;
        return res;
    }

    // IntersectionFn: (Point) -> DistanceTy, where `DistanceTy` inherits from `RootFinderInput`.
    template<typename IntersectionFn, typename DistanceTy>
    bool find_root(Point start, Point end, IntersectionFn distance_fn, inout float hit_t, inout float miss_t, out DistanceTy hit_d) {
        const Point dir = end - start;

        float min_t = this.min_t;
        float max_t = this.max_t;

        DistanceTy min_d = (DistanceTy)0;
        DistanceTy max_d = (DistanceTy)0;

        const float step_size = (max_t - min_t) / linear_steps;

        bool intersected = false;

        //
        // Ray march using linear steps

        if (linear_steps > 0) {
            //const float candidate_t = min_t + step_size * jitter;
            const float candidate_t = lerp(this.min_t, this.max_t, pow((0 + jitter) / linear_steps, this.linear_march_exponent));

            const Point candidate = start + dir * candidate_t;
            const DistanceTy candidate_d = distance_fn(candidate);
            intersected = candidate_d.distance < 0 && candidate_d.valid;

            if (intersected) {
                max_t = candidate_t;
                max_d = candidate_d;
                // The `[min_t .. max_t]` interval contains an intersection. End the linear search.
            } else {
                // No intersection yet. Carry on.
                min_t = candidate_t;
                min_d = candidate_d;

                for (uint step = 1; step < linear_steps; ++step) {
                    //const float candidate_t = min_t + step_size;
                    const float candidate_t = lerp(this.min_t, this.max_t, pow((step + jitter) / linear_steps, this.linear_march_exponent));

                    const Point candidate = start + dir * candidate_t;
                    const DistanceTy candidate_d = distance_fn(candidate);
                    intersected = candidate_d.distance < 0 && candidate_d.valid;

                    if (intersected) {
                        max_t = candidate_t;
                        max_d = candidate_d;
                        // The `[min_t .. max_t]` interval contains an intersection. End the linear search.
                        break;
                    } else {
                        // No intersection yet. Carry on.
                        min_t = candidate_t;
                        min_d = candidate_d;
                    }
                }
            }
        }

        miss_t = min_t;
        hit_t = min_t;

        //
        // Refine the hit using bisection

        if (intersected) {
            for (uint step = 0; step < bisection_steps; ++step) {
                const float mid_t = (min_t + max_t) * 0.5;
                const Point candidate = start + dir * mid_t;
                const DistanceTy candidate_d = distance_fn(candidate);

                if (candidate_d.distance < 0 && candidate_d.valid) {
                    // Intersection at the mid point. Refine the first half.
                    max_t = mid_t;
                    max_d = candidate_d;
                } else {
                    // No intersection yet at the mid point. Refine the second half.
                    min_t = mid_t;
                    min_d = candidate_d;

                    // This in practice seems to yield worse results as they end up being too sharp,
                    // and not diffused by noise, resulting in lots of pixel stretching:
                    //miss_t = min_t;
                }
            }

            if (this.use_secant) {
                // Finish with one application of the secant method
                const float total_d = min_d.distance + -max_d.distance;
                
                const float mid_t = lerp(min_t, max_t, min_d.distance / total_d);
                const Point candidate = start + dir * mid_t;
                const DistanceTy candidate_d = distance_fn(candidate);

                // Only accept the result of the secant method if it improves upon the previous result.
                //
                // Technically this should be `abs(candidate_d.distance) < min(min_d.distance, -max_d.distance) * frac`,
                // but this seems sufficient.
                if (abs(candidate_d.distance) < min_d.distance * 0.9 && candidate_d.valid) {
                    hit_t = mid_t;
                    hit_d = candidate_d;
                } else {
                    hit_t = max_t;
                    hit_d = max_d;
                }

                return true;
            } else {
                hit_t = max_t;
                hit_d = max_d;
                return true;
            }
        } else {
            // Mark the conservative miss distance.
            hit_t = min_t;
            return false;
        }
    }
};

struct DistanceWithPenetration: RootFinderInput {
    /// Conservative estimate of depth to which the ray penetrates the marched surface.
    float penetration;
};

struct DepthRaymarchDistanceFn {
    Texture2D<float> depth_tex;
    float2 depth_tex_size;

    bool march_behind_surfaces;
    float depth_thickness;

    bool use_sloppy_march;

    DistanceWithPenetration operator()(float3 ray_point_cs) {
        const float2 interp_uv = cs_to_uv(ray_point_cs.xy);

        const float ray_depth = 1.0 / ray_point_cs.z;

        // We're using both point-sampled and bilinear-filtered values from the depth buffer.
        //
        // That's really stupid but works like magic. For samples taken near the ray origin,
        // the discrete nature of the depth buffer becomes a problem. It's not a land of continuous surfaces,
        // but a bunch of stacked duplo bricks.
        //
        // Technically we should be taking discrete steps in this duplo land, but then we're at the mercy
        // of arbitrary quantization of our directions -- and sometimes we'll take a step which would
        // claim that the ray is occluded -- even though the underlying smooth surface wouldn't occlude it.
        //
        // If we instead take linear taps from the depth buffer, we reconstruct the linear surface.
        // That fixes acne, but introduces false shadowing near object boundaries, as we now pretend
        // that everything is shrink-wrapped by this continuous 2.5D surface, and our depth thickness
        // heuristic ends up falling apart.
        //
        // The fix is to consider both the smooth and the discrete surfaces, and only claim occlusion
        // when the ray descends below both.
        //
        // The two approaches end up fixing each other's artifacts:
        // * The false occlusions due to duplo land are rejected because the ray stays above the smooth surface.
        // * The shrink-wrap surface is no longer continuous, so it's possible for rays to miss it.

        #if 1
            const float linear_depth = 1.0 / depth_tex.SampleLevel(sampler_llc, interp_uv, 0);
            const float unfiltered_depth = 1.0 / depth_tex.SampleLevel(sampler_nnc, interp_uv, 0);
        #else
            // Manual bilinear; in case using a linear sampler on the depth format is not supported.
            const float4 depth_vals = depth_tex.GatherRed(sampler_nnc, interp_uv, 0);
            const float2 frac_px = frac(interp_uv * depth_tex_size - 0.5.xx);
            const float linear_depth = 1.0 / lerp(
                lerp(depth_vals.w, depth_vals.z, frac_px.x),
                lerp(depth_vals.x, depth_vals.y, frac_px.x),
                frac_px.y
            );

            const float unfiltered_depth = 1.0 / depth_tex.SampleLevel(sampler_nnc, interp_uv, 0);
        #endif

        float max_depth;
        float min_depth;

        if (use_sloppy_march) {
            max_depth = unfiltered_depth;
            min_depth = unfiltered_depth;
        } else {
            max_depth = max(linear_depth, unfiltered_depth);
            min_depth = min(linear_depth, unfiltered_depth);
        }

        const float bias = 0.000002;
        
        DistanceWithPenetration res;
        res.distance = max_depth * (1.0 + bias) - ray_depth;

        // This will be used at the end of the ray march to potentially discard the hit.
        res.penetration = ray_depth - min_depth;

        if (this.march_behind_surfaces) {
            res.valid = res.penetration < this.depth_thickness;
        } else {
            res.valid = true;
        }

        return res;
    }
};

struct DepthRayMarchResult {
    /// True if the raymarch hit something.
    bool hit;

    /// In case of a hit, the normalized distance to it.
    ///
    /// In case of a miss, the furthest the ray managed to travel, which could either be
    /// exceeding the max range, or getting behind a surface further than the depth thickness.
    ///
    /// Range: `0..=1` as a lerp factor over `ray_start_cs..=ray_end_cs`.
    float hit_t;

    /// UV correspindong to `hit_t`.
    float2 hit_uv;

    /// The distance that the hit point penetrates into the hit surface.
    /// Will normally be non-zero due to limited precision of the ray march.
    ///
    /// In case of a miss: undefined.
    float hit_penetration;

    /// Ditto, within the range `0..DepthRayMarch::depth_thickness_linear_z`
    ///
    /// In case of a miss: undefined.
    float hit_penetration_frac;
};

struct DepthRayMarch {
    /// Number of steps to be taken at regular intervals to find an initial intersection.
    /// Must not be zero.
    uint linear_steps;

    /// Exponent to be applied in the linear part of the march.
    ///
    /// A value of 1.0 will result in equidistant steps, and higher values will compress
    /// the earlier steps, and expand the later ones. This might be desirable in order
    /// to get more detail close to objects in SSR or SSGI.
    ///
    /// For optimal performance, this should be a small compile-time unsigned integer,
    /// such as 1 or 2.
    float linear_march_exponent;

    /// Number of steps in a bisection (binary search) to perform once the linear search
    /// has found an intersection. Helps narrow down the hit, increasing the chance of
    /// the secant method finding an accurate hit point.
    ///
    /// Useful when sampling color, e.g. SSR or SSGI, but pointless for contact shadows.
    uint bisection_steps;

    /// Approximate the root position using the secant method -- by solving for line-line
    /// intersection between the ray approach rate and the surface gradient.
    ///
    /// Useful when sampling color, e.g. SSR or SSGI, but pointless for contact shadows.
    bool use_secant;

    /// Jitter to apply to the first step of the linear search; 0..=1 range, mapping
    /// to the extent of a single linear step in the first phase of the search.
    /// Use 1.0 if you don't want jitter.
    float jitter;

    /// Clip space coordinates (w=1) of the ray.
    float3 ray_start_cs;
    float3 ray_end_cs;

    /// Should be used for contact shadows, but not for any color bounce, e.g. SSR.
    ///
    /// For SSR etc. this can easily create leaks, but with contact shadows it allows the rays
    /// to pass over invalid occlusions (due to thickness), and find potentially valid ones ahead.
    ///
    /// Note that this will cause the linear search to potentially miss surfaces,
    /// because when the ray overshoots and ends up penetrating a surface further than
    /// `depth_thickness_linear_z`, the ray marcher will just carry on.
    ///
    /// For this reason, this may require a lot of samples, or high depth thickness,
    /// so that `depth_thickness_linear_z >= world space ray length / linear_steps`.
    bool march_behind_surfaces;

    /// If `true`, the ray marcher only performs nearest lookups of the depth buffer,
    /// resulting in aliasing and false occlusion when marching tiny detail.
    /// It should work fine for longer traces with fewer rays though.
    bool use_sloppy_march;

    /// When marching the depth buffer, we only have 2.5D information, and don't know how
    /// thick surfaces are. We shall assume that the depth buffer fragments are litte squares
    /// with a constant thickness defined by this parameter.
    float depth_thickness_linear_z;

    /// Size of the depth buffer we're marching in, in pixels.
    float2 depth_tex_size;

    /// The depth buffer. Must use reverse Z with an infinite far plane.
    Texture2D<float> depth_tex;


    static DepthRayMarch new_from_depth(
        Texture2D<float> depth_tex,
        float2 depth_tex_size
    ) {
        DepthRayMarch res = (DepthRayMarch)0;
        res.jitter = 1;
        res.linear_steps = 4;
        res.bisection_steps = 0;
        res.linear_march_exponent = 1;
        res.depth_tex = depth_tex;
        res.depth_tex_size = depth_tex_size;
        res.depth_thickness_linear_z = 1;
        res.march_behind_surfaces = false;
        res.use_sloppy_march = false;
        return res;
    }

    DepthRayMarch with_linear_steps(uint v) {
        DepthRayMarch res = this;
        res.linear_steps = v;
        return res;
    }

    DepthRayMarch with_bisection_steps(uint v) {
        DepthRayMarch res = this;
        res.bisection_steps = v;
        return res;
    }

    DepthRayMarch with_use_secant(bool v) {
        DepthRayMarch res = this;
        res.use_secant = v;
        return res;
    }

    DepthRayMarch with_linear_march_exponent(float v) {
        DepthRayMarch res = this;
        res.linear_march_exponent = v;
        return res;
    }

    DepthRayMarch with_jitter(float v) {
        DepthRayMarch res = this;
        res.jitter = v;
        return res;
    }

    DepthRayMarch with_depth_thickness(float v) {
        DepthRayMarch res = this;
        res.depth_thickness_linear_z = v;
        return res;
    }

    DepthRayMarch with_march_behind_surfaces(bool v) {
        DepthRayMarch res = this;
        res.march_behind_surfaces = v;
        return res;
    }

    DepthRayMarch with_use_sloppy_march(bool v) {
        DepthRayMarch res = this;
        res.use_sloppy_march = v;
        return res;
    }

    /// March from a clip-space position (w = 1)
    DepthRayMarch from_cs(float3 v) {
        DepthRayMarch res = this;
        res.ray_start_cs = v;
        return res;
    }

    /// March towards a clip-space direction.
    /// If `infinite` is `true`, then the ray is extended to cover the whole view frustum.
    /// If `infinite` is `false`, then the ray length is that of the `dir_cs` parameter.
    //
    /// Must be called after `from_cs`, as it will clip the world-space ray to the view frustum.
    DepthRayMarch to_cs_dir_impl(float4 dir_cs, bool infinite) {
        float4 end_cs = float4(this.ray_start_cs, 1) + dir_cs;

        // Perform perspective division, but avoid dividing by zero for rays
        // heading directly towards the eye.
        end_cs /= (end_cs.w >= 0 ? 1 : -1) * max(1e-10, abs(end_cs.w));

        // Clip ray start to the view frustum
        //if (any(abs(this.ray_start_cs) > 1)) {
        if (true) {
            const float3 delta_cs = end_cs.xyz - this.ray_start_cs;
            const float3 near_edge = select(delta_cs < 0, float3(1, 1, 1), float3(-1, -1, 0));
            const float3 dist_to_near_edge = (near_edge - this.ray_start_cs) / delta_cs;
            //const float max_dist_to_near_edge = max(max(dist_to_near_edge.x, dist_to_near_edge.y), dist_to_near_edge.z);
            const float max_dist_to_near_edge = max(dist_to_near_edge.x, dist_to_near_edge.y);
            this.ray_start_cs += delta_cs * max(0.0, max_dist_to_near_edge);
        }
        
        // Clip ray end to the view frustum

        float3 delta_cs = end_cs.xyz - this.ray_start_cs;
        const float3 far_edge = select(delta_cs >= 0, float3(1, 1, 1), float3(-1, -1, 0));
        const float3 dist_to_far_edge = (far_edge - this.ray_start_cs) / delta_cs;
        const float min_dist_to_far_edge = min(min(dist_to_far_edge.x, dist_to_far_edge.y), dist_to_far_edge.z);

        if (infinite) {
            delta_cs *= min_dist_to_far_edge;
        } else {
            // If unbounded, would make the ray reach the end of the frustum
            delta_cs *= min(1.0, min_dist_to_far_edge);
        }

        DepthRayMarch res = this;
        res.ray_end_cs = this.ray_start_cs + delta_cs;
        return res;
    }

    /// March to a clip-space position (w = 1)
    ///
    /// Must be called after `from_cs`, as it will clip the world-space ray to the view frustum.
    DepthRayMarch to_cs(float3 end_cs) {
        float4 dir = float4(end_cs - this.ray_start_cs, 0) * sign(end_cs.z);
        return this.to_cs_dir_impl(dir, false);
    }

    /// March towards a clip-space direction. Infinite (ray is extended to cover the whole view frustum).
    ///
    /// Must be called after `from_cs`, as it will clip the world-space ray to the view frustum.
    DepthRayMarch to_cs_dir(float4 dir) {
        return this.to_cs_dir_impl(dir, true);
    }

    /// March to a world-space position.
    ///
    /// Must be called after `from_cs`, as it will clip the world-space ray to the view frustum.
    DepthRayMarch to_ws(float3 end) {
        return this.to_cs(main_view().position_world_to_sample(end));
    }

    /// March towards a world-space direction. Infinite (ray is extended to cover the whole view frustum).
    ///
    /// Must be called after `from_cs`, as it will clip the world-space ray to the view frustum.
    DepthRayMarch to_ws_dir(float3 dir) {
        return this.to_cs_dir_impl(main_view().direction_world_to_clip(dir), true);
    }

    /// Perform the ray march.
    DepthRayMarchResult march() {
        DepthRayMarchResult res = (DepthRayMarchResult)0;

        const float2 ray_start_uv = cs_to_uv(ray_start_cs.xy);
        const float2 ray_end_uv = cs_to_uv(ray_end_cs.xy);

        const float2 ray_uv_delta = ray_end_uv - ray_start_uv;
        const float2 ray_len_px = ray_uv_delta * depth_tex_size;

        const uint MIN_PX_PER_STEP = 1;
        const int step_count = max(2, min(linear_steps, int(floor(length(ray_len_px) / MIN_PX_PER_STEP))));

        const float linear_z_to_scaled_linear_z = main_view().rcp_near_plane_distance();
        const float depth_thickness = this.depth_thickness_linear_z * linear_z_to_scaled_linear_z;

        DepthRaymarchDistanceFn distance_fn;
        distance_fn.depth_tex = depth_tex;
        distance_fn.depth_tex_size = depth_tex_size;
        distance_fn.march_behind_surfaces = this.march_behind_surfaces;
        distance_fn.depth_thickness = depth_thickness;
        distance_fn.use_sloppy_march = this.use_sloppy_march;

        DistanceWithPenetration hit;

        float miss_t;
        bool intersected = HybridRootFinder<float3>
            ::new_with_linear_steps(step_count)
            .with_bisection_steps(this.bisection_steps)
            .with_use_secant(this.use_secant)
            .with_linear_march_exponent(this.linear_march_exponent)
            .with_jitter(this.jitter)
            .find_root(ray_start_cs, ray_end_cs, distance_fn, res.hit_t, miss_t, hit);

        if (intersected && hit.penetration < depth_thickness && hit.distance < depth_thickness) {
            res.hit = true;
            res.hit_uv = lerp(ray_start_uv, ray_end_uv, res.hit_t);
            res.hit_penetration = hit.penetration / linear_z_to_scaled_linear_z;
            res.hit_penetration_frac = hit.penetration / depth_thickness;
            return res;
        }

        res.hit_t = miss_t;
        res.hit_uv = lerp(ray_start_uv, ray_end_uv, res.hit_t);

        return res;
    }
};

DepthRayMarchResult raymarch_result = DepthRayMarch
    ::new_from_depth(depth_tex, depth_tex_dims)
    .from_cs(vc.ray_hit_cs.xyz)    // clip-space coord of the start pixel
    .to_ws(ray_hit_ws + direction_to_sun() * 0.3)
    .with_linear_steps(4)
    .with_depth_thickness(0.5)
    .with_jitter(raymarch_jitter)    // interleaved gradient noise
    .with_march_behind_surfaces(true)
    .march();

if (raymarch_result.hit) {
    shadow = smoothstep(1.0, 0.5, raymarch_result.hit_penetration_frac);
}