/*
 * Copyright 2024 Tom Clabault. GNU GPL3 license.
 * GNU GPL3 license copy: https://www.gnu.org/licenses/gpl-3.0.txt
 */

#include "Device/includes/AdaptiveSampling.h"
#include "Device/includes/FixIntellisense.h"
#include "Device/includes/Lights.h"
#include "Device/includes/Envmap.h"
#include "Device/includes/Material.h"
#include "Device/includes/RayPayload.h"
#include "Device/includes/Sampling.h"
#include "HostDeviceCommon/HIPRTCamera.h"
#include "HostDeviceCommon/Xorshift.h"

HIPRT_HOST_DEVICE HIPRT_INLINE unsigned int wang_hash(unsigned int seed)
{
    seed = (seed ^ 61) ^ (seed >> 16);
    seed *= 9;
    seed = seed ^ (seed >> 4);
    seed *= 0x27d4eb2d;
    seed = seed ^ (seed >> 15);
    return seed;
}

HIPRT_HOST_DEVICE HIPRT_INLINE void debug_set_final_color(const HIPRTRenderData& render_data, int x, int y, int res_x, ColorRGB32F final_color)
{
    if (render_data.render_settings.sample_number == 0)
        render_data.buffers.pixels[y * res_x + x] = final_color;
    else
        render_data.buffers.pixels[y * res_x + x] = final_color * render_data.render_settings.sample_number;
}

HIPRT_HOST_DEVICE HIPRT_INLINE bool check_for_negative_color(ColorRGB32F ray_color, int x, int y, int sample)
{
    if (ray_color.r < 0 || ray_color.g < 0 || ray_color.b < 0)
    {
#ifndef __KERNELCC__
        std::cout << "Negative color at [" << x << ", " << y << "], sample " << sample << std::endl;
#endif

        return true;
    }

    return false;
}

HIPRT_HOST_DEVICE HIPRT_INLINE bool check_for_nan(ColorRGB32F ray_color, int x, int y, int sample)
{
    if (hippt::isNaN(ray_color.r) || hippt::isNaN(ray_color.g) || hippt::isNaN(ray_color.b))
    {
#ifndef __KERNELCC__
        std::cout << "NaN at [" << x << ", " << y << "], sample" << sample << std::endl;
#endif
        return true;
    }

    return false;
}

#ifndef __KERNELCC__
#include "Utils/Utils.h" // For debugbreak in sanity_check()
#endif
HIPRT_HOST_DEVICE HIPRT_INLINE bool sanity_check(const HIPRTRenderData& render_data, RayPayload& ray_payload, int x, int y, int2& res, int sample)
{
    bool invalid = false;
    invalid |= check_for_negative_color(ray_payload.ray_color, x, y, sample);
    invalid |= check_for_nan(ray_payload.ray_color, x, y, sample);

    if (invalid)
    {
        if (render_data.render_settings.display_NaNs)
            debug_set_final_color(render_data, x, y, res.x, ColorRGB32F(1.0e15f, 0.0f, 1.0e15f));
        else
            ray_payload.ray_color = ColorRGB32F(0.0f);
#ifndef __KERNELCC__
        Utils::debugbreak();
#endif
    }

    return !invalid;
}

HIPRT_HOST_DEVICE HIPRT_INLINE void reset_render(const HIPRTRenderData& render_data, uint32_t pixel_index)
{
    // Resetting all buffers on the first frame
    render_data.buffers.pixels[pixel_index] = ColorRGB32F(0.0f);
    render_data.aux_buffers.denoiser_normals[pixel_index] = make_float3(1.0f, 1.0f, 1.0f);
    render_data.aux_buffers.denoiser_albedo[pixel_index] = ColorRGB32F(0.0f, 0.0f, 0.0f);

    if (render_data.render_settings.has_access_to_adaptive_sampling_buffers())
    {
        // These buffers are only available when either the adaptive sampling or the stop noise threshold is enabled
        render_data.aux_buffers.pixel_sample_count[pixel_index] = 0;
        render_data.aux_buffers.pixel_squared_luminance[pixel_index] = 0;
    }
}

#ifdef __KERNELCC__
GLOBAL_KERNEL_SIGNATURE(void) PathTracerKernel(HIPRTRenderData render_data, int2 res, HIPRTCamera camera)
#else
GLOBAL_KERNEL_SIGNATURE(void) inline PathTracerKernel(HIPRTRenderData render_data, int2 res, HIPRTCamera camera, int x, int y)
#endif
{
#ifdef __KERNELCC__
    const uint32_t x = blockIdx.x * blockDim.x + threadIdx.x;
    const uint32_t y = blockIdx.y * blockDim.y + threadIdx.y;
#endif
    uint32_t pixel_index = (x + y * res.x);
    if (pixel_index >= res.x * res.y)
        return;

    // 'Render low resolution' means that the user is moving the camera for example
    // so we're going to reduce the quality of the render for increased framerates
    // while moving
    if (render_data.render_settings.render_low_resolution)
    {
        // Reducing the number of bounces to 3
        render_data.render_settings.nb_bounces = 3;
        render_data.render_settings.samples_per_frame = 1;
        int res_scaling = render_data.render_settings.render_low_resolution_scaling;
        pixel_index /= res_scaling;

        // If rendering at low resolution, only one pixel out of res_scaling^2
        // (a square of res_scaling * res_scaling) will be rendered
        if (x % res_scaling != 0 || y % res_scaling != 0)
            return;
    }

    if (render_data.render_settings.sample_number == 0)
        reset_render(render_data, pixel_index);

    bool sampling_needed = true;
    bool pixel_converged = false;
    sampling_needed = adaptive_sampling(render_data, pixel_index, pixel_converged);

    if (pixel_converged || !sampling_needed)
        // Indicating that this pixel has reached the threshold in render_settings.stop_pixel_noise_threshold
        hippt::atomic_add(render_data.aux_buffers.stop_noise_threshold_count, 1u);

    if (!sampling_needed)
    {
        // Because when displaying the framebuffer, we're dividing by the number of samples to 
        // rescale the color of a pixel, we're going to have a problem if some pixels stopped samping
        // at 10 samples while the other pixels are still being sampled and have 100 samples for example. 
        // The pixels that only received 10 samples are going to be divided by 100 at display time, making them
        // appear too dark.
        // We're rescaling the color of the pixels that stopped sampling here for correct display
        render_data.buffers.pixels[pixel_index] = render_data.buffers.pixels[pixel_index] / render_data.render_settings.sample_number * (render_data.render_settings.sample_number + render_data.render_settings.samples_per_frame);

        return;
    }

    unsigned int seed;
    if (render_data.render_settings.freeze_random)
        seed = wang_hash(pixel_index + 1);
    else
        seed = wang_hash((pixel_index + 1) * (render_data.render_settings.sample_number + 1));
    Xorshift32Generator random_number_generator(seed);

    float squared_luminance_of_samples = 0.0f;
    ColorRGB32F final_color = ColorRGB32F(0.0f, 0.0f, 0.0f);
    ColorRGB32F denoiser_albedo = ColorRGB32F(0.0f, 0.0f, 0.0f);
    float3 denoiser_normal = make_float3(0.0f, 0.0f, 0.0f);
    for (int sample = 0; sample < render_data.render_settings.samples_per_frame; sample++)
    {
        //Jittered around the center
        float x_jittered = (x + 0.5f) + random_number_generator() - 1.0f;
        float y_jittered = (y + 0.5f) + random_number_generator() - 1.0f;

        hiprtRay ray = camera.get_camera_ray(x_jittered, y_jittered, res);
        RayPayload ray_payload;

        // Whether or not we've already written to the denoiser's buffers
        bool denoiser_AOVs_set = false;
        float denoiser_blend = 1.0f;

        for (int bounce = 0; bounce < render_data.render_settings.nb_bounces; bounce++)
        {
            if (ray_payload.next_ray_state == RayState::BOUNCE)
            {
                HitInfo closest_hit_info;
                bool intersection_found = trace_ray(render_data, ray, ray_payload, closest_hit_info);

                if (intersection_found)
                {
                    if (bounce == 0)
                    {
                        denoiser_normal += closest_hit_info.shading_normal;
                        denoiser_albedo += ray_payload.material.base_color;
                    }

                    // For the BRDF calculations, bounces, ... to be correct, we need the normal to be in the same hemisphere as
                    // the view direction. One thing that can go wrong is when we have an emissive triangle (typical area light)
                    // and a ray hits the back of the triangle. The normal will not be facing the view direction in this
                    // case and this will cause issues later in the BRDF.
                    // Because we want to allow backfacing emissive geometry (making the emissive geometry double sided
                    // and emitting light in both directions of the surface), we're negating the normal to make
                    // it face the view direction (but only for emissive geometry)
                    
                    if (ray_payload.material.is_emissive() && hippt::dot(-ray.direction, closest_hit_info.geometric_normal) < 0)
                    {
                        closest_hit_info.geometric_normal = -closest_hit_info.geometric_normal;
                        closest_hit_info.shading_normal = -closest_hit_info.shading_normal;
                    }

                    // --------------------------------------------------- //
                    // ----------------- Direct lighting ----------------- //
                    // --------------------------------------------------- //

                    ColorRGB32F light_sample_radiance = sample_one_light(render_data, ray_payload, closest_hit_info, -ray.direction, random_number_generator);
                    ColorRGB32F envmap_radiance = sample_environment_map(render_data, ray_payload, closest_hit_info, -ray.direction, random_number_generator);

                    //// TODO fix NaNs being clamped to 1.0e35f here
                    //if (!light_sample_radiance.has_NaN())
                    //{
                    //    ColorRGB32F direct_lighting_clamp(render_data.render_settings.direct_contribution_clamp > 0.0f ? render_data.render_settings.direct_contribution_clamp : 1.0e35f);
                    //    if (bounce == 0)
                    //        // Clamping only on the primary rays
                    //        light_sample_radiance = ColorRGB32F::min(direct_lighting_clamp, light_sample_radiance);
                    //}

                    //if (!envmap_radiance.has_NaN())
                    //{
                    //    ColorRGB32F envmap_lighting_clamp(render_data.render_settings.envmap_contribution_clamp > 0.0f ? render_data.render_settings.envmap_contribution_clamp : 1.0e35f);

                    //    envmap_radiance = ColorRGB32F::min(envmap_lighting_clamp, envmap_radiance);
                    //}

#if DirectLightSamplingStrategy == LSS_NO_DIRECT_LIGHT_SAMPLING // No direct light sampling
                    ray_payload.ray_color += ray_payload.material.emission * ray_payload.throughput;
#else
                    if (bounce == 0)
                        // If we do have emissive geometry sampling, we only want to take
                        // it into account on the first bounce, otherwise we would be
                        // accounting for direct light sampling twice (bounce on emissive
                        // geometry + direct light sampling). Otherwise, we don't check for bounce == 0
                        ray_payload.ray_color += ray_payload.material.emission * ray_payload.throughput;
#endif

                    ray_payload.ray_color += (light_sample_radiance + envmap_radiance) * ray_payload.throughput;

                    // --------------------------------------- //
                    // ---------- Indirect lighting ---------- //
                    // --------------------------------------- //

                    float brdf_pdf;
                    float3 bounce_direction;
                    ColorRGB32F bsdf_color = bsdf_dispatcher_sample(render_data.buffers.materials_buffer, ray_payload.material, ray_payload.volume_state, -ray.direction, closest_hit_info.shading_normal, closest_hit_info.geometric_normal, bounce_direction, brdf_pdf, random_number_generator);

                    // Terminate ray if bad sampling
                    if (brdf_pdf <= 0.0f)
                        break;

                    //ColorRGB32F indirect_clamp(render_data.render_settings.indirect_contribution_clamp > 0.0f ? render_data.render_settings.indirect_contribution_clamp : 1.0e35f);
                    ray_payload.throughput *= bsdf_color * hippt::abs(hippt::dot(bounce_direction, closest_hit_info.shading_normal)) / brdf_pdf;
                    //ray_payload.throughput = ColorRGB32F::min(indirect_clamp, ray_payload.throughput);

                    int outside_surface = hippt::dot(bounce_direction, closest_hit_info.shading_normal) < 0 ? -1.0f : 1.0f;
                    ray.origin = closest_hit_info.inter_point + closest_hit_info.shading_normal * 3.0e-3f * outside_surface;
                    ray.direction = bounce_direction;

                    ray_payload.next_ray_state = RayState::BOUNCE;
                }
                else
                {
                    ColorRGB32F skysphere_color;
                    if (render_data.world_settings.ambient_light_type == AmbientLightType::UNIFORM)
                        skysphere_color = render_data.world_settings.uniform_light_color;
                    else if (render_data.world_settings.ambient_light_type == AmbientLightType::ENVMAP)
                    {
#if EnvmapSamplingStrategy != ESS_NO_SAMPLING
                        // If we have sampling, only taking envmap into account on camera ray miss
                        if (bounce == 0)
#endif
                        {
                            // We're only getting the skysphere radiance for the first rays because the
                            // syksphere is importance sampled.
                            // 
                            // We're also getting the skysphere radiance for perfectly specular BRDF since those
                            // are not importance sampled.

                            skysphere_color = sample_environment_map_from_direction(render_data.world_settings, ray.direction);

#if EnvmapSamplingStrategy == ESS_NO_SAMPLING
                            // If we don't have envmap sampling, we're only going to unscale on
                            // bounce 0 (which is when a ray misses directly --> background color).
                            // Otherwise, if not bounce 2, we do want to take the scaling into
                            // account so this if will fail and the envmap color will never be unscaled
                            if (!render_data.world_settings.envmap_scale_background_intensity && bounce == 0)
#else
                            if (!render_data.world_settings.envmap_scale_background_intensity)
#endif
                                // Un-scaling the envmap if the user doesn't want to scale the background
                                skysphere_color /= render_data.world_settings.envmap_intensity;
                        }
                    }

                    //ColorRGB32F skysphere_clamp(render_data.render_settings.envmap_contribution_clamp > 0.0f ? render_data.render_settings.envmap_contribution_clamp : 1.0e35f);
                    //skysphere_color = ColorRGB32F::min(skysphere_clamp, skysphere_color);

                    ray_payload.ray_color += skysphere_color * ray_payload.throughput;
                    ray_payload.next_ray_state = RayState::MISSED;
                }
            }
            else if (ray_payload.next_ray_state == RayState::MISSED)
                break;
        }

        // Checking for NaNs / negative value samples. Output 
        if (!sanity_check(render_data, ray_payload, x, y, res, sample))
            return;

        squared_luminance_of_samples += ray_payload.ray_color.luminance() * ray_payload.ray_color.luminance();
        final_color += ray_payload.ray_color;
    }

    // If we got here, this means that we still have at least one ray active
    render_data.aux_buffers.still_one_ray_active[0] = 1;

    if (render_data.render_settings.has_access_to_adaptive_sampling_buffers())
    {
        // We can only use these buffers if the adaptive sampling or the stop noise threshold is enabled.
        // Otherwise, the buffers are destroyed to save some VRAM so they are not accessible
        render_data.aux_buffers.pixel_squared_luminance[pixel_index] += squared_luminance_of_samples;
        render_data.aux_buffers.pixel_sample_count[pixel_index] += render_data.render_settings.samples_per_frame;
    }

    render_data.buffers.pixels[pixel_index] += final_color;

    // Handling denoiser's albedo and normals AOVs    
    denoiser_albedo /= (float)render_data.render_settings.samples_per_frame;
    denoiser_normal /= (float)render_data.render_settings.samples_per_frame;

    render_data.aux_buffers.denoiser_albedo[pixel_index] = (render_data.aux_buffers.denoiser_albedo[pixel_index] * render_data.render_settings.frame_number + denoiser_albedo) / (render_data.render_settings.frame_number + 1.0f);

    float3 accumulated_normal = (render_data.aux_buffers.denoiser_normals[pixel_index] * render_data.render_settings.frame_number + denoiser_normal) / (render_data.render_settings.frame_number + 1.0f);
    float normal_length = hippt::length(accumulated_normal);
    if (normal_length != 0.0f)
        // Checking that it is non-zero otherwise we would accumulate a persistent NaN in the buffer when normalizing by the 0-length
        render_data.aux_buffers.denoiser_normals[pixel_index] = accumulated_normal / normal_length;
}
