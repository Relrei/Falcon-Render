/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef KERNEL_STRUCT_BEGIN
#  define KERNEL_STRUCT_BEGIN(name, parent)
#endif
#ifndef KERNEL_STRUCT_END
#  define KERNEL_STRUCT_END(name)
#endif
#ifndef KERNEL_STRUCT_MEMBER
#  define KERNEL_STRUCT_MEMBER(parent, type, name)
#endif
#ifndef KERNEL_STRUCT_MEMBER_DONT_SPECIALIZE
#  define KERNEL_STRUCT_MEMBER_DONT_SPECIALIZE
#endif

/* Background. */

KERNEL_STRUCT_BEGIN(KernelBackground, background)
/* xyz store direction, w the angle. float4 instead of float3 is used
 * to ensure consistent padding/alignment across devices. */
KERNEL_STRUCT_MEMBER(background, float4, sun)
KERNEL_STRUCT_MEMBER(background, int, use_sun_guiding)
/* Only shader index. */
KERNEL_STRUCT_MEMBER(background, int, surface_shader)
KERNEL_STRUCT_MEMBER(background, int, volume_shader)
KERNEL_STRUCT_MEMBER(background, int, transparent)
KERNEL_STRUCT_MEMBER(background, float, transparent_roughness_squared_threshold)
/* Sun sampling. */
KERNEL_STRUCT_MEMBER(background, float, sun_weight)
/* Importance map sampling. */
KERNEL_STRUCT_MEMBER(background, float, map_weight)
KERNEL_STRUCT_MEMBER(background, float, portal_weight)
KERNEL_STRUCT_MEMBER(background, int, map_res_x)
KERNEL_STRUCT_MEMBER(background, int, map_res_y)
/* Multiple importance sampling. */
KERNEL_STRUCT_MEMBER(background, int, use_mis)
/* Light-group. */
KERNEL_STRUCT_MEMBER(background, int, lightgroup)
/* Light Index. */
KERNEL_STRUCT_MEMBER(background, int, light_index)
/* Object Index. */
KERNEL_STRUCT_MEMBER(background, int, object_index)
/* Padding. */
KERNEL_STRUCT_MEMBER(background, int, pad1)
KERNEL_STRUCT_MEMBER(background, int, pad2)
KERNEL_STRUCT_END(KernelBackground)

/* BVH: own BVH2 if no native device acceleration struct used. */

KERNEL_STRUCT_BEGIN(KernelBVH, bvh)
KERNEL_STRUCT_MEMBER(bvh, int, root)
KERNEL_STRUCT_MEMBER(bvh, int, have_motion)
KERNEL_STRUCT_MEMBER(bvh, int, have_curves)
KERNEL_STRUCT_MEMBER(bvh, int, have_points)
KERNEL_STRUCT_MEMBER(bvh, int, have_volumes)
KERNEL_STRUCT_MEMBER(bvh, int, bvh_layout)
KERNEL_STRUCT_MEMBER(bvh, int, use_bvh_steps)
KERNEL_STRUCT_MEMBER(bvh, int, curve_subdivisions)
KERNEL_STRUCT_END(KernelBVH)

/* Film. */

KERNEL_STRUCT_BEGIN(KernelFilm, film)
/* XYZ to rendering color space transform. float4 instead of float3 to
 * ensure consistent padding/alignment across devices. */
KERNEL_STRUCT_MEMBER(film, float4, xyz_to_r)
KERNEL_STRUCT_MEMBER(film, float4, xyz_to_g)
KERNEL_STRUCT_MEMBER(film, float4, xyz_to_b)
KERNEL_STRUCT_MEMBER(film, float4, rgb_to_y)
KERNEL_STRUCT_MEMBER(film, float4, white_xyz)
/* Rec709 to rendering color space. */
KERNEL_STRUCT_MEMBER(film, float4, rec709_to_r)
KERNEL_STRUCT_MEMBER(film, float4, rec709_to_g)
KERNEL_STRUCT_MEMBER(film, float4, rec709_to_b)
KERNEL_STRUCT_MEMBER(film, int, is_rec709)
/* Exposure. */
KERNEL_STRUCT_MEMBER(film, float, exposure)
/* Passed used. */
KERNEL_STRUCT_MEMBER(film, int, pass_flag)
KERNEL_STRUCT_MEMBER(film, int, light_pass_flag)
/* Pass offsets. */
KERNEL_STRUCT_MEMBER(film, int, pass_stride)
KERNEL_STRUCT_MEMBER(film, int, pass_combined)
KERNEL_STRUCT_MEMBER(film, int, pass_depth)
KERNEL_STRUCT_MEMBER(film, int, pass_position)
KERNEL_STRUCT_MEMBER(film, int, pass_normal)
KERNEL_STRUCT_MEMBER(film, int, pass_roughness)
KERNEL_STRUCT_MEMBER(film, int, pass_motion)
KERNEL_STRUCT_MEMBER(film, int, pass_motion_weight)
KERNEL_STRUCT_MEMBER(film, int, pass_uv)
KERNEL_STRUCT_MEMBER(film, int, pass_object_id)
KERNEL_STRUCT_MEMBER(film, int, pass_material_id)
KERNEL_STRUCT_MEMBER(film, int, pass_diffuse_color)
KERNEL_STRUCT_MEMBER(film, int, pass_glossy_color)
KERNEL_STRUCT_MEMBER(film, int, pass_transmission_color)
KERNEL_STRUCT_MEMBER(film, int, pass_diffuse_indirect)
KERNEL_STRUCT_MEMBER(film, int, pass_glossy_indirect)
KERNEL_STRUCT_MEMBER(film, int, pass_transmission_indirect)
KERNEL_STRUCT_MEMBER(film, int, pass_volume_indirect)
KERNEL_STRUCT_MEMBER(film, int, pass_diffuse_direct)
KERNEL_STRUCT_MEMBER(film, int, pass_glossy_direct)
KERNEL_STRUCT_MEMBER(film, int, pass_transmission_direct)
KERNEL_STRUCT_MEMBER(film, int, pass_volume_direct)
KERNEL_STRUCT_MEMBER(film, int, pass_volume_scatter)
KERNEL_STRUCT_MEMBER(film, int, pass_volume_scatter_denoised)
KERNEL_STRUCT_MEMBER(film, int, pass_volume_transmit)
KERNEL_STRUCT_MEMBER(film, int, pass_volume_transmit_denoised)
KERNEL_STRUCT_MEMBER(film, int, pass_volume_majorant)
KERNEL_STRUCT_MEMBER(film, int, pass_volume_majorant_sample_count)
KERNEL_STRUCT_MEMBER(film, int, pass_emission)
KERNEL_STRUCT_MEMBER(film, int, pass_background)
KERNEL_STRUCT_MEMBER(film, int, pass_ao)
KERNEL_STRUCT_MEMBER(film, float, pass_alpha_threshold)
KERNEL_STRUCT_MEMBER(film, int, pass_shadow_catcher)
KERNEL_STRUCT_MEMBER(film, int, pass_shadow_catcher_sample_count)
KERNEL_STRUCT_MEMBER(film, int, pass_shadow_catcher_matte)
KERNEL_STRUCT_MEMBER(film, int, pass_render_time)
/* Cryptomatte. */
KERNEL_STRUCT_MEMBER(film, int, cryptomatte_passes)
KERNEL_STRUCT_MEMBER(film, int, cryptomatte_depth)
KERNEL_STRUCT_MEMBER(film, int, pass_cryptomatte)
/* Adaptive sampling. */
KERNEL_STRUCT_MEMBER(film, int, pass_adaptive_aux_buffer)
KERNEL_STRUCT_MEMBER(film, int, pass_sample_count)
/* Mist. */
KERNEL_STRUCT_MEMBER(film, int, pass_mist)
KERNEL_STRUCT_MEMBER(film, float, mist_start)
KERNEL_STRUCT_MEMBER(film, float, mist_inv_depth)
KERNEL_STRUCT_MEMBER(film, float, mist_falloff)
/* Denoising. */
KERNEL_STRUCT_MEMBER(film, int, pass_denoising_albedo)
KERNEL_STRUCT_MEMBER(film, int, pass_denoising_normal)
KERNEL_STRUCT_MEMBER(film, int, pass_denoising_depth)
/* AOVs. */
KERNEL_STRUCT_MEMBER(film, int, pass_aov_color)
KERNEL_STRUCT_MEMBER(film, int, pass_aov_value)
/* Light groups. */
KERNEL_STRUCT_MEMBER(film, int, pass_lightgroup)
/* Baking. */
KERNEL_STRUCT_MEMBER(film, int, pass_bake_primitive)
KERNEL_STRUCT_MEMBER(film, int, pass_bake_seed)
KERNEL_STRUCT_MEMBER(film, int, pass_bake_differential)
/* Shadow catcher. */
KERNEL_STRUCT_MEMBER(film, int, use_approximate_shadow_catcher)
/* Path Guiding */
KERNEL_STRUCT_MEMBER(film, int, pass_guiding_color)
KERNEL_STRUCT_MEMBER(film, int, pass_guiding_probability)
KERNEL_STRUCT_MEMBER(film, int, pass_guiding_avg_roughness)
KERNEL_STRUCT_END(KernelFilm)

/* Integrator. */

KERNEL_STRUCT_BEGIN(KernelIntegrator, integrator)
/* Emission. */
KERNEL_STRUCT_MEMBER(integrator, int, use_direct_light)
KERNEL_STRUCT_MEMBER(integrator, int, use_light_mis)
KERNEL_STRUCT_MEMBER(integrator, int, use_light_tree)
KERNEL_STRUCT_MEMBER(integrator, int, num_lights)
KERNEL_STRUCT_MEMBER(integrator, int, num_distant_lights)
KERNEL_STRUCT_MEMBER(integrator, int, num_background_lights)
/* Portal sampling. */
KERNEL_STRUCT_MEMBER(integrator, int, num_portals)
KERNEL_STRUCT_MEMBER(integrator, int, portal_offset)
/* Flat light distribution. */
KERNEL_STRUCT_MEMBER(integrator, int, num_distribution)
KERNEL_STRUCT_MEMBER(integrator, float, distribution_pdf_triangles)
KERNEL_STRUCT_MEMBER(integrator, float, distribution_pdf_lights)
KERNEL_STRUCT_MEMBER(integrator, float, light_inv_rr_threshold)
/* Bounces. */
KERNEL_STRUCT_MEMBER(integrator, int, min_bounce)
KERNEL_STRUCT_MEMBER(integrator, int, max_bounce)
KERNEL_STRUCT_MEMBER(integrator, int, max_diffuse_bounce)
KERNEL_STRUCT_MEMBER(integrator, int, max_glossy_bounce)
KERNEL_STRUCT_MEMBER(integrator, int, max_transmission_bounce)
KERNEL_STRUCT_MEMBER(integrator, int, max_volume_bounce)
/* AO bounces. */
KERNEL_STRUCT_MEMBER(integrator, int, ao_bounces)
KERNEL_STRUCT_MEMBER(integrator, float, ao_bounces_distance)
KERNEL_STRUCT_MEMBER(integrator, float, ao_bounces_factor)
KERNEL_STRUCT_MEMBER(integrator, float, ao_additive_factor)
/* Transparency. */
KERNEL_STRUCT_MEMBER(integrator, int, transparent_min_bounce)
KERNEL_STRUCT_MEMBER(integrator, int, transparent_max_bounce)
KERNEL_STRUCT_MEMBER(integrator, int, transparent_shadows)
/* Caustics. */
KERNEL_STRUCT_MEMBER(integrator, int, caustics_reflective)
KERNEL_STRUCT_MEMBER(integrator, int, caustics_refractive)
KERNEL_STRUCT_MEMBER(integrator, float, filter_glossy)
/* Seed. */
KERNEL_STRUCT_MEMBER_DONT_SPECIALIZE
KERNEL_STRUCT_MEMBER(integrator, int, seed)
/* Clamp. */
KERNEL_STRUCT_MEMBER(integrator, float, sample_clamp_direct)
KERNEL_STRUCT_MEMBER(integrator, float, sample_clamp_indirect)
/* Caustics. */
KERNEL_STRUCT_MEMBER(integrator, int, use_caustics)
/* Sampling pattern. */
KERNEL_STRUCT_MEMBER(integrator, int, sampling_pattern)
KERNEL_STRUCT_MEMBER(integrator, float, scrambling_distance)
/* Sobol pattern. */
KERNEL_STRUCT_MEMBER_DONT_SPECIALIZE
KERNEL_STRUCT_MEMBER(integrator, int, tabulated_sobol_sequence_size)
KERNEL_STRUCT_MEMBER_DONT_SPECIALIZE
KERNEL_STRUCT_MEMBER(integrator, int, sobol_index_mask)
KERNEL_STRUCT_MEMBER_DONT_SPECIALIZE
KERNEL_STRUCT_MEMBER(integrator, int, blue_noise_sequence_length)
/* Volume render. */
KERNEL_STRUCT_MEMBER(integrator, int, use_volumes)
KERNEL_STRUCT_MEMBER(integrator, int, volume_ray_marching)
KERNEL_STRUCT_MEMBER(integrator, int, volume_max_steps)
/* Shadow catcher. */
KERNEL_STRUCT_MEMBER(integrator, int, has_shadow_catcher)
/* Closure filter. */
KERNEL_STRUCT_MEMBER(integrator, int, filter_closures)
/* MIS debugging. */
KERNEL_STRUCT_MEMBER(integrator, int, direct_light_sampling_type)
/* Path Guiding */
KERNEL_STRUCT_MEMBER(integrator, float, surface_guiding_probability)
KERNEL_STRUCT_MEMBER(integrator, float, volume_guiding_probability)
KERNEL_STRUCT_MEMBER(integrator, int, guiding_distribution_type)
KERNEL_STRUCT_MEMBER(integrator, int, guiding_directional_sampling_type)
KERNEL_STRUCT_MEMBER(integrator, float, guiding_roughness_threshold)
KERNEL_STRUCT_MEMBER(integrator, int, use_guiding)
KERNEL_STRUCT_MEMBER(integrator, int, train_guiding)
KERNEL_STRUCT_MEMBER(integrator, int, use_surface_guiding)
KERNEL_STRUCT_MEMBER(integrator, int, use_volume_guiding)
KERNEL_STRUCT_MEMBER(integrator, int, use_guiding_direct_light)
KERNEL_STRUCT_MEMBER(integrator, int, use_guiding_mis_weights)

/* Falcon SHARC. `falcon_sharc_active` is 1 only when FALCON_SHARC_MODE selects
 * warmup/blend; it gates the in-kernel blend so a normal render is a complete
 * no-op (no cache lookup at all). `falcon_sharc_alpha` is the runtime blend
 * factor (FALCON_SHARC_ALPHA), tunable without a kernel recompile. */
#ifdef WITH_FALCON_SHARC
KERNEL_STRUCT_MEMBER(integrator, int, falcon_sharc_active)
KERNEL_STRUCT_MEMBER(integrator, float, falcon_sharc_alpha)
/* Falcon DAS: gates the per-pixel threshold scale in the adaptive sampling
 * convergence check. `falcon_das_strength` lerps the map's effect (0 = off,
 * 1 = map as-is) so it can be tuned without rebuilding the map. */
KERNEL_STRUCT_MEMBER(integrator, int, falcon_das_active)
KERNEL_STRUCT_MEMBER(integrator, float, falcon_das_strength)
/* Falcon Photon Cache: additive caustic lookup at any surface vertex (the
 * cache then holds photon-estimated caustic radiance, not path radiance).
 * `falcon_sharc_cell_size` makes the grid resolution a runtime knob shared by
 * SHARC and the photon cache (FALCON_SHARC_CELL, default 0.2). */
KERNEL_STRUCT_MEMBER(integrator, int, falcon_photon_add)
KERNEL_STRUCT_MEMBER(integrator, float, falcon_sharc_cell_size)
/* Falcon Photon GPU bake pass: camera rays are replaced by light-emitted
 * photons that deposit caustic radiance into the (const-cast) sharc cache at
 * their first diffuse hit. flux = per-photon power (host: light watts * k /
 * photon count). Sun photons launch as parallel rays over the scene-footprint
 * rectangle at z = sun_z. 8 members keep the falcon block a multiple of 4. */
KERNEL_STRUCT_MEMBER(integrator, int, falcon_photon_pass)
KERNEL_STRUCT_MEMBER(integrator, float, falcon_photon_flux)
KERNEL_STRUCT_MEMBER(integrator, int, falcon_photon_is_sun)
KERNEL_STRUCT_MEMBER(integrator, float, falcon_photon_sun_minx)
KERNEL_STRUCT_MEMBER(integrator, float, falcon_photon_sun_miny)
KERNEL_STRUCT_MEMBER(integrator, float, falcon_photon_sun_sizex)
KERNEL_STRUCT_MEMBER(integrator, float, falcon_photon_sun_sizey)
KERNEL_STRUCT_MEMBER(integrator, float, falcon_photon_sun_z)
/* Falcon Dispersion v0: global Cauchy-B strength (um^2) applied to every
 * smooth refractive microfacet closure; 0 disables (FALCON_DISPERSION.md).
 * Paired with falcon_pad0 to keep the falcon block a multiple of 4. */
KERNEL_STRUCT_MEMBER(integrator, float, falcon_dispersion_b)
/* Falcon Photon map: caustic deposit radius in cells (fixed-radius kernel
 * density estimation; 0/1 = legacy 2x2x2 splat). Wider = smoother caustics
 * from fewer photons, at more deposits/photon during the bake. */
KERNEL_STRUCT_MEMBER(integrator, float, falcon_photon_radius)
/* Falcon Photon POINT map (Round 9, LuxCore-faithful): photons stored as raw
 * points (pos/flux/normal), density estimated at lookup over a host-built
 * neighbor grid. point_store gates the bake-time append; point_mode gates the
 * add-time gather. radius (m), cos (normal-angle rejection) and gain are
 * RENDER-TIME knobs -- retuning them never needs a rebake. 8 members keep the
 * falcon block a multiple of 4. */
KERNEL_STRUCT_MEMBER(integrator, int, falcon_photon_point_store)
KERNEL_STRUCT_MEMBER(integrator, int, falcon_photon_point_max)
KERNEL_STRUCT_MEMBER(integrator, int, falcon_photon_point_mode)
KERNEL_STRUCT_MEMBER(integrator, float, falcon_photon_point_radius)
KERNEL_STRUCT_MEMBER(integrator, float, falcon_photon_point_cos)
KERNEL_STRUCT_MEMBER(integrator, float, falcon_photon_point_gain)
/* Spot photon emission: is_spot selects cone emission from the light origin
 * (uniform in solid angle; flux = watts * Omega/(4 pi) / N, i.e. the point
 * light's intensity W/(4 pi) integrated over the cone). */
KERNEL_STRUCT_MEMBER(integrator, int, falcon_photon_is_spot)
/* Falcon Light Tracing (FQ, cache-free caustics): when set, the photon bake
 * pass splats the first-diffuse-hit flux DIRECTLY to the camera pixel (project
 * + connect) instead of depositing into the map. gain is a labeled brightness
 * knob until the camera importance We is calibrated. 4 members keep the block a
 * multiple of 4. */
KERNEL_STRUCT_MEMBER(integrator, int, falcon_lighttrace)
KERNEL_STRUCT_MEMBER(integrator, float, falcon_lighttrace_gain)
KERNEL_STRUCT_MEMBER(integrator, int, falcon_lt_direct)
KERNEL_STRUCT_MEMBER(integrator, int, falcon_lt_samples)
/* Light-tracing performance knobs (labeled non-physical): splat_radius (px,
 * 0 = physical single-pixel splat) spreads each splat over an energy-
 * normalized Gaussian footprint so few photons/SPP look smooth. visibility
 * gates the vertex->camera occlusion ray (reserved, wired in the next step).
 * 4 members keep the block a multiple of 4. */
KERNEL_STRUCT_MEMBER(integrator, float, falcon_lt_splat_radius)
KERNEL_STRUCT_MEMBER(integrator, int, falcon_lt_visibility)
KERNEL_STRUCT_MEMBER(integrator, int, falcon_lt_pad1)
KERNEL_STRUCT_MEMBER(integrator, int, falcon_lt_pad2)
#endif

/* Padding. */
KERNEL_STRUCT_MEMBER(integrator, int, pad1)
KERNEL_STRUCT_MEMBER(integrator, int, pad2)
KERNEL_STRUCT_END(KernelIntegrator)

/* SVM. For shader specialization. */

KERNEL_STRUCT_BEGIN(KernelSVMUsage, svm_usage)
#define SHADER_NODE_TYPE(type) KERNEL_STRUCT_MEMBER(svm_usage, int, type)
#include "kernel/svm/node_types_template.h"
KERNEL_STRUCT_END(KernelSVMUsage)

#undef KERNEL_STRUCT_BEGIN
#undef KERNEL_STRUCT_MEMBER
#undef KERNEL_STRUCT_MEMBER_DONT_SPECIALIZE
#undef KERNEL_STRUCT_END
