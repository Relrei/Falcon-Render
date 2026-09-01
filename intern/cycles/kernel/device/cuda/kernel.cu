/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* CUDA kernel entry points */

#ifdef __CUDA_ARCH__

#  include "kernel/device/cuda/compat.h"
#  include "kernel/device/cuda/config.h"
#  include "kernel/device/cuda/globals.h"

#  include "kernel/device/gpu/image.h"
#  include "kernel/device/gpu/kernel.h"

/* --------------------------------------------------------------------
 * Denoising.
 */

ccl_gpu_kernel(GPU_KERNEL_BLOCK_NUM_THREADS, GPU_KERNEL_MAX_REGISTERS)
    ccl_gpu_kernel_signature(filter_color_preprocess_to_surface,
                             cudaSurfaceObject_t color_surface,
                             cudaSurfaceObject_t color_before_transparency_surface,
                             cudaSurfaceObject_t color_before_particles_surface,
                             cudaSurfaceObject_t color_before_fog_surface,
                             const int layer_guides,
                             const int pass_volume_direct,
                             const int pass_volume_indirect,
                             ccl_global float *render_buffer,
                             const int full_x,
                             const int full_y,
                             const int width,
                             const int height,
                             const int offset,
                             const int stride,
                             const int pass_stride,
                             const int pass_denoised,
                             const int pass_sample_count,
                             const int pass_transmission_direct,
                             const int pass_transmission_indirect,
                             const int num_samples,
                             const int pass_albedo,
                             const int pass_specular_albedo,
                             const int demodulate,
                             const float color_scale)
{
  const int work_index = ccl_gpu_global_id_x();
  const int y = work_index / width;
  const int x = work_index - y * width;

  if (x >= width || y >= height) {
    return;
  }

  const uint64_t render_pixel_index = offset + (x + full_x) + (y + full_y) * stride;
  ccl_global float *denoised_pixel = render_buffer + render_pixel_index * pass_stride +
                                     pass_denoised;

  float4 color_value;
  color_value.x = denoised_pixel[0] * color_scale;
  color_value.y = denoised_pixel[1] * color_scale;
  color_value.z = denoised_pixel[2] * color_scale;
  color_value.w = 1.0;

  surf2Dwrite(color_value, color_surface, x * sizeof(float4), y);

  /* Demodulation sweep (FALCON_DLSS_DEMOD): divide by the summed albedo guides
   * (both are per-sample sums, so scale by the sample count). Overwrites the
   * colour written above; the before-transparency guide keeps the modulated
   * colour so its 'no transmission => bit identical' contract is unaffected. */
  if (demodulate && pass_albedo != PASS_UNUSED && pass_specular_albedo != PASS_UNUSED) {
    const ccl_global float *buffer = render_buffer + render_pixel_index * pass_stride;
    const float inv_samples = (pass_sample_count == PASS_UNUSED) ?
                                  1.0f / num_samples :
                                  1.0f / __float_as_uint(buffer[pass_sample_count]);
    const ccl_global float *a = buffer + pass_albedo;
    const ccl_global float *sa = buffer + pass_specular_albedo;
    float4 demod = color_value;
    demod.x /= max((a[0] + sa[0]) * inv_samples, 0.01f);
    demod.y /= max((a[1] + sa[1]) * inv_samples, 0.01f);
    demod.z /= max((a[2] + sa[2]) * inv_samples, 0.01f);
    surf2Dwrite(demod, color_surface, x * sizeof(float4), y);
  }

  /* ColorBeforeTransparency guide: the color with the transmission contribution
   * removed. Pixels without transmission stay bit-identical to the color input,
   * which is required for RR to treat them as fully opaque. */
  float4 before_value = color_value;
  if (pass_transmission_direct != PASS_UNUSED && pass_transmission_indirect != PASS_UNUSED) {
    const ccl_global float *buffer = render_buffer + render_pixel_index * pass_stride;

    float pixel_scale;
    if (pass_sample_count == PASS_UNUSED) {
      pixel_scale = 1.0f / num_samples;
    }
    else {
      pixel_scale = 1.0f / __float_as_uint(buffer[pass_sample_count]);
    }

    const ccl_global float *transmission_direct = buffer + pass_transmission_direct;
    const ccl_global float *transmission_indirect = buffer + pass_transmission_indirect;

    pixel_scale *= color_scale;
    before_value.x = max(
        before_value.x - (transmission_direct[0] + transmission_indirect[0]) * pixel_scale, 0.0f);
    before_value.y = max(
        before_value.y - (transmission_direct[1] + transmission_indirect[1]) * pixel_scale, 0.0f);
    before_value.z = max(
        before_value.z - (transmission_direct[2] + transmission_indirect[2]) * pixel_scale, 0.0f);
  }

  surf2Dwrite(before_value, color_before_transparency_surface, x * sizeof(float4), y);

  /* FALCON_DLSS_LAYER_GUIDES: ColorBeforeParticles = the colour itself (Cycles
   * has no particle layer), ColorBeforeFog = colour minus the volume passes
   * (colour itself when the passes are missing). */
  if (layer_guides) {
    surf2Dwrite(color_value, color_before_particles_surface, x * sizeof(float4), y);
    float4 fog_value = color_value;
    if (pass_volume_direct != PASS_UNUSED && pass_volume_indirect != PASS_UNUSED) {
      const ccl_global float *buffer = render_buffer + render_pixel_index * pass_stride;
      float pixel_scale = (pass_sample_count == PASS_UNUSED) ?
                              1.0f / num_samples :
                              1.0f / __float_as_uint(buffer[pass_sample_count]);
      pixel_scale *= color_scale;
      const ccl_global float *vd = buffer + pass_volume_direct;
      const ccl_global float *vi = buffer + pass_volume_indirect;
      fog_value.x = max(fog_value.x - (vd[0] + vi[0]) * pixel_scale, 0.0f);
      fog_value.y = max(fog_value.y - (vd[1] + vi[1]) * pixel_scale, 0.0f);
      fog_value.z = max(fog_value.z - (vd[2] + vi[2]) * pixel_scale, 0.0f);
    }
    surf2Dwrite(fog_value, color_before_fog_surface, x * sizeof(float4), y);
  }
}
ccl_gpu_kernel_postfix

ccl_gpu_kernel(GPU_KERNEL_BLOCK_NUM_THREADS, GPU_KERNEL_MAX_REGISTERS)
    ccl_gpu_kernel_signature(filter_guiding_preprocess_to_surface,
                             cudaSurfaceObject_t depth_surface,
                             cudaSurfaceObject_t albedo_surface,
                             cudaSurfaceObject_t specular_albedo_surface,
                             cudaSurfaceObject_t normal_roughness_surface,
                             cudaSurfaceObject_t motion_surface,
                             cudaSurfaceObject_t specular_motion_surface,
                             cudaSurfaceObject_t specular_hit_distance_surface,
                             const int render_pass_specular_hit_distance,
                             cudaSurfaceObject_t emissive_surface,
                             const int render_pass_emission,
                             const ccl_global float *render_buffer,
                             const int render_offset,
                             const int render_stride,
                             const int render_pass_stride,
                             const int render_pass_sample_count,
                             const int render_pass_depth,
                             const int render_pass_albedo,
                             const int render_pass_specular_albedo,
                             const int render_pass_normal,
                             const int render_pass_roughness,
                             const int render_pass_motion,
                             const int render_pass_motion_weight,
                             const int render_pass_specular_motion,
                             const int full_x,
                             const int full_y,
                             const int width,
                             const int height,
                             const int num_samples,
                             const int zero_motion,
                             const int normalize_normal,
                             const float depth_far)
{
  const int work_index = ccl_gpu_global_id_x();
  const int y = work_index / width;
  const int x = work_index - y * width;

  if (x >= width || y >= height) {
    return;
  }

  const uint64_t render_pixel_index = render_offset + (x + full_x) + (y + full_y) * render_stride;
  const ccl_global float *buffer = render_buffer + render_pixel_index * render_pass_stride;

  float pixel_scale;
  if (render_pass_sample_count == PASS_UNUSED) {
    pixel_scale = 1.0f / num_samples;
  }
  else {
    pixel_scale = 1.0f / __float_as_uint(buffer[render_pass_sample_count]);
  }

  /* Depth pass. */
  if (render_pass_depth != PASS_UNUSED) {
    const ccl_global float *depth_in = buffer + render_pass_depth;

    float depth_value = depth_in[0] * pixel_scale;

    /* The background writes FLT_MAX into the depth pass, and this divide turns it into
     * FLT_MAX / samples -- so the sky's "depth" is not even a constant: it moves with the sample
     * count, which under adaptive sampling and a time limit differs from frame to frame (measured
     * 2.0e37 and 1.06e37 on the same frame of the tree scene). RR is told the depth is linear, so
     * hand it a finite far value instead of a number no scene ever reaches. depth_far <= 0 keeps
     * the raw sentinel (FALCON_DLSS_DEPTH_FAR=0). */
    if (depth_far > 0.0f) {
      depth_value = fminf(depth_value, depth_far);
    }

    surf2Dwrite(depth_value, depth_surface, x * sizeof(float), y);
  }

  /* Diffuse albedo pass. */
  if (render_pass_albedo != PASS_UNUSED) {
    const ccl_global float *albedo_in = buffer + render_pass_albedo;

    float4 albedo_value;
    albedo_value.x = albedo_in[0] * pixel_scale;
    albedo_value.y = albedo_in[1] * pixel_scale;
    albedo_value.z = albedo_in[2] * pixel_scale;
    albedo_value.w = 1.0;

    /* Tonemap the albedo with simple reinhard operator. */
    albedo_value.x = clamp(albedo_value.x / (1.0f + albedo_value.x), 0.0f, 1.0f);
    albedo_value.y = clamp(albedo_value.y / (1.0f + albedo_value.y), 0.0f, 1.0f);
    albedo_value.z = clamp(albedo_value.z / (1.0f + albedo_value.z), 0.0f, 1.0f);

    surf2Dwrite(albedo_value, albedo_surface, x * sizeof(float4), y);
  }

  /* Specular albedo pass. */
  if (render_pass_specular_albedo != PASS_UNUSED) {
    const ccl_global float *albedo_in = buffer + render_pass_specular_albedo;

    float4 specular_albedo_value;
    specular_albedo_value.x = albedo_in[0] * pixel_scale;
    specular_albedo_value.y = albedo_in[1] * pixel_scale;
    specular_albedo_value.z = albedo_in[2] * pixel_scale;
    specular_albedo_value.w = 1.0;

    surf2Dwrite(specular_albedo_value, specular_albedo_surface, x * sizeof(float4), y);
  }

  /* Normal and roughness pass. */
  if (render_pass_normal != PASS_UNUSED && render_pass_roughness != PASS_UNUSED) {
    const ccl_global float *normal_in = buffer + render_pass_normal;
    const ccl_global float *roughness_in = buffer + render_pass_roughness;

    /* The pass holds the sum of per-sample unit normals, so the mean is shorter
     * than one wherever the normals inside a pixel disagree (an edge, a particle,
     * a volume) or where only some samples wrote one. RR reads the guide as a
     * direction and expects it unit length: a half-length normal reads as a
     * different surface, which is what let thin geometry drift. Restore the
     * length and rescale the roughness that was averaged the same way. */
    float4 normal_roughness_value;
    normal_roughness_value.x = normal_in[0] * pixel_scale;
    normal_roughness_value.y = normal_in[1] * pixel_scale;
    normal_roughness_value.z = normal_in[2] * pixel_scale;
    normal_roughness_value.w = roughness_in[0] * pixel_scale;

    const float normal_length = sqrtf(normal_roughness_value.x * normal_roughness_value.x +
                                      normal_roughness_value.y * normal_roughness_value.y +
                                      normal_roughness_value.z * normal_roughness_value.z);
    if (normalize_normal && normal_length > 1e-4f) {
      const float inv_length = 1.0f / normal_length;
      normal_roughness_value.x *= inv_length;
      normal_roughness_value.y *= inv_length;
      normal_roughness_value.z *= inv_length;
      normal_roughness_value.w = min(normal_roughness_value.w * inv_length, 1.0f);
    }

    surf2Dwrite(normal_roughness_value, normal_roughness_surface, x * sizeof(float4), y);
  }

  /* Motion pass.
   *
   * The motion vector pass stores inter-frame motion, which is only the correct
   * history alignment for the first denoise after a frame change. Re-denoising
   * the same frame (progressive accumulation) must use zero motion, otherwise
   * the history is re-warped by the same vector every round and smears along
   * the motion direction. The host passes zero_motion accordingly, and we
   * always write so the surface contents are defined. */
  {
    float2 motion_value;
    motion_value.x = 0.0f;
    motion_value.y = 0.0f;
    if (!zero_motion && render_pass_motion != PASS_UNUSED) {
      const ccl_global float *motion_in = buffer + render_pass_motion;
      /* The motion pass is summed only over the samples that actually found a
       * surface to move, and Cycles divides it by its own weight pass (see
       * PassInfo::divide_type). Dividing by the sample count instead shortens
       * the vector by the fraction of samples that wrote nothing -- a particle
       * covering half the pixel, a path that ended inside smoke, an edge. RR
       * then warps the history to a place short of where the pixel went. */
      float motion_scale = pixel_scale;
      if (render_pass_motion_weight != PASS_UNUSED) {
        const float motion_weight = buffer[render_pass_motion_weight];
        motion_scale = (motion_weight > 0.0f) ? 1.0f / motion_weight : 0.0f;
      }
      motion_value.x = motion_in[0] * motion_scale;
      motion_value.y = motion_in[1] * motion_scale;
    }
    surf2Dwrite(motion_value, motion_surface, x * sizeof(float2), y);
  }

  /* Specular motion pass. */
  {
    float2 motion_value;
    motion_value.x = 0.0f;
    motion_value.y = 0.0f;
    if (!zero_motion && render_pass_specular_motion != PASS_UNUSED) {
      const ccl_global float *motion_in = buffer + render_pass_specular_motion;
      motion_value.x = motion_in[0] * pixel_scale;
      motion_value.y = motion_in[1] * pixel_scale;
    }
    surf2Dwrite(motion_value, specular_motion_surface, x * sizeof(float2), y);
  }

  /* Specular hit distance. Only the samples whose primary bounce was glossy
   * wrote to it, so the mean is over their count, not over all samples. Pixels
   * where nothing reflected stay at zero, which RR reads as "no specular hit". */
  {
    float hit_distance = 0.0f;
    if (render_pass_specular_hit_distance != PASS_UNUSED) {
      const ccl_global float *in = buffer + render_pass_specular_hit_distance;
      const float sample_count = in[1];
      if (sample_count > 0.0f) {
        hit_distance = in[0] / sample_count;
      }
    }
    surf2Dwrite(hit_distance, specular_hit_distance_surface, x * sizeof(float), y);
  }

  /* Emissive G-buffer: the surface's own emission, so RR reads a lamp or a screen as
   * material instead of averaging it with its neighbours as if it were noisy lighting.
   * Written whenever the pass exists (FALCON_DLSS_EMISSIVE_GUIDE=1); the surface is
   * otherwise never handed to RR, so leaving it untouched is fine. */
  if (render_pass_emission != PASS_UNUSED) {
    const ccl_global float *emission_in = buffer + render_pass_emission;

    float4 emissive_value;
    emissive_value.x = emission_in[0] * pixel_scale;
    emissive_value.y = emission_in[1] * pixel_scale;
    emissive_value.z = emission_in[2] * pixel_scale;
    emissive_value.w = 1.0f;

    surf2Dwrite(emissive_value, emissive_surface, x * sizeof(float4), y);
  }
}
ccl_gpu_kernel_postfix

ccl_gpu_kernel(GPU_KERNEL_BLOCK_NUM_THREADS, GPU_KERNEL_MAX_REGISTERS)
    ccl_gpu_kernel_signature(filter_color_postprocess_from_surface,
                             cudaSurfaceObject_t color_surface,
                             ccl_global float *render_buffer,
                             const int full_x,
                             const int full_y,
                             const int width,
                             const int height,
                             const int offset,
                             const int stride,
                             const int render_full_x,
                             const int render_full_y,
                             const int render_offset,
                             const int render_stride,
                             const int pass_stride,
                             const int num_samples,
                             const int pass_noisy,
                             const int pass_denoised,
                             const int pass_sample_count,
                             const int num_components,
                             const int use_compositing,
                             const float upscale_factor,
                             const int pass_albedo,
                             const int pass_specular_albedo,
                             const int demodulate,
                             const float inv_color_scale)
{
  const int work_index = ccl_gpu_global_id_x();
  const int y = work_index / width;
  const int x = work_index - y * width;

  if (x >= width || y >= height) {
    return;
  }

  const uint64_t render_pixel_index = render_offset + (int(x / upscale_factor) + render_full_x) +
                                      (int(y / upscale_factor) + render_full_y) * render_stride;
  ccl_global float *buffer = render_buffer + render_pixel_index * pass_stride;

  float pixel_scale;
  if (pass_sample_count == PASS_UNUSED) {
    pixel_scale = num_samples;
  }
  else {
    pixel_scale = __float_as_uint(buffer[pass_sample_count]);
  }

  const uint64_t denoised_pixel_index = offset + (x + full_x) + (y + full_y) * stride;
  ccl_global float *denoised_pixel = render_buffer + denoised_pixel_index * pass_stride +
                                     pass_denoised;

  float4 color_value;
  surf2Dread(&color_value, color_surface, x * sizeof(float4), y);

  denoised_pixel[0] = color_value.x * inv_color_scale;
  denoised_pixel[1] = color_value.y * inv_color_scale;
  denoised_pixel[2] = color_value.z * inv_color_scale;

  /* Re-modulate (see filter_color_preprocess_to_surface). */
  if (demodulate && pass_albedo != PASS_UNUSED && pass_specular_albedo != PASS_UNUSED) {
    const float inv_samples = 1.0f / pixel_scale;
    const ccl_global float *a = buffer + pass_albedo;
    const ccl_global float *sa = buffer + pass_specular_albedo;
    denoised_pixel[0] *= max((a[0] + sa[0]) * inv_samples, 0.01f);
    denoised_pixel[1] *= max((a[1] + sa[1]) * inv_samples, 0.01f);
    denoised_pixel[2] *= max((a[2] + sa[2]) * inv_samples, 0.01f);
  }

  if (pass_sample_count == PASS_UNUSED || upscale_factor == 1.0f) {
    denoised_pixel[0] *= pixel_scale;
    denoised_pixel[1] *= pixel_scale;
    denoised_pixel[2] *= pixel_scale;
  }

  if (num_components == 3) {
    /* Pass without alpha channel. */
  }
  else if (!use_compositing) {
    /* Currently compositing passes are either 3-component (derived by dividing light passes)
     * or do not have transparency (shadow catcher). Implicitly rely on this logic, as it
     * simplifies logic and avoids extra memory allocation. */
    const ccl_global float *noisy_pixel = buffer + pass_noisy;
    denoised_pixel[3] = noisy_pixel[3];

    if (pass_sample_count != PASS_UNUSED && upscale_factor != 1.0f) {
      denoised_pixel[3] /= pixel_scale;
    }
  }
  else {
    /* Assigning to zero since this is a default alpha value for 3-component passes, and it
     * is an opaque pixel for 4 component passes. */
    denoised_pixel[3] = 0;
  }
}
ccl_gpu_kernel_postfix

#endif
