/* SPDX-FileCopyrightText: 2026 Falcon (CyclesF)
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Falcon Dispersion: LuxCore-style on-demand spectral dispersion
 * (FALCON_DISPERSION.md). The engine stays RGB; a path samples a wavelength
 * only at its first smooth refractive scatter, converts it to a white-balanced
 * linear-sRGB throughput weight once, and from then on refracts with the
 * Cauchy-offset IOR for that wavelength. No spectral throughput, no film
 * changes. Math verified standalone in
 * obs2/claude_memo/bench/falcon_spectral_20260703.py. */

#pragma once

CCL_NAMESPACE_BEGIN

/* Blender 5.2 removed BSDF_ROUGHNESS_SQ_THRESH from svm/types.h; keep the 5.1
 * value (squared roughness under which a microfacet counts as smooth) for the
 * dispersion gate in shade_surface.h. */
#ifndef BSDF_ROUGHNESS_SQ_THRESH
#  define BSDF_ROUGHNESS_SQ_THRESH 2e-10f
#endif

#ifdef __FALCON_SHARC__

/* Sampled band. Uniform lambda over [380, 730] nm. */
#define FALCON_DISPERSION_LAMBDA_MIN 380.0f
#define FALCON_DISPERSION_LAMBDA_SPAN 350.0f

ccl_device_forceinline float falcon_dispersion_cie_gauss(const float x,
                                                         const float mu,
                                                         const float s1,
                                                         const float s2)
{
  const float s = (x < mu) ? s1 : s2;
  const float t = (x - mu) / s;
  return expf(-0.5f * t * t);
}

/* Per-path spectral weight: CIE 1931 color matching (Wyman/Sloan/Shirley 2013
 * multi-lobe fit) -> XYZ (D65) -> linear sRGB, clamped to gamut, then
 * white-balanced per channel so that the MEAN over uniformly sampled lambda is
 * exactly (1,1,1): white light through a dispersive interface stays white.
 * Balance constants = 1 / channel mean over the band (precomputed, MC-checked
 * to 0.35% at 1M samples). */
ccl_device float3 falcon_dispersion_cie_weight(const float lambda_nm)
{
  const float x = 1.056f * falcon_dispersion_cie_gauss(lambda_nm, 599.8f, 37.9f, 31.0f) +
                  0.362f * falcon_dispersion_cie_gauss(lambda_nm, 442.0f, 16.0f, 26.7f) -
                  0.065f * falcon_dispersion_cie_gauss(lambda_nm, 501.1f, 20.4f, 26.2f);
  const float y = 0.821f * falcon_dispersion_cie_gauss(lambda_nm, 568.8f, 46.9f, 40.5f) +
                  0.286f * falcon_dispersion_cie_gauss(lambda_nm, 530.9f, 16.3f, 31.1f);
  const float z = 1.217f * falcon_dispersion_cie_gauss(lambda_nm, 437.0f, 11.8f, 36.0f) +
                  0.681f * falcon_dispersion_cie_gauss(lambda_nm, 459.0f, 26.0f, 13.8f);

  float3 rgb = make_float3(3.2404542f * x - 1.5371385f * y - 0.4985314f * z,
                           -0.9692660f * x + 1.8760108f * y + 0.0415560f * z,
                           0.0556434f * x - 0.2040259f * y + 1.0572252f * z);
  rgb = max(rgb, zero_float3());
  return rgb * make_float3(1.9922777f, 3.0420891f, 3.2107011f);
}

/* Cauchy IOR offset for dispersion strength B (um^2), normalized so that
 * n(587.6nm) == the material's IOR socket value: changing B spreads the
 * spectrum without shifting the overall look. */
ccl_device_forceinline float falcon_dispersion_ior_offset(const float b_um2,
                                                          const float lambda_nm)
{
  const float l_um = lambda_nm * 1e-3f;
  const float ref_d = 1.0f / (0.5876f * 0.5876f);
  return b_um2 * (1.0f / (l_um * l_um) - ref_d);
}

#endif /* __FALCON_SHARC__ */

CCL_NAMESPACE_END
