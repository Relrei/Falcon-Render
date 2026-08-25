/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "kernel/types.h"

#ifndef KERNEL_DATA_ARRAY
#  define KERNEL_DATA_ARRAY(type, name)
#endif
#ifndef KERNEL_DATA_ARRAY_WRITABLE
#  define KERNEL_DATA_ARRAY_WRITABLE(type, name) KERNEL_DATA_ARRAY(type, name)
#endif

/* BVH2, not used for OptiX or Embree. */
KERNEL_DATA_ARRAY(float4, bvh_nodes)
KERNEL_DATA_ARRAY(float4, bvh_leaf_nodes)
KERNEL_DATA_ARRAY(uint, prim_type)
KERNEL_DATA_ARRAY(uint, prim_visibility)
KERNEL_DATA_ARRAY(uint, prim_index)
KERNEL_DATA_ARRAY(uint, prim_object)
KERNEL_DATA_ARRAY(uint, object_node)
KERNEL_DATA_ARRAY(float2, prim_time)

/* objects */
KERNEL_DATA_ARRAY(KernelObject, objects)
KERNEL_DATA_ARRAY(Transform, object_motion_pass)
KERNEL_DATA_ARRAY(DecomposedTransform, object_motion)
KERNEL_DATA_ARRAY(uint, object_flag)
KERNEL_DATA_ARRAY(uint, object_prim_offset)

/* cameras */
KERNEL_DATA_ARRAY(DecomposedTransform, camera_motion)

/* triangles */
KERNEL_DATA_ARRAY(uint, tri_shader)
KERNEL_DATA_ARRAY(packed_uint3, tri_vindex)
KERNEL_DATA_ARRAY(packed_float3, tri_verts)

/* curves */
KERNEL_DATA_ARRAY(KernelCurve, curves)
KERNEL_DATA_ARRAY(float4, curve_keys)
KERNEL_DATA_ARRAY(KernelCurveSegment, curve_segments)

/* pointclouds */
KERNEL_DATA_ARRAY(float4, points)
KERNEL_DATA_ARRAY(uint, points_shader)

/* attributes */
KERNEL_DATA_ARRAY(AttributeMap, attributes_map)
KERNEL_DATA_ARRAY(float, attributes_float)
KERNEL_DATA_ARRAY(float2, attributes_float2)
KERNEL_DATA_ARRAY(packed_float3, attributes_float3)
KERNEL_DATA_ARRAY(float4, attributes_float4)
KERNEL_DATA_ARRAY(uchar4, attributes_uchar4)
KERNEL_DATA_ARRAY(packed_normal, attributes_normal)

/* lights */
KERNEL_DATA_ARRAY(KernelLightDistribution, light_distribution)
KERNEL_DATA_ARRAY(KernelLight, lights)
KERNEL_DATA_ARRAY(float2, light_background_marginal_cdf)
KERNEL_DATA_ARRAY(float2, light_background_conditional_cdf)

/* light tree */
KERNEL_DATA_ARRAY(KernelLightTreeNode, light_tree_nodes)
KERNEL_DATA_ARRAY(KernelLightTreeEmitter, light_tree_emitters)
KERNEL_DATA_ARRAY(uint, light_to_tree)
KERNEL_DATA_ARRAY(uint, object_lookup_offset)
KERNEL_DATA_ARRAY(uint, triangle_to_tree)

/* particles */
KERNEL_DATA_ARRAY(KernelParticle, particles)

/* shaders */
KERNEL_DATA_ARRAY(uint, svm_nodes)
KERNEL_DATA_ARRAY(KernelShader, shaders)

/* lookup tables */
KERNEL_DATA_ARRAY(float, lookup_table)

/* tabulated Sobol sample pattern */
KERNEL_DATA_ARRAY(float, sample_pattern_lut)

/* ies lights */
KERNEL_DATA_ARRAY(float, ies)

/* Volume. */
KERNEL_DATA_ARRAY(KernelOctreeNode, volume_tree_nodes)
KERNEL_DATA_ARRAY(KernelOctreeRoot, volume_tree_roots)
KERNEL_DATA_ARRAY(int, volume_tree_root_ids)
KERNEL_DATA_ARRAY(float, volume_step_size)

/* image textures */
KERNEL_DATA_ARRAY(KernelImageTexture, image_textures)
KERNEL_DATA_ARRAY_WRITABLE(KernelTileDescriptor, image_texture_tile_descriptors)
KERNEL_DATA_ARRAY_WRITABLE(uint8_t, image_texture_tile_access_state)
KERNEL_DATA_ARRAY(KernelImageUDIM, image_texture_udims)
KERNEL_DATA_ARRAY(KernelImageInfo, image_info)

/* Falcon SHARC spatial hash radiance cache. Read-only from the kernel: the host
 * fills it during the warmup pass and uploads it, then the camera-hit blend only
 * looks it up. Allocated only when WITH_FALCON_SHARC, otherwise a null pointer.
 * (Cycles 4.5 has no kernel-writable data array, so unlike the standalone 5.2
 * version this is a plain read-only array and there is no per-pixel cell buffer;
 * the host deposits via the Position pass instead.) */
KERNEL_DATA_ARRAY(float, falcon_sharc_cache)

/* Falcon Photon point map (Round 9): raw photon points (stride 9: pos3, flux3,
 * normal3), bake-time append counter, and the host-built uniform neighbor grid
 * (cell = lookup radius; per-slot start/count into the sorted photon index).
 * Hash-slot collisions are resolved by the lookup's distance test. */
KERNEL_DATA_ARRAY(float, falcon_photon_points)
KERNEL_DATA_ARRAY(uint, falcon_photon_pcount)
KERNEL_DATA_ARRAY(uint, falcon_photon_grid_start)
KERNEL_DATA_ARRAY(uint, falcon_photon_grid_count)
KERNEL_DATA_ARRAY(uint, falcon_photon_index)

/* Falcon DAS (denoiser-aware sampling): per-pixel adaptive-threshold scale map,
 * host-built from an OIDN probe residual (|denoised - noisy|). scale < 1 makes
 * the convergence check stricter (more samples where the denoiser struggles),
 * scale > 1 lets denoiser-easy pixels stop early. Read-only, indexed by
 * render_pixel_index; allocated only when FALCON_DAS_MAP is set. */
KERNEL_DATA_ARRAY(float, falcon_das_scale)

/* Falcon error field: one relative-error scalar per world cell (same hash as
 * the SHARC grid, its own cell size). Empty cells hold a negative value. */
KERNEL_DATA_ARRAY(float, falcon_error_field)

#undef KERNEL_DATA_ARRAY
#undef KERNEL_DATA_ARRAY_WRITABLE
