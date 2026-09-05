/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "device/device.h"

#ifdef WITH_FALCON_SHARC
#  include <cmath>
#  include <cstdio>
#  include <cstdlib>
#  include <cstring>
#  include <vector>
#endif

#include "scene/background.h"
#include "scene/bake.h"
#include "scene/camera.h"
#include "scene/film.h"
#include "kernel/integrator/falcon_sharc_size.h"
#include "scene/integrator.h"
#include "scene/light.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/stats.h"
#include "scene/tabulated_sobol.h"
#include "scene/volume.h"

#include "kernel/types.h"

#include "util/hash.h"
#include "util/log.h"
#include "util/path.h"
#include "util/task.h"
#include "util/time.h"

CCL_NAMESPACE_BEGIN

/* Halton sequence generator using only integer numbers.
 * See https://doi.org/10.1016/0010-4655(91)90064-R for details. */
static float halton(int &a, int &b, int base)
{
  int x = b - a;
  if (x == 1) {
    a = 1;
    b *= base;
  }
  else {
    int y = b / base;
    while (x <= y) {
      y /= base;
    }
    a = (1 + base) * y - x;
  }
  return static_cast<float>(a) / static_cast<float>(b);
}
float2 HaltonSequence::next()
{
  return make_float2(halton(a2, b2, 2) - 0.5f, halton(a3, b3, 3) - 0.5f);
}

#ifdef WITH_FALCON_SHARC
/* Falcon knobs live on Integrator sockets (fed from the scene properties in
 * sync.cpp), but the environment variable of the same name still wins when it
 * is set: the measurement harnesses drive whole sweeps that way, and a stray
 * export should keep behaving as it always has. A GUI session exports nothing,
 * so there the socket -- i.e. the value saved in the .blend, per scene, and
 * different between the viewport and the final render -- is what renders. */
static float falcon_knob_float(const char *env_name, const float socket_value)
{
  const char *value = getenv(env_name);
  return value ? (float)atof(value) : socket_value;
}

/* Presence means on, as it always did, except for an explicit "0". */
static bool falcon_knob_flag(const char *env_name, const bool socket_value)
{
  const char *value = getenv(env_name);
  return value ? strcmp(value, "0") != 0 : socket_value;
}

static string falcon_knob_string(const char *env_name, const ustring &socket_value)
{
  const char *value = getenv(env_name);
  return value ? string(value) : string(socket_value.c_str());
}

static int falcon_knob_sharc_mode(const int socket_value)
{
  const char *value = getenv("FALCON_SHARC_MODE");
  if (!value) {
    return socket_value;
  }
  if (strcmp(value, "warmup") == 0) {
    return FALCON_SHARC_MODE_WARMUP;
  }
  if (strcmp(value, "blend") == 0) {
    return FALCON_SHARC_MODE_BLEND;
  }
  if (strcmp(value, "live") == 0) {
    return FALCON_SHARC_MODE_LIVE;
  }
  return FALCON_SHARC_MODE_OFF;
}

/* ---------------------------------------------------------------------------
 * Falcon の値は .blend から来る「外部入力」である (2026-08-21)
 *
 * scene.cycles の Falcon プロパティは PropertyGroup の IDProperty なので、
 * .blend を読み込むときに RNA の min/max が当て直されない。実測: RNA 上限 8.0 の
 * falcon_photon_radius に 1e9 を書いた .blend が 1000000000.0 のまま読み戻る。
 * つまりここへ届く値は全部が無検証の外部入力で、ファイルを開いただけで踏める。
 * 以下はその検証だけを足したもので、アルゴリズムには触っていない。
 * ------------------------------------------------------------------------- */

/* falcon_photon_deposit_wide() の三重ループの上限。RNA スライダと同じ 8。 */
static const float FALCON_PHOTON_RADIUS_MAX = 8.0f;

/* キャッシュファイルの置き場。ベイク操作 (operators.py の
 * _falcon_photon_cache_paths) が使うのと同じディレクトリで、ここが唯一の
 * 書き込み先になる。両方を直すときは片方だけ変えないこと。 */
static string falcon_cache_dir()
{
  const char *xdg = getenv("XDG_CACHE_HOME");
  string base;
  if (xdg && xdg[0] != '\0') {
    base = string(xdg);
  }
  else {
    const char *home = getenv("HOME");
    base = path_join(string(home && home[0] != '\0' ? home : "/tmp"), ".cache");
  }
  return path_join(base, "falcon_photon");
}

/* SHARC / フォトンキャッシュの書き込み先を、上のディレクトリの中へ閉じ込める。
 *
 * ここは検証がまったく無かった。LIVE モードは PathTrace::render_pipeline を
 * 通る = Rendered シェーディングのビューポートで走るので、Rendered 状態で保存
 * された .blend を開いた瞬間に path_trace.cpp が fopen(path,"wb") + 1GiB の
 * fwrite を実行する。実測で、出荷バイナリが .blend に書かれた任意のパスを
 * 1,073,741,824 バイトで上書きした (.spread / .meta も同時に作られる)。
 *
 * ベイク操作が作るパス (falcon_photon_*.bin / .L0 / _lt_scratch.bin) はどれも
 * このディレクトリの中なので素通りする。外を指していたらベース名だけ取って
 * 中へ移す。計測ハーネスのように外へ書きたいときだけ
 * FALCON_SHARC_ALLOW_ABSOLUTE_CACHE=1 で従来どおりになる (既定は安全側)。 */
static string falcon_confine_cache_path(const string &requested)
{
  if (requested.empty()) {
    return requested;
  }
  if (getenv("FALCON_SHARC_ALLOW_ABSOLUTE_CACHE")) {
    return requested;
  }

  const string dir = falcon_cache_dir();
  const string prefix = dir + "/";
  const string absolute = path_is_relative(requested) ? path_join(dir, requested) : requested;
  const string normalized = path_normalize(absolute);

  /* すでに中にいるなら何もしない。".." が残っていたら正規化で消えているはずだが、
   * 残っていた場合は下のベース名扱いへ落とす。 */
  if (normalized.size() > prefix.size() && normalized.compare(0, prefix.size(), prefix) == 0 &&
      normalized.find("..") == string::npos)
  {
    return normalized;
  }

  string name = path_filename(requested);
  for (size_t i = 0; i < name.size(); i++) {
    if (name[i] == '/' || name[i] == '\\') {
      name[i] = '_';
    }
  }
  if (name.empty() || name == "." || name == "..") {
    name = "falcon_sharc_cache.bin";
  }
  const string confined = path_join(dir, name);
  /* ベイクを通さずに LIVE を使った場合、置き場がまだ無いことがある。 */
  path_create_directories(confined);
  LOG_WARNING << "Falcon: キャッシュの書き込み先 " << requested << " は "
              << dir << " の外なので " << confined << " に読み替えました "
              << "(FALCON_SHARC_ALLOW_ABSOLUTE_CACHE=1 で従来どおり)";
  return confined;
}
#endif

NODE_DEFINE(Integrator)
{
  NodeType *type = NodeType::add("integrator", create);

  SOCKET_INT(min_bounce, "Min Bounce", 0);
  SOCKET_INT(max_bounce, "Max Bounce", 7);

  SOCKET_INT(max_diffuse_bounce, "Max Diffuse Bounce", 7);
  SOCKET_INT(max_glossy_bounce, "Max Glossy Bounce", 7);
  SOCKET_INT(max_transmission_bounce, "Max Transmission Bounce", 7);
  SOCKET_INT(max_volume_bounce, "Max Volume Bounce", 7);

  SOCKET_INT(transparent_min_bounce, "Transparent Min Bounce", 0);
  SOCKET_INT(transparent_max_bounce, "Transparent Max Bounce", 7);

#ifdef WITH_CYCLES_DEBUG
  static NodeEnum direct_light_sampling_type_enum;
  direct_light_sampling_type_enum.insert("multiple_importance_sampling",
                                         DIRECT_LIGHT_SAMPLING_MIS);
  direct_light_sampling_type_enum.insert("forward_path_tracing", DIRECT_LIGHT_SAMPLING_FORWARD);
  direct_light_sampling_type_enum.insert("next_event_estimation", DIRECT_LIGHT_SAMPLING_NEE);
  SOCKET_ENUM(direct_light_sampling_type,
              "Direct Light Sampling Type",
              direct_light_sampling_type_enum,
              DIRECT_LIGHT_SAMPLING_MIS);
#endif

  SOCKET_INT(ao_bounces, "AO Bounces", 0);
  SOCKET_FLOAT(ao_factor, "AO Factor", 0.0f);
  SOCKET_FLOAT(ao_distance, "AO Distance", FLT_MAX);
  SOCKET_FLOAT(ao_additive_factor, "AO Additive Factor", 0.0f);

  SOCKET_BOOLEAN(volume_ray_marching, "Biased", false);
  SOCKET_INT(volume_max_steps, "Volume Max Steps", 1024);
  SOCKET_FLOAT(volume_step_rate, "Volume Step Rate", 1.0f);

  static NodeEnum guiding_distribution_enum;
  guiding_distribution_enum.insert("PARALLAX_AWARE_VMM", GUIDING_TYPE_PARALLAX_AWARE_VMM);
  guiding_distribution_enum.insert("DIRECTIONAL_QUAD_TREE", GUIDING_TYPE_DIRECTIONAL_QUAD_TREE);
  guiding_distribution_enum.insert("VMM", GUIDING_TYPE_VMM);

  static NodeEnum guiding_directional_sampling_type_enum;
  guiding_directional_sampling_type_enum.insert("MIS",
                                                GUIDING_DIRECTIONAL_SAMPLING_TYPE_PRODUCT_MIS);
  guiding_directional_sampling_type_enum.insert("RIS", GUIDING_DIRECTIONAL_SAMPLING_TYPE_RIS);
  guiding_directional_sampling_type_enum.insert("ROUGHNESS",
                                                GUIDING_DIRECTIONAL_SAMPLING_TYPE_ROUGHNESS);

  SOCKET_BOOLEAN(use_guiding, "Guiding", false);
  SOCKET_BOOLEAN(deterministic_guiding, "Deterministic Guiding", true);
  SOCKET_BOOLEAN(use_surface_guiding, "Surface Guiding", true);
  SOCKET_FLOAT(surface_guiding_probability, "Surface Guiding Probability", 0.5f);
  SOCKET_BOOLEAN(use_volume_guiding, "Volume Guiding", true);
  SOCKET_FLOAT(volume_guiding_probability, "Volume Guiding Probability", 0.5f);
  SOCKET_INT(guiding_training_samples, "Training Samples", 128);
  SOCKET_BOOLEAN(use_guiding_direct_light, "Guide Direct Light", true);
  SOCKET_BOOLEAN(use_guiding_mis_weights, "Use MIS Weights", true);
  SOCKET_ENUM(guiding_distribution_type,
              "Guiding Distribution Type",
              guiding_distribution_enum,
              GUIDING_TYPE_PARALLAX_AWARE_VMM);
  SOCKET_ENUM(guiding_directional_sampling_type,
              "Guiding Directional Sampling Type",
              guiding_directional_sampling_type_enum,
              GUIDING_DIRECTIONAL_SAMPLING_TYPE_RIS);
  SOCKET_FLOAT(guiding_roughness_threshold, "Guiding Roughness Threshold", 0.05f);

  SOCKET_BOOLEAN(caustics_reflective, "Reflective Caustics", true);
  SOCKET_BOOLEAN(caustics_refractive, "Refractive Caustics", true);
  SOCKET_FLOAT(filter_glossy, "Filter Glossy", 0.0f);

  SOCKET_BOOLEAN(use_direct_light, "Use Direct Light", true);
  SOCKET_BOOLEAN(use_indirect_light, "Use Indirect Light", true);
  SOCKET_BOOLEAN(use_diffuse, "Use Diffuse", true);
  SOCKET_BOOLEAN(use_glossy, "Use Glossy", true);
  SOCKET_BOOLEAN(use_transmission, "Use Transmission", true);
  SOCKET_BOOLEAN(use_emission, "Use Emission", true);

  SOCKET_INT(seed, "Seed", 0);

  SOCKET_FLOAT(sample_clamp_direct, "Sample Clamp Direct", 0.0f);
  SOCKET_FLOAT(sample_clamp_indirect, "Sample Clamp Indirect", 10.0f);
  SOCKET_BOOLEAN(motion_blur, "Motion Blur", false);

  SOCKET_INT(aa_samples, "AA Samples", 0);
  SOCKET_BOOLEAN(use_sample_subset, "Use Sample Subset", false);
  SOCKET_INT(sample_subset_offset, "Sample Subset Offset", 0);
  SOCKET_INT(sample_subset_length, "Sample Subset Length", MAX_SAMPLES);

  SOCKET_BOOLEAN(use_adaptive_sampling, "Use Adaptive Sampling", true);
  SOCKET_FLOAT(adaptive_threshold, "Adaptive Threshold", 0.01f);
  SOCKET_INT(adaptive_min_samples, "Adaptive Min Samples", 0);

  SOCKET_BOOLEAN(use_light_tree, "Use light tree to optimize many light sampling", true);
  SOCKET_FLOAT(light_sampling_threshold, "Light Sampling Threshold", 0.0f);

  static NodeEnum sampling_pattern_enum;
  sampling_pattern_enum.insert("sobol_burley", SAMPLING_PATTERN_SOBOL_BURLEY);
  sampling_pattern_enum.insert("tabulated_sobol", SAMPLING_PATTERN_TABULATED_SOBOL);
  sampling_pattern_enum.insert("blue_noise_pure", SAMPLING_PATTERN_BLUE_NOISE_PURE);
  sampling_pattern_enum.insert("blue_noise_round", SAMPLING_PATTERN_BLUE_NOISE_ROUND);
  sampling_pattern_enum.insert("blue_noise_first", SAMPLING_PATTERN_BLUE_NOISE_FIRST);
  SOCKET_ENUM(sampling_pattern,
              "Sampling Pattern",
              sampling_pattern_enum,
              SAMPLING_PATTERN_TABULATED_SOBOL);
  SOCKET_FLOAT(scrambling_distance, "Scrambling Distance", 1.0f);

  SOCKET_BOOLEAN(use_pixel_jitter, "Use Pixel Jitter", false);
  SOCKET_BOOLEAN(use_custom_pixel_jitter_sample, "Use custom pixel jitter sample value", false);
  SOCKET_FLOAT_ARRAY(
      custom_pixel_jitter_sample, "Custom pixel jitter sample overwrite value", array<float>());

  static NodeEnum denoiser_type_enum;
  denoiser_type_enum.insert("none", DENOISER_NONE);
  denoiser_type_enum.insert("optix", DENOISER_OPTIX);
  denoiser_type_enum.insert("openimagedenoise", DENOISER_OPENIMAGEDENOISE);
  denoiser_type_enum.insert("dlss", DENOISER_DLSS);

  static NodeEnum denoiser_prefilter_enum;
  denoiser_prefilter_enum.insert("none", DENOISER_PREFILTER_NONE);
  denoiser_prefilter_enum.insert("fast", DENOISER_PREFILTER_FAST);
  denoiser_prefilter_enum.insert("accurate", DENOISER_PREFILTER_ACCURATE);

  static NodeEnum denoiser_quality_enum;
  denoiser_quality_enum.insert("high", DENOISER_QUALITY_HIGH);
  denoiser_quality_enum.insert("balanced", DENOISER_QUALITY_BALANCED);
  denoiser_quality_enum.insert("fast", DENOISER_QUALITY_FAST);

  /* Default to accurate denoising with OpenImageDenoise. For interactive viewport
   * it's best use OptiX and disable the normal pass since it does not always have
   * the desired effect for that denoiser. */
  SOCKET_BOOLEAN(use_denoise, "Use Denoiser", false);
  SOCKET_ENUM(denoiser_type, "Denoiser Type", denoiser_type_enum, DENOISER_OPENIMAGEDENOISE);
  SOCKET_INT(denoise_start_sample, "Start Sample to Denoise", 0);
  SOCKET_INT(denoiser_passes, "Denoiser Passes", DENOISER_PASS_ALBEDO | DENOISER_PASS_NORMAL);
  SOCKET_ENUM(denoiser_prefilter,
              "Denoiser Prefilter",
              denoiser_prefilter_enum,
              DENOISER_PREFILTER_ACCURATE);
  SOCKET_BOOLEAN(denoise_use_gpu, "Denoise on GPU", true);
  SOCKET_ENUM(denoiser_quality, "Denoiser Quality", denoiser_quality_enum, DENOISER_QUALITY_HIGH);
  SOCKET_FLOAT(denoiser_upscale_factor, "Denoiser Upscale Factor", 1.0f);
  SOCKET_BOOLEAN(denoiser_carry_history, "Denoiser Carry History", false);
  SOCKET_INT(denoiser_preroll_passes, "Denoiser Preroll Passes", 4);
  SOCKET_INT(denoiser_preroll_passes_cut, "Denoiser Preroll Passes Cut", 0);
  SOCKET_BOOLEAN(denoiser_cut_warmup, "Denoiser Cut Warmup", true);

#ifdef WITH_FALCON_SHARC
  /* Falcon knobs, see integrator.h. Defaults repeat the values device_update()
   * used when the matching environment variable was absent, so a scene that
   * never touches them renders exactly as before. */
  SOCKET_INT(falcon_sharc_mode, "Falcon SHARC Mode", FALCON_SHARC_MODE_OFF);
  SOCKET_FLOAT(falcon_sharc_cell, "Falcon SHARC Cell Size", 0.2f);
  SOCKET_FLOAT(falcon_sharc_alpha, "Falcon SHARC Blend", 0.7f);
  SOCKET_FLOAT(falcon_sharc_keep, "Falcon SHARC Live Keep", 0.9f);
  SOCKET_STRING(falcon_sharc_cache, "Falcon SHARC Cache File", ustring());
  SOCKET_BOOLEAN(falcon_sharc_gate, "Falcon SHARC Auto GI Gate", true);
  SOCKET_FLOAT(falcon_sharc_gate_low, "Falcon SHARC Gate Low", 0.15f);
  SOCKET_FLOAT(falcon_sharc_gate_high, "Falcon SHARC Gate High", 0.40f);
  SOCKET_FLOAT(falcon_dispersion_b, "Falcon Dispersion B", 0.0f);
  SOCKET_FLOAT(falcon_photon_radius, "Falcon Photon Deposit Radius", 3.0f);
  SOCKET_FLOAT(falcon_photon_point_radius_m, "Falcon Photon Point Radius", 0.03f);
  SOCKET_FLOAT(falcon_photon_point_normal_deg, "Falcon Photon Point Normal Cone", 30.0f);
  SOCKET_FLOAT(falcon_photon_point_gain, "Falcon Photon Point Gain", 1.0f);
  SOCKET_FLOAT(falcon_lt_gain, "Falcon LT Gain", 1.0f);
  SOCKET_FLOAT(falcon_lt_splat_radius, "Falcon LT Splat Radius", 0.0f);
  SOCKET_BOOLEAN(falcon_lt_visibility, "Falcon LT Visibility", false);
  SOCKET_BOOLEAN(falcon_lt_direct, "Falcon LT Direct Floor", false);
  SOCKET_STRING(falcon_das_map, "Falcon DAS Map", ustring());
  SOCKET_FLOAT(falcon_das_strength, "Falcon DAS Strength", 1.0f);
  SOCKET_STRING(falcon_error_map, "Falcon Error Field File", ustring());
  SOCKET_FLOAT(falcon_error_cell, "Falcon Error Field Cell Size", 0.8f);
  SOCKET_FLOAT(falcon_error_threshold, "Falcon Error Termination Threshold", 0.0f);
  SOCKET_BOOLEAN(falcon_error_raise_alpha, "Falcon Error Field May Take The Cache", false);
#endif

  return type;
}

Integrator::Integrator() : Node(get_node_type()) {}

Integrator::~Integrator() = default;

void Integrator::device_update(Device *device, DeviceScene *dscene, Scene *scene)
{
  if (!is_modified()) {
    return;
  }

  const scoped_callback_timer timer([scene](double time) {
    if (scene->update_stats) {
      scene->update_stats->integrator.times.add_entry({"device_update", time});
    }
  });

  if (use_denoise && denoiser_type == DENOISER_DLSS) {
    use_pixel_jitter = true;
  }

  KernelIntegrator *kintegrator = &dscene->data.integrator;

  device_free(device, dscene);

  /* integrator parameters */

  /* Plus one so that a bounce of 0 indicates no global illumination, only direct illumination. */
  kintegrator->min_bounce = min_bounce + 1;
  kintegrator->max_bounce = max_bounce + 1;

  kintegrator->max_diffuse_bounce = max_diffuse_bounce + 1;
  kintegrator->max_glossy_bounce = max_glossy_bounce + 1;
  kintegrator->max_transmission_bounce = max_transmission_bounce + 1;
  kintegrator->max_volume_bounce = max_volume_bounce + 1;

  kintegrator->transparent_min_bounce = transparent_min_bounce + 1;

  /* Unlike other type of bounces, 0 transparent bounce means there is no transparent bounce in the
   * scene. */
  kintegrator->transparent_max_bounce = transparent_max_bounce;

  kintegrator->ao_bounces = (ao_factor != 0.0f) ? ao_bounces : 0;
  kintegrator->ao_bounces_distance = ao_distance;
  kintegrator->ao_bounces_factor = ao_factor;
  kintegrator->ao_additive_factor = ao_additive_factor;

#ifdef WITH_CYCLES_DEBUG
  kintegrator->direct_light_sampling_type = direct_light_sampling_type;
#else
  kintegrator->direct_light_sampling_type = DIRECT_LIGHT_SAMPLING_MIS;
#endif

  /* Transparent Shadows
   * We only need to enable transparent shadows, if we actually have
   * transparent shaders in the scene. Otherwise we can disable it
   * to improve performance a bit. */
  kintegrator->transparent_shadows = false;
  for (Shader *shader : scene->shaders) {
    if (shader->reference_count() == 0) {
      continue;
    }
    /* keep this in sync with SD_HAS_TRANSPARENT_SHADOW in shader.cpp */
    if ((shader->has_surface_transparent && shader->get_use_transparent_shadow()) ||
        shader->has_volume)
    {
      kintegrator->transparent_shadows = true;
      break;
    }
  }

  kintegrator->volume_ray_marching = volume_ray_marching;
  kintegrator->volume_max_steps = volume_max_steps;

  kintegrator->caustics_reflective = caustics_reflective;
  kintegrator->caustics_refractive = caustics_refractive;
  kintegrator->filter_glossy = (filter_glossy == 0.0f) ? FLT_MAX : 1.0f / filter_glossy;
  kintegrator->differential_widen_scale = min(1.0f, filter_glossy);

  kintegrator->filter_closures = 0;
  if (!use_direct_light) {
    kintegrator->filter_closures |= FILTER_CLOSURE_DIRECT_LIGHT;
  }
  if (!use_indirect_light) {
    kintegrator->min_bounce = 1;
    kintegrator->max_bounce = 1;
  }
  if (!use_diffuse) {
    kintegrator->filter_closures |= FILTER_CLOSURE_DIFFUSE;
  }
  if (!use_glossy) {
    kintegrator->filter_closures |= FILTER_CLOSURE_GLOSSY;
  }
  if (!use_transmission) {
    kintegrator->filter_closures |= FILTER_CLOSURE_TRANSMISSION;
  }
  if (!use_emission) {
    kintegrator->filter_closures |= FILTER_CLOSURE_EMISSION;
  }
  if (scene->bake_manager->get_baking()) {
    /* Baking does not need to trace through transparency, we only want to bake
     * the object itself. */
    kintegrator->filter_closures |= FILTER_CLOSURE_TRANSPARENT;
  }

  const GuidingParams guiding_params = get_guiding_params(device);
  kintegrator->use_guiding = guiding_params.use;
  kintegrator->train_guiding = kintegrator->use_guiding;
  kintegrator->use_surface_guiding = guiding_params.use_surface_guiding;
  kintegrator->use_volume_guiding = guiding_params.use_volume_guiding;
  kintegrator->surface_guiding_probability = surface_guiding_probability;
  kintegrator->volume_guiding_probability = volume_guiding_probability;
  kintegrator->use_guiding_direct_light = use_guiding_direct_light;
  kintegrator->use_guiding_mis_weights = use_guiding_mis_weights;
  kintegrator->guiding_distribution_type = guiding_params.type;
  kintegrator->guiding_directional_sampling_type = guiding_params.sampling_type;
  kintegrator->guiding_roughness_threshold = guiding_params.roughness_threshold;

  kintegrator->sample_clamp_direct = (sample_clamp_direct == 0.0f) ? FLT_MAX :
                                                                     sample_clamp_direct * 3.0f;
  kintegrator->sample_clamp_indirect = (sample_clamp_indirect == 0.0f) ?
                                           FLT_MAX :
                                           sample_clamp_indirect * 3.0f;

  const int clamped_aa_samples = min(aa_samples, MAX_SAMPLES);

  kintegrator->sampling_pattern = sampling_pattern;
  kintegrator->scrambling_distance = scrambling_distance;
  kintegrator->sobol_index_mask = reverse_integer_bits(next_power_of_two(clamped_aa_samples - 1) -
                                                       1);
  kintegrator->blue_noise_sequence_length = clamped_aa_samples;
  if (kintegrator->sampling_pattern == SAMPLING_PATTERN_BLUE_NOISE_ROUND) {
    if (!is_power_of_two(clamped_aa_samples)) {
      kintegrator->blue_noise_sequence_length = next_power_of_two(clamped_aa_samples);
    }
    kintegrator->sampling_pattern = SAMPLING_PATTERN_BLUE_NOISE_PURE;
  }
  if (kintegrator->sampling_pattern == SAMPLING_PATTERN_BLUE_NOISE_FIRST) {
    kintegrator->blue_noise_sequence_length -= 1;
  }

  /* Randomize the seed every frame when applying pixel jitter. */
  if (use_pixel_jitter) {
    if (use_custom_pixel_jitter_sample) {
      kintegrator->seed = hash_uint2(seed, pixel_jitter_frame);
    }
    else {
      kintegrator->seed = hash_uint3(seed, pixel_jitter_state.a2, pixel_jitter_state.a3);
    }
  }
  /* The blue-noise sampler needs a randomized seed to scramble properly, providing e.g. 0 won't
   * work properly. Therefore, hash the seed in those cases. */
  else if (kintegrator->sampling_pattern == SAMPLING_PATTERN_BLUE_NOISE_FIRST ||
           kintegrator->sampling_pattern == SAMPLING_PATTERN_BLUE_NOISE_PURE)
  {
    kintegrator->seed = hash_uint(seed);
  }
  else {
    kintegrator->seed = seed;
  }

  /* NOTE: The kintegrator->use_light_tree is assigned to the efficient value in the light manager,
   * and the synchronization code is expected to tag the light manager for update when the
   * `use_light_tree` is changed. */
  if (light_sampling_threshold > 0.0f && !kintegrator->use_light_tree) {
    kintegrator->light_inv_rr_threshold = scene->film->get_exposure() / light_sampling_threshold;
  }
  else {
    kintegrator->light_inv_rr_threshold = 0.0f;
  }

  /* Build pre-tabulated Sobol samples if needed. */
  const int sequence_size = clamp(
      next_power_of_two(clamped_aa_samples - 1), MIN_TAB_SOBOL_SAMPLES, MAX_TAB_SOBOL_SAMPLES);
  const int table_size = sequence_size * NUM_TAB_SOBOL_PATTERNS * NUM_TAB_SOBOL_DIMENSIONS;
  if (kintegrator->sampling_pattern == SAMPLING_PATTERN_TABULATED_SOBOL &&
      dscene->sample_pattern_lut.size() != table_size)
  {
    kintegrator->tabulated_sobol_sequence_size = sequence_size;

    if (dscene->sample_pattern_lut.size() != 0) {
      dscene->sample_pattern_lut.free();
    }
    float4 *directions = (float4 *)dscene->sample_pattern_lut.alloc(table_size);
    TaskPool pool;
    for (int j = 0; j < NUM_TAB_SOBOL_PATTERNS; ++j) {
      float4 *sequence = directions + j * sequence_size;
      pool.push([sequence, sequence_size, j] {
        tabulated_sobol_generate_4D(sequence, sequence_size, j);
      });
    }
    pool.wait_work();

    dscene->sample_pattern_lut.copy_to_device();
  }

  kintegrator->has_shadow_catcher = scene->has_shadow_catcher();

  if (use_pixel_jitter) {
    if (use_custom_pixel_jitter_sample) {
      kintegrator->pixel_jitter = make_float2(custom_pixel_jitter_sample[0],
                                              custom_pixel_jitter_sample[1]);
      ++pixel_jitter_frame;
    }
    else {
      if (!pixel_jitter_pinned_ || pixel_jitter_pending_) {
        pixel_jitter_current_ = pixel_jitter_state.next();
        pixel_jitter_pending_ = false;
      }
      kintegrator->pixel_jitter = pixel_jitter_current_;
    }
  }
  else {
    kintegrator->pixel_jitter = make_float2(FLT_MAX);
    pixel_jitter_state.reset();
  }

  dscene->sample_pattern_lut.clear_modified();

#ifdef WITH_FALCON_SHARC
  /* Falcon SHARC is only active when FALCON_SHARC_MODE selects warmup or blend.
   * Everything below is gated on this so a normal render pays nothing: no 64 MB
   * cache allocation and (via falcon_sharc_active) no in-kernel cache lookup. */
  {
    const int sharc_mode = falcon_knob_sharc_mode(falcon_sharc_mode);
    const bool sharc_active = sharc_mode != FALCON_SHARC_MODE_OFF;
    kintegrator->falcon_sharc_active = sharc_active ? 1 : 0;

    /* Hand the render loop the knobs it needs but the kernel does not, resolved
     * here once so path_trace.cpp never has to read the environment itself. */
    dscene->falcon_sharc_mode = sharc_mode;
    const string cache_setting = falcon_confine_cache_path(
        falcon_knob_string("FALCON_SHARC_CACHE", falcon_sharc_cache));
    dscene->falcon_sharc_cache_path = cache_setting.empty() ?
                                          string("/tmp/falcon_sharc_cache.bin") :
                                          cache_setting;
    const float keep = falcon_knob_float("FALCON_SHARC_KEEP", falcon_sharc_keep);
    dscene->falcon_sharc_keep = keep < 0.0f ? 0.0f : (keep > 1.0f ? 1.0f : keep);

    /* Grid resolution shared by SHARC and the photon cache. Runtime knob so
     * caustic-scale grids (0.05-0.1) need no rebuild; must match the cell
     * size the host tracer used when depositing. */
    float cell = falcon_knob_float("FALCON_SHARC_CELL", falcon_sharc_cell);
    cell = cell > 1e-4f ? cell : 0.2f;
    kintegrator->falcon_sharc_cell_size = cell;

    /* Falcon Dispersion v0: global on-demand spectral knob. Cauchy B in um^2
     * (BK7 0.0042 / flint 0.013 / movie 0.030); 0 = off, zero overhead. */
    float dispersion_b = falcon_knob_float("FALCON_DISPERSION_B", falcon_dispersion_b);
    dispersion_b = dispersion_b > 0.0f ? dispersion_b : 0.0f;
    kintegrator->falcon_dispersion_b = dispersion_b;

    /* Falcon Photon map deposit radius (cells). FALCON_PHOTON_RADIUS, default 3
     * = kernel density estimation footprint; <=1 falls back to the 2x2x2 splat.
     *
     * The upper clamp is not cosmetic. falcon_photon_deposit_wide() walks
     * (2*ceil(r)+1)^3 cells per photon, so the cost is cubic in this number and
     * a big enough value hangs the GPU until the driver watchdog fires. The RNA
     * slider stops at 8, but a PropertyGroup IDProperty is NOT re-clamped when a
     * .blend is loaded (measured: 1e9 written into the file reads back as 1e9),
     * so the value arriving here is untrusted input. Clamp to the same 8 the UI
     * shows -- inside the slider's range nothing changes. */
    const float pr = falcon_knob_float("FALCON_PHOTON_RADIUS", falcon_photon_radius);
    if (pr > FALCON_PHOTON_RADIUS_MAX) {
      LOG_WARNING << "Falcon: コースティクスの広がり " << pr << " は上限 "
                  << FALCON_PHOTON_RADIUS_MAX << " を超えているので切り詰めました "
                  << "(この値のまま焼くと GPU が返ってきません)";
    }
    kintegrator->falcon_photon_radius = pr > 0.0f ?
                                            (pr > FALCON_PHOTON_RADIUS_MAX ?
                                                 FALCON_PHOTON_RADIUS_MAX :
                                                 pr) :
                                            3.0f;

    /* Falcon Photon Cache: FALCON_PHOTON_MODE=add + FALCON_SHARC_CACHE file
     * (written by tools/falcon_photon_trace.py). Loads into the same buffer as
     * SHARC -- the two modes share storage and are not meant to run together. */
    kintegrator->falcon_photon_add = 0;
    /* Restrict cache lookups to surfaces the bake could have deposited on.
     * FALCON_PHOTON_NO_LOOKUP_GATE=1 restores the old look-everywhere
     * behaviour for A/B. */
    kintegrator->falcon_photon_lookup_gate = getenv("FALCON_PHOTON_NO_LOOKUP_GATE") ? 0 : 1;
    /* How many diffuse vertices may add the layer (FALCON_PHOTON_LOOKUP_BOUNCES,
     * 0 = every one of them, which is the behaviour up to 2026-08-15). */
    kintegrator->falcon_photon_lookup_bounces = (int)falcon_knob_float(
        "FALCON_PHOTON_LOOKUP_BOUNCES", 0.0f);
    /* Deposit radius (cells) for photons whose last segment is under 2 cells,
     * i.e. the caster is touching the receiver. 0 = off. */
    kintegrator->falcon_photon_contact_radius = falcon_knob_float(
        "FALCON_PHOTON_CONTACT_RADIUS", 0.0f);
    kintegrator->falcon_photon_contact_cells = falcon_knob_float(
        "FALCON_PHOTON_CONTACT_CELLS", 2.0f);
    /* Grid lookup: read the single cell the shading point falls in, instead
     * of averaging the deposit stencil a second time. FALCON_PHOTON_INTERP=1
     * restores the old interpolating read for A/B. */
    /* 0 = the hole-filling gather (halves the light: it averages across the
     * normal layers too), 1 = read the cell the shading point lands in,
     * 2 = same but averaged over the tangential neighbours first (a blur that
     * keeps the total; FALCON_PHOTON_SOFT). */
    kintegrator->falcon_photon_nearest = getenv("FALCON_PHOTON_INTERP") ?
                                             0 :
                                             (getenv("FALCON_PHOTON_HARD") ? 1 : 2);
    kintegrator->falcon_photon_pass = 0;
    kintegrator->falcon_photon_is_world = 0;
    kintegrator->falcon_photon_point_store = 0;
    kintegrator->falcon_photon_point_mode = 0;
    kintegrator->falcon_photon_point_max = 0;
    kintegrator->falcon_photon_point_gain = 1.0f;
    kintegrator->falcon_lighttrace = 0;
    kintegrator->falcon_lighttrace_gain = 1.0f;
    kintegrator->falcon_lt_direct = 0;
    kintegrator->falcon_lt_samples = 1;
    kintegrator->falcon_lt_splat_radius = 0.0f;
    kintegrator->falcon_lt_visibility = 0;
    const char *photon_mode = getenv("FALCON_PHOTON_MODE");

    /* Falcon Light Tracing (FQ): FALCON_PHOTON_MODE=bake + FALCON_LIGHTTRACE=1.
     * The bake pass, instead of depositing, projects the first diffuse caustic
     * hit to the camera and splats (see falcon_lighttrace.h). The bake operator
     * renders at CAMERA resolution and keeps the film. GAIN is a labeled knob
     * until the camera importance We is calibrated. */
    if (getenv("FALCON_LIGHTTRACE") && strcmp(getenv("FALCON_LIGHTTRACE"), "1") == 0) {
      kintegrator->falcon_lighttrace = 1;
      kintegrator->falcon_lighttrace_gain = falcon_knob_float("FALCON_LIGHTTRACE_GAIN",
                                                              falcon_lt_gain);
      /* FALCON_LT_DIRECT=1 also splats the bounce-0 direct diffuse hit (not a
       * caustic) so its floor radiance can be matched to E*albedo/pi -- the
       * absolute-brightness calibration control. Off for real caustic renders. */
      kintegrator->falcon_lt_direct = falcon_knob_flag("FALCON_LT_DIRECT", falcon_lt_direct) ? 1 :
                                                                                              0;
      /* SPP of the driving render: the splat is multiplied by it to cancel the
       * combined pass's /sample_count divide (see falcon_lighttrace.h). Must
       * match cs.samples; fixed (non-adaptive) sampling only. */
      const char *lts = getenv("FALCON_LIGHTTRACE_SAMPLES");
      kintegrator->falcon_lt_samples = lts ? atoi(lts) : 1;
      /* FALCON_LT_SPLAT_RADIUS (px): Gaussian splat-reconstruction blur, the
       * labeled non-physical smoothness-for-photons trade (0 = physical). */
      const float r = falcon_knob_float("FALCON_LT_SPLAT_RADIUS", falcon_lt_splat_radius);
      kintegrator->falcon_lt_splat_radius = r > 0.0f ? r : 0.0f;
      /* FALCON_LT_VISIBILITY: occlusion ray vertex->camera before splatting.
       * Needs the raytrace kernel variant -- get_kernel_features() adds
       * KERNEL_FEATURE_NODE_RAYTRACE and intersect_closest routes all shading
       * there while this is set. */
      kintegrator->falcon_lt_visibility = falcon_knob_flag("FALCON_LT_VISIBILITY",
                                                           falcon_lt_visibility) ?
                                              1 :
                                              0;
    }

    /* GPU photon bake: the render becomes a photon pass (init_from_camera
     * emits from the first light, shade_surface deposits into the cache,
     * path_trace saves it). FALCON_PHOTON_N = total photons (pixels*samples of
     * the driving render), FALCON_PHOTON_GAIN = calibration knob. */
    Light *photon_light = nullptr;
    Object *photon_light_ob = nullptr;
    if (photon_mode && strcmp(photon_mode, "bake") == 0) {
      for (Object *ob : scene->objects) {
        if (ob->get_geometry() && ob->get_geometry()->is_light()) {
          Light *l = static_cast<Light *>(ob->get_geometry());
          if (l->get_is_enabled()) {
            photon_light = l;
            photon_light_ob = ob;
            break;
          }
        }
      }
    }
    /* Multi-light bake (operators.py): each light gets its own fully independent
     * bake call (separate cache/point files); the host merges the resulting
     * files afterward. An earlier attempt kept ONE device buffer alive across
     * repeated bpy.ops.render.render() calls (skipping the reset below on all
     * but the first light) to accumulate on-device -- this reliably crashed
     * the CUDA queue on the second render call ("Illegal address ... in
     * integrator_sorted_paths_array"), so device_vector state apparently isn't
     * safe to keep across separate render invocations. File-level merging
     * avoids touching that lifecycle at all.
     *
     * FALCON_PHOTON_ACCUM=1 accumulates through the FILE instead: start this
     * pass from the cache the previous one wrote, rather than from zeros. That
     * is the same alloc-and-upload the add-mode path already does every render,
     * so it does not touch the device_vector lifecycle that crashed.
     *
     * It exists because file-level merging cannot survive collision probing
     * (falcon_sharc.h): a site takes the first FREE slot on its chain, and
     * which slot that is depends on what else was already in the table, so two
     * independently baked passes put the same site in different slots and
     * summing the files element-wise mixes strangers. Measured on ocean at
     * shipping resolution, merging a lamp pass with a sky pass put 253 dark
     * specks into a frame that had 0 -- one site's light split across two
     * slots, each read back as a fixed step below its neighbours. Accumulating
     * through the file makes the second pass see the first pass's tags, so it
     * probes to the same answer. */
    const bool accumulate = false;
    /* ...and this one is the file-based version, which is NOT the same thing:
     * `accumulate` above means "the device buffer from the previous render is
     * still live, do not re-upload the host copy", and the point-map buffers
     * below rely on that reading (turning it on for them skips zeroing the
     * point counter -- the device then appends at a garbage index and the
     * whole context dies with an illegal address, measured 2026-08-14). */
    const bool grid_accumulate = getenv("FALCON_PHOTON_ACCUM") != nullptr;
    /* FALCON_PHOTON_WORLD=<L>: photon pass emitting from a uniform environment
     * of radiance L instead of a lamp (the world pass runs with every lamp
     * hidden, so photon_light is null then). Covers the world->glass->shadow
     * caustic component the exclusive separation removes from beauty and the
     * lamp LT passes never carry (LuxCore parity gap #2, 2026-07-06). */
    const char *world_env = (photon_mode && strcmp(photon_mode, "bake") == 0) ?
                                getenv("FALCON_PHOTON_WORLD") :
                                nullptr;
    const float world_radiance = world_env ? (float)atof(world_env) : 0.0f;
    if (photon_light || world_radiance > 0.0f) {
      /* ★2026-08-25: ライトトレースは格子に一度も書かない。
       * shade_surface.h の LT の分岐は `return LABEL_NONE` で抜けるので、
       * その下の堆積コードには到達しない。それなのにここで
       * 2^26 セル × 4 float = **1 GiB ちょうど**を確保し、memset して
       * GPU へ転送していた(毎レンダー)。読みもしないので、VRAM も時間も丸損。
       * 焼き(bake)と足し込み(add)は今までどおり要る。 */
      const bool sharc_needed = (kintegrator->falcon_lighttrace == 0);
      const size_t floats = sharc_needed ?
                                size_t(FALCON_SHARC_CELL_COUNT) * FALCON_SHARC_CELL_STRIDE :
                                0;
      if (!sharc_needed) {
        if (dscene->falcon_sharc_cache.size() != 0) {
          dscene->falcon_sharc_cache.free();
        }
      }
      else if (dscene->falcon_sharc_cache.size() != floats) {
        if (dscene->falcon_sharc_cache.size() != 0) {
          dscene->falcon_sharc_cache.free();
        }
        dscene->falcon_sharc_cache.alloc(floats);
      }
      LOG_INFO << "Falcon SHARC: " << (sharc_needed ? "alloc " : "skipped (light tracing) ")
               << (floats * sizeof(float) / (1024 * 1024)) << " MiB";
      bool loaded = false;
      if (sharc_needed && grid_accumulate) {
        /* Start from what the previous pass wrote. A missing or short file is
         * not an error -- the first pass of a bake has nothing to read. */
        FILE *pf = fopen(dscene->falcon_sharc_cache_path.c_str(), "rb");
        if (pf) {
          loaded = fread(dscene->falcon_sharc_cache.data(), sizeof(float), floats, pf) == floats;
          fclose(pf);
          if (loaded) {
            dscene->falcon_sharc_cache.copy_to_device();
            LOG_INFO << "Falcon Photon: accumulating into "
                     << dscene->falcon_sharc_cache_path;
          }
        }
      }
      if (sharc_needed) {
        if (!loaded) {
          memset(dscene->falcon_sharc_cache.data(), 0, floats * sizeof(float));
          dscene->falcon_sharc_cache.copy_to_device();
        }
        dscene->falcon_sharc_cache.clear_modified();
      }

      double n_photons = 1e6;
      const char *n_env = getenv("FALCON_PHOTON_N");
      if (n_env) {
        n_photons = atof(n_env);
      }
      /* ★焼き側では gain を掛けない(2026-08-16)。
       *
       * 以前はここでも掛けていて、読み出し側(shade_surface.h の
       * `cached *= falcon_photon_point_gain`)と**二重に掛かって g^2** に
       * なっていた。既定 1.0 では 1^2 = 1 なので出荷状態では気づけず、
       * スライダーを動かした時だけ効きが二乗になる。
       * 実測(glasszoo_worldfit・点マップ): gain 1 -> 2 で層の合計が
       * 181.8 -> 727.2、比ちょうど 4.000(正常なら 2.0)。
       *
       * どちらを残すかは説明文が決めている —— properties.py は
       * "applied at lookup time" かつ "Takes effect on the next render,
       * WITHOUT rebaking" と書いてある。**焼き側に掛けるとその約束が破れる**
       * (焼き直さないと変わらなくなる)ので、外すのは焼き側。
       *
       * 環境変数 FALCON_PHOTON_GAIN は焼きの光束を直接いじる逃げ道として
       * 残す(既定は 1.0 = 掛けない)。 */
      const float gain = falcon_knob_float("FALCON_PHOTON_GAIN", 1.0f);
      kintegrator->falcon_photon_pass = 1;

      Light *light = photon_light;
      const float watts = light ? (light->get_strength().x + light->get_strength().y +
                                   light->get_strength().z) /
                                      3.0f :
                                  0.0f;
      kintegrator->falcon_photon_is_sun = (light && light->get_light_type() == LIGHT_SUN) ?
                                              1 :
                                              0;
      kintegrator->falcon_photon_is_spot = (light && light->get_light_type() == LIGHT_SPOT) ? 1 :
                                                                                              0;
      kintegrator->falcon_photon_flux = (float)(watts * gain / n_photons);

      if (!light) {
        /* World (uniform environment) emission: parallel beams through the
         * caster bounding sphere's cross-section, one uniform direction per
         * photon. Power intercepted by the sphere = 4 pi^2 r^2 L (pbrt
         * UniformInfiniteLight), so flux/photon = that / N. The launch disk
         * center/radius come from FALCON_PHOTON_TARGET (fallback: whole-scene
         * bounding sphere) and ride in the sun_* fields; backoff pushes the
         * launch plane outside every bound so occlusion (e.g. the floor
         * blocking light from below) stays physical. */
        kintegrator->falcon_photon_is_world = 1;
        BoundBox bbox = BoundBox::empty;
        for (Object *ob : scene->objects) {
          if (ob->get_geometry() && ob->bounds.valid()) {
            bbox.grow(ob->bounds);
          }
        }
        float3 c = bbox.valid() ? 0.5f * (bbox.min + bbox.max) : zero_float3();
        float disk_r = bbox.valid() ? 0.5f * len(bbox.max - bbox.min) : 1.0f;
        const char *tgt_env = getenv("FALCON_PHOTON_TARGET");
        float tc[4];
        if (tgt_env && sscanf(tgt_env, "%f,%f,%f,%f", &tc[0], &tc[1], &tc[2], &tc[3]) == 4) {
          c = make_float3(tc[0], tc[1], tc[2]);
          disk_r = tc[3] * 1.05f;
        }
        float backoff = disk_r;
        if (bbox.valid()) {
          for (int i = 0; i < 8; i++) {
            const float3 corner = make_float3((i & 1) ? bbox.max.x : bbox.min.x,
                                              (i & 2) ? bbox.max.y : bbox.min.y,
                                              (i & 4) ? bbox.max.z : bbox.min.z);
            backoff = max(backoff, len(corner - c));
          }
        }
        backoff = backoff * 1.05f + 0.1f;
        kintegrator->falcon_photon_sun_minx = c.x;
        kintegrator->falcon_photon_sun_miny = c.y;
        kintegrator->falcon_photon_sun_z = c.z;
        kintegrator->falcon_photon_sun_sizex = disk_r;
        kintegrator->falcon_photon_sun_sizey = backoff;
        kintegrator->falcon_photon_flux = (float)(4.0 * M_PI_F * M_PI_F * double(disk_r) *
                                                  double(disk_r) * world_radiance * gain /
                                                  n_photons);
        LOG_INFO << "Falcon Photon: world emission, L " << world_radiance << " disk r " << disk_r
                 << " backoff " << backoff;
      }

      if (kintegrator->falcon_photon_is_spot) {
        /* Blender spot = point light (intensity W / 4 pi) clipped to the cone,
         * so the power actually leaving through the cone is W * Omega/(4 pi),
         * Omega = 2 pi (1 - cos(half angle)). Uniform cone sampling in the
         * kernel then gives every photon equal flux. */
        /* 5.2: spot cone angle moved to the SpotLight subclass. */
        const SpotLight *spot = dynamic_cast<const SpotLight *>(light);
        const float cos_half = cosf((spot ? spot->get_angle() : M_PI_F) * 0.5f);
        const float omega = M_2PI_F * (1.0f - cos_half);
        double share = 1.0;

        /* FALCON_PHOTON_TILE="ti,tj,n": emit only from tile (ti,tj) of an n x n
         * split of the cone's sampling square (u = the uniform cos-parameter,
         * v = the azimuth). Uniform cone sampling makes that split an exact
         * partition of the solid angle, so each tile carries 1/n^2 of the
         * power and the n^2 tiles add up to the untiled pass -- but each tile
         * can be given its own photon count. That is what emission guiding
         * needs: the guide field says the caustic fringe is still noisy while
         * its core has converged, and the fringe is fed by particular
         * directions out of this cone.
         *
         * The window rides in the sun_* fields, which the spot path does not
         * otherwise use, so no kernel data layout changes (see the 5.2 port
         * lesson about float2 alignment in data_template.h). sizex == 0 means
         * "no window", which is what every existing scene has. */
        kintegrator->falcon_photon_sun_minx = 0.0f;
        kintegrator->falcon_photon_sun_miny = 0.0f;
        kintegrator->falcon_photon_sun_sizex = 0.0f;
        kintegrator->falcon_photon_sun_sizey = 0.0f;
        const char *tile_env = getenv("FALCON_PHOTON_TILE");
        int ti = 0, tj = 0, tn = 0;
        if (tile_env && sscanf(tile_env, "%d,%d,%d", &ti, &tj, &tn) == 3 && tn > 0 && ti >= 0 &&
            tj >= 0 && ti < tn && tj < tn)
        {
          const float step = 1.0f / tn;
          kintegrator->falcon_photon_sun_minx = ti * step;
          kintegrator->falcon_photon_sun_miny = tj * step;
          kintegrator->falcon_photon_sun_sizex = step;
          kintegrator->falcon_photon_sun_sizey = step;
          share = double(step) * double(step);
          LOG_INFO << "Falcon Photon: spot cone tile " << ti << "," << tj << " of " << tn << "x"
                   << tn << " (solid-angle share " << share << ")";
        }
        kintegrator->falcon_photon_flux = (float)(watts * (omega / (4.0f * M_PI_F)) * share * gain /
                                                  n_photons);
      }

      if (kintegrator->falcon_photon_is_sun) {
        /* Parallel photons over the scene footprint. */
        BoundBox bbox = BoundBox::empty;
        for (Object *ob : scene->objects) {
          if (ob->get_geometry() && ob->bounds.valid()) {
            bbox.grow(ob->bounds);
          }
        }
        if (bbox.valid()) {
          kintegrator->falcon_photon_sun_minx = bbox.min.x;
          kintegrator->falcon_photon_sun_miny = bbox.min.y;
          kintegrator->falcon_photon_sun_sizex = bbox.max.x - bbox.min.x;
          kintegrator->falcon_photon_sun_sizey = bbox.max.y - bbox.min.y;
          kintegrator->falcon_photon_sun_z = bbox.max.z + 1.0f;
          /* Sun energy is irradiance; power over the footprint = E*cos*A.
           * The emission direction is the light object's -Z axis. */
          const Transform tfm = photon_light_ob->get_tfm();
          const float3 dir = -transform_get_column(&tfm, 2);
          const float3 ndir = normalize(dir);
          const float cos_zen = fabsf(ndir.z);

          /* FALCON_PHOTON_TARGET="cx,cy,cz,r": shrink the launch square to the
           * specular targets' bounding sphere instead of the whole scene
           * footprint (a 40 m floor wastes 99.9% of the photons on direct
           * floor hits that never deposit -- the GPU twin of the CPU tracer's
           * Round 8 sun fix). The sphere center is projected UP the sun
           * direction onto the launch plane so diagonal travel still hits it,
           * and the radius is stretched by 1/cos_zen (slanted footprint of a
           * sphere) plus 15% margin. Host-only: the kernel still launches from
           * an axis-aligned horizontal square. */
          const char *tgt_env = getenv("FALCON_PHOTON_TARGET");
          float tc[4];
          if (tgt_env && ndir.z < -0.05f &&
              sscanf(tgt_env, "%f,%f,%f,%f", &tc[0], &tc[1], &tc[2], &tc[3]) == 4) {
            const float t_up = (kintegrator->falcon_photon_sun_z - tc[2]) / (-ndir.z);
            const float cx = tc[0] - ndir.x * t_up;
            const float cy = tc[1] - ndir.y * t_up;
            const float pad = tc[3] * 1.15f / cos_zen;
            kintegrator->falcon_photon_sun_minx = cx - pad;
            kintegrator->falcon_photon_sun_miny = cy - pad;
            kintegrator->falcon_photon_sun_sizex = 2.0f * pad;
            kintegrator->falcon_photon_sun_sizey = 2.0f * pad;
            LOG_INFO << "Falcon Photon: sun launch targeted at (" << tc[0] << ", " << tc[1]
                     << ", " << tc[2] << ") r " << tc[3] << ", pad " << pad;
          }

          /* FALCON_PHOTON_TILE="ti,tj,n": launch only from tile (ti,tj) of an
           * n x n split of the square computed above. The tiles partition it
           * exactly, so rendering all n^2 tiles and adding the layers gives the
           * same total energy as one untiled pass -- with each tile free to
           * spend its own photon count. That is the knob emission guiding needs:
           * the fringe of a caustic (where the guide field says the light
           * tracer is still noisy) can be fed more photons than its converged
           * core, without touching the kernel. The split happens here because
           * this is where the launch square is known. */
          const char *tile_env = getenv("FALCON_PHOTON_TILE");
          int ti = 0, tj = 0, tn = 0;
          if (tile_env && sscanf(tile_env, "%d,%d,%d", &ti, &tj, &tn) == 3 && tn > 0 && ti >= 0 &&
              tj >= 0 && ti < tn && tj < tn)
          {
            const float tw = kintegrator->falcon_photon_sun_sizex / tn;
            const float th = kintegrator->falcon_photon_sun_sizey / tn;
            kintegrator->falcon_photon_sun_minx += ti * tw;
            kintegrator->falcon_photon_sun_miny += tj * th;
            kintegrator->falcon_photon_sun_sizex = tw;
            kintegrator->falcon_photon_sun_sizey = th;
            LOG_INFO << "Falcon Photon: launch tile " << ti << "," << tj << " of " << tn << "x"
                     << tn << " (" << tw << " x " << th << " m)";
          }

          const float area = kintegrator->falcon_photon_sun_sizex *
                             kintegrator->falcon_photon_sun_sizey;
          kintegrator->falcon_photon_flux = (float)(watts * cos_zen * area * gain / n_photons);
        }
      }
      LOG_INFO << "Falcon Photon: GPU bake pass, flux/photon "
               << kintegrator->falcon_photon_flux << " sun " << kintegrator->falcon_photon_is_sun;

      /* Point map (Round 9): allocate the raw-photon append buffer + counter.
       * FALCON_PHOTON_MAXPTS caps it (default 12M points = 432 MB); 0 disables
       * point storage (grid-only bake). Contents beyond the counter are never
       * read, so no zeroing of the point buffer itself. */
      int max_pts = 12000000;
      const char *maxpts_env = getenv("FALCON_PHOTON_MAXPTS");
      if (maxpts_env) {
        max_pts = atoi(maxpts_env);
      }
      if (max_pts > 0) {
        const size_t pfloats = (size_t)max_pts * 9;
        const bool points_realloc = dscene->falcon_photon_points.size() != pfloats;
        if (points_realloc) {
          if (dscene->falcon_photon_points.size() != 0) {
            dscene->falcon_photon_points.free();
          }
          dscene->falcon_photon_points.alloc(pfloats);
        }
        if (!accumulate || points_realloc) {
          /* Skipped when accumulating into an existing device buffer: the host
           * copy is stale (photon deposits only ever land device-side), so
           * uploading it here would overwrite the previous light's points with
           * whatever garbage/zeros the host array holds. */
          dscene->falcon_photon_points.copy_to_device();
        }
        dscene->falcon_photon_points.clear_modified();
        if (dscene->falcon_photon_pcount.size() != 1) {
          if (dscene->falcon_photon_pcount.size() != 0) {
            dscene->falcon_photon_pcount.free();
          }
          dscene->falcon_photon_pcount.alloc(1);
        }
        if (!accumulate) {
          dscene->falcon_photon_pcount.data()[0] = 0;
          dscene->falcon_photon_pcount.copy_to_device();
        }
        dscene->falcon_photon_pcount.clear_modified();
        kintegrator->falcon_photon_point_store = 1;
        kintegrator->falcon_photon_point_max = max_pts;
        LOG_INFO << "Falcon Photon: point buffer ready, cap " << max_pts << " points"
                 << (accumulate ? " (accumulating)" : "");
      }
    }
    if (photon_mode && strcmp(photon_mode, "add") == 0 && !sharc_active) {
      /* Point map (Round 9) takes priority: FALCON_PHOTON_POINTS file =
       * header {magic 'FPH1', uint32 count} + count * 9 floats (pos3, flux3,
       * normal3). The neighbor grid is rebuilt here every scene update, so the
       * lookup radius is a pure render-time knob. */
      const char *pts_env = getenv("FALCON_PHOTON_POINTS");
      FILE *pf = pts_env ? fopen(pts_env, "rb") : nullptr;
      if (pf) {
        uint32_t header[2] = {0, 0};
        size_t count = 0;
        if (fread(header, sizeof(uint32_t), 2, pf) == 2 && header[0] == 0x46504831u) {
          count = header[1];
        }
        if (count > 0) {
          const size_t pfloats = count * 9;
          if (dscene->falcon_photon_points.size() != pfloats) {
            if (dscene->falcon_photon_points.size() != 0) {
              dscene->falcon_photon_points.free();
            }
            dscene->falcon_photon_points.alloc(pfloats);
          }
          float *pts = dscene->falcon_photon_points.data();
          if (fread(pts, sizeof(float), pfloats, pf) == pfloats) {
            float radius = falcon_knob_float("FALCON_PHOTON_RADIUS_M",
                                             falcon_photon_point_radius_m);
            radius = radius > 1e-4f ? radius : 1e-4f;
            const float normal_deg = falcon_knob_float("FALCON_PHOTON_NORMAL_DEG",
                                                       falcon_photon_point_normal_deg);
            const float gain = falcon_knob_float("FALCON_PHOTON_GAIN", falcon_photon_point_gain);

            /* Counting sort of photon indices into the uniform neighbor grid
             * (cell = radius; slot = hash_uint3 of cell coords). */
            const uint32_t GRID = 1u << 22;
            const uint32_t MASK = GRID - 1u;
            std::vector<uint32_t> keys(count);
            const float inv = 1.0f / radius;
            for (size_t i = 0; i < count; i++) {
              const float *p = pts + i * 9;
              const uint gx = (uint)((int)floorf(p[0] * inv));
              const uint gy = (uint)((int)floorf(p[1] * inv));
              const uint gz = (uint)((int)floorf(p[2] * inv));
              keys[i] = hash_uint3(gx, gy, gz) & MASK;
            }
            if (dscene->falcon_photon_grid_start.size() != GRID) {
              if (dscene->falcon_photon_grid_start.size() != 0) {
                dscene->falcon_photon_grid_start.free();
                dscene->falcon_photon_grid_count.free();
              }
              dscene->falcon_photon_grid_start.alloc(GRID);
              dscene->falcon_photon_grid_count.alloc(GRID);
            }
            uint *gstart = dscene->falcon_photon_grid_start.data();
            uint *gcount = dscene->falcon_photon_grid_count.data();
            memset(gcount, 0, GRID * sizeof(uint));
            for (size_t i = 0; i < count; i++) {
              gcount[keys[i]]++;
            }
            uint32_t run = 0;
            for (uint32_t k = 0; k < GRID; k++) {
              gstart[k] = run;
              run += gcount[k];
            }
            if (dscene->falcon_photon_index.size() != count) {
              if (dscene->falcon_photon_index.size() != 0) {
                dscene->falcon_photon_index.free();
              }
              dscene->falcon_photon_index.alloc(count);
            }
            uint *gindex = dscene->falcon_photon_index.data();
            std::vector<uint32_t> cursor(gstart, gstart + GRID);
            for (size_t i = 0; i < count; i++) {
              gindex[cursor[keys[i]]++] = (uint32_t)i;
            }

            dscene->falcon_photon_points.copy_to_device();
            dscene->falcon_photon_points.clear_modified();
            dscene->falcon_photon_grid_start.copy_to_device();
            dscene->falcon_photon_grid_start.clear_modified();
            dscene->falcon_photon_grid_count.copy_to_device();
            dscene->falcon_photon_grid_count.clear_modified();
            dscene->falcon_photon_index.copy_to_device();
            dscene->falcon_photon_index.clear_modified();

            kintegrator->falcon_photon_add = 1;
            kintegrator->falcon_photon_point_mode = 1;
            kintegrator->falcon_photon_point_radius = radius;
            kintegrator->falcon_photon_point_cos = cosf(normal_deg * (float)M_PI / 180.0f);
            kintegrator->falcon_photon_point_gain = gain;
            LOG_INFO << "Falcon Photon: point map loaded, " << count << " points from "
                     << pts_env << " (radius " << radius << " m, normal " << normal_deg
                     << " deg, gain " << gain << ")";
          }
        }
        fclose(pf);
      }

      if (!kintegrator->falcon_photon_point_mode) {
        const string &path = dscene->falcon_sharc_cache_path;
        FILE *f = fopen(path.c_str(), "rb");
        if (f) {
          const size_t floats = size_t(FALCON_SHARC_CELL_COUNT) * FALCON_SHARC_CELL_STRIDE;
          if (dscene->falcon_sharc_cache.size() != floats) {
            if (dscene->falcon_sharc_cache.size() != 0) {
              dscene->falcon_sharc_cache.free();
            }
            dscene->falcon_sharc_cache.alloc(floats);
          }
          if (fread(dscene->falcon_sharc_cache.data(), sizeof(float), floats, f) == floats) {
            dscene->falcon_sharc_cache.copy_to_device();
            dscene->falcon_sharc_cache.clear_modified();
            kintegrator->falcon_photon_add = 1;
            LOG_INFO << "Falcon Photon: additive caustic cache loaded from " << path
                     << " (cell " << cell << ")";
          }
          fclose(f);
        }
      }
    }

    if (sharc_active) {
      /* Allocate and zero the spatial hash radiance cache. Size must match
       * FALCON_SHARC_CELL_COUNT * FALCON_SHARC_CELL_STRIDE in
       * kernel/integrator/falcon_sharc.h (64M cells * 4 floats = 1 GB). Read-only
       * from the kernel; the host fills it during the warmup pass. */
      const size_t falcon_sharc_floats = size_t(FALCON_SHARC_CELL_COUNT) * FALCON_SHARC_CELL_STRIDE;
      if (dscene->falcon_sharc_cache.size() != falcon_sharc_floats) {
        if (dscene->falcon_sharc_cache.size() != 0) {
          dscene->falcon_sharc_cache.free();
        }
        float *cache = (float *)dscene->falcon_sharc_cache.alloc(falcon_sharc_floats);
        memset(cache, 0, falcon_sharc_floats * sizeof(float));
        dscene->falcon_sharc_cache.copy_to_device();
      }
      dscene->falcon_sharc_cache.clear_modified();

      /* Blend factor: runtime knob via FALCON_SHARC_ALPHA (default 0.7, clamped
       * to [0,1]). Uploaded in kernel_data so tuning needs no recompile. */
      float alpha = falcon_knob_float("FALCON_SHARC_ALPHA", falcon_sharc_alpha);
      alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);

      /* Auto GI gate: SHARC helps GI-dominated scenes but hurts direct-lit ones
       * (Phase0 data), so scale alpha by the scene's GI dominance measured during
       * warmup (indirect / (direct + indirect), stored in <cache>.meta). The gate
       * smoothsteps from off below GATE_LOW to full above GATE_HIGH, so a direct-
       * lit scene self-disables SHARC. Only for blend/live (warmup's blend is a
       * no-op). Missing meta or FALCON_SHARC_GATE=0 -> no gating (alpha as-is). */
      const bool is_blend = sharc_mode == FALCON_SHARC_MODE_BLEND ||
                            sharc_mode == FALCON_SHARC_MODE_LIVE;
      const bool gate_on = falcon_knob_flag("FALCON_SHARC_GATE", falcon_sharc_gate);
      if (is_blend && gate_on) {
        const string meta_path = dscene->falcon_sharc_cache_path + ".meta";
        FILE *mf = fopen(meta_path.c_str(), "r");
        if (mf) {
          float gi_ratio = 1.0f;
          if (fscanf(mf, "%f", &gi_ratio) == 1) {
            const float low = falcon_knob_float("FALCON_SHARC_GATE_LOW", falcon_sharc_gate_low);
            const float high = falcon_knob_float("FALCON_SHARC_GATE_HIGH", falcon_sharc_gate_high);
            float t = (high > low) ? (gi_ratio - low) / (high - low) : (gi_ratio >= high);
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            const float gate = t * t * (3.0f - 2.0f * t);
            LOG_INFO << "Falcon SHARC: GI gate " << gate << " (ratio " << gi_ratio << ", alpha "
                     << alpha << " -> " << (alpha * gate) << ")";
            alpha *= gate;
          }
          fclose(mf);
        }
      }
      kintegrator->falcon_sharc_alpha = alpha;
    }
    else {
      /* Not active: release the cache if a previous active render allocated it,
       * so a normal render reserves no extra VRAM. (Not when the photon cache
       * is using the same buffer -- add mode reads it, bake mode writes it.) */
      if (!kintegrator->falcon_photon_add && !kintegrator->falcon_photon_pass &&
          dscene->falcon_sharc_cache.size() != 0) {
        dscene->falcon_sharc_cache.free();
      }
      kintegrator->falcon_sharc_alpha = 0.0f;
    }
  }

  /* Falcon DAS (denoiser-aware sampling). FALCON_DAS_MAP points at a per-pixel
   * threshold-scale map built by the OIDN probe (raw file: int32 width, int32
   * height, then width*height float32 scales, render_pixel_index order). Only
   * valid for full-frame renders at exactly that resolution: the kernel indexes
   * the map by render_pixel_index, so border/tiled renders would misalign.
   *
   * That "only valid at exactly that resolution" was a comment, not a check
   * (2026-08-21). The header's dims sized the allocation while the kernel kept
   * indexing by render_pixel_index, so a 12-byte das.raw claiming 1x1 next to a
   * 1920x1080 render had the kernel read up to 2.07M floats out of a 4-byte
   * allocation -- and the path arrives from the .blend, which is not re-clamped
   * on load. Two things now stop it: the resolution has to match here, and the
   * kernel bounds-checks the index (falcon_das_active carries the element
   * count). Neither changes anything for a correctly built map. */
  {
    kintegrator->falcon_das_active = 0;
    kintegrator->falcon_das_strength = 0.0f;
    const string map_path = falcon_knob_string("FALCON_DAS_MAP", falcon_das_map);
    if (!map_path.empty()) {
      const int render_width = scene->camera ? scene->camera->get_full_width() : 0;
      const int render_height = scene->camera ? scene->camera->get_full_height() : 0;
      FILE *f = fopen(map_path.c_str(), "rb");
      if (f) {
        int32_t dims[2] = {0, 0};
        const bool header_ok = (fread(dims, sizeof(int32_t), 2, f) == 2);
        if (header_ok && dims[0] > 0 && dims[1] > 0 && dims[0] == render_width &&
            dims[1] == render_height)
        {
          const size_t count = (size_t)dims[0] * (size_t)dims[1];
          if (dscene->falcon_das_scale.size() != count) {
            if (dscene->falcon_das_scale.size() != 0) {
              dscene->falcon_das_scale.free();
            }
            dscene->falcon_das_scale.alloc(count);
          }
          float *map = dscene->falcon_das_scale.data();
          if (fread(map, sizeof(float), count, f) == count) {
            dscene->falcon_das_scale.copy_to_device();
            dscene->falcon_das_scale.clear_modified();
            const float strength = falcon_knob_float("FALCON_DAS_STRENGTH", falcon_das_strength);
            kintegrator->falcon_das_active = (int)count;
            kintegrator->falcon_das_strength = strength;
            LOG_INFO << "Falcon DAS: loaded " << dims[0] << "x" << dims[1]
                     << " threshold-scale map (strength " << strength << ")";
          }
        }
        else {
          LOG_WARNING << "Falcon DAS: " << map_path << " は " << dims[0] << "x" << dims[1]
                      << " で、このレンダーの " << render_width << "x" << render_height
                      << " と一致しないので使いません";
        }
        fclose(f);
      }
    }
    if (!kintegrator->falcon_das_active && dscene->falcon_das_scale.size() != 0) {
      dscene->falcon_das_scale.free();
    }
  }

  /* Falcon error field: the measured relative error per world cell, written by
   * the probe tooling (cyclesf-lab/tools). One float per SHARC hash slot, in
   * the same hash the kernel uses, with a negative value for cells the probe
   * never reached. Armed only when a threshold is set, so a scene that does
   * not opt in pays neither the 16 MB nor the lookup. */
  {
    kintegrator->falcon_error_cell_size = 0.8f;
    kintegrator->falcon_error_threshold = 0.0f;
    kintegrator->falcon_error_raise_alpha = 0;
    /* falcon_error_pad1 は falcon_photon_contact_cells になった(上で設定済み) */
    const float threshold = falcon_knob_float("FALCON_ERROR_THRESHOLD", falcon_error_threshold);
    const string field_path = falcon_knob_string("FALCON_ERROR_FIELD", falcon_error_map);
    if (threshold > 0.0f && !field_path.empty()) {
      FILE *f = fopen(field_path.c_str(), "rb");
      if (f) {
        uint32_t header[2] = {0, 0};
        float cell_size = 0.0f;
        const size_t cells = size_t(FALCON_SHARC_CELL_COUNT);
        if (fread(header, sizeof(uint32_t), 2, f) == 2 && header[0] == 0x46454631u &&
            header[1] == cells && fread(&cell_size, sizeof(float), 1, f) == 1 &&
            cell_size > 1e-4f)
        {
          if (dscene->falcon_error_field.size() != cells) {
            if (dscene->falcon_error_field.size() != 0) {
              dscene->falcon_error_field.free();
            }
            dscene->falcon_error_field.alloc(cells);
          }
          float *field = dscene->falcon_error_field.data();
          if (fread(field, sizeof(float), cells, f) == cells) {
            dscene->falcon_error_field.copy_to_device();
            dscene->falcon_error_field.clear_modified();
            kintegrator->falcon_error_cell_size = cell_size;
            kintegrator->falcon_error_threshold = threshold;
            kintegrator->falcon_error_raise_alpha = falcon_knob_flag("FALCON_ERROR_RAISE_ALPHA",
                                                                    falcon_error_raise_alpha) ?
                                                        1 :
                                                        0;
            size_t filled = 0;
            for (size_t i = 0; i < cells; i++) {
              filled += (field[i] >= 0.0f);
            }
            LOG_INFO << "Falcon error field: " << filled << " measured cells from " << field_path
                     << " (cell " << cell_size << " m), terminating at error <= " << threshold;
          }
        }
        else {
          LOG_WARNING << "Falcon error field: " << field_path
                      << " is not a 4M-cell field file, ignored";
        }
        fclose(f);
      }
    }
    if (kintegrator->falcon_error_threshold <= 0.0f && dscene->falcon_error_field.size() != 0) {
      dscene->falcon_error_field.free();
    }
  }
#endif

  clear_modified();
}

void Integrator::device_free(Device * /*unused*/, DeviceScene *dscene, bool force_free)
{
  dscene->sample_pattern_lut.free_if_need_realloc(force_free);
#ifdef WITH_FALCON_SHARC
  dscene->falcon_sharc_cache.free_if_need_realloc(force_free);
  dscene->falcon_das_scale.free_if_need_realloc(force_free);
  dscene->falcon_error_field.free_if_need_realloc(force_free);
#endif
}

bool Integrator::is_modified() const
{
  return Node::is_modified() || shadow_catcher_needs_recalc_;
}

void Integrator::clear_modified()
{
  Node::clear_modified();
  shadow_catcher_needs_recalc_ = false;
}

void Integrator::tag_update(Scene *scene, const uint32_t flag)
{
  if (flag == UPDATE_ALL) {
    tag_modified();
  }

  if (flag & AO_PASS_MODIFIED) {
    /* tag only the ao_bounces socket as modified so we avoid updating sample_pattern_lut
     * unnecessarily */
    tag_ao_bounces_modified();
  }

  if (flag & OBJECT_MANAGER) {
    shadow_catcher_needs_recalc_ = true;
  }

  if (motion_blur_is_modified()) {
    scene->object_manager->tag_update(scene, ObjectManager::MOTION_BLUR_MODIFIED);
    scene->camera->tag_modified();
  }

  if (volume_ray_marching_is_modified()) {
    scene->volume_manager->tag_update_algorithm();
    scene->geometry_manager->tag_update(scene, GeometryManager::VOLUME_MODIFIED);
  }
}

uint Integrator::get_kernel_features() const
{
  uint kernel_features = 0;

  if (ao_additive_factor != 0.0f) {
    kernel_features |= KERNEL_FEATURE_AO_ADDITIVE;
  }

  if (get_use_light_tree()) {
    kernel_features |= KERNEL_FEATURE_LIGHT_TREE;
  }

  /* Falcon light tracing with the visibility ray traces vertex->camera from
   * shade_surface: the raytrace kernel variant must be in the pipeline (on
   * OptiX the plain kernel cannot trace). */
#ifdef WITH_FALCON_SHARC
  if (getenv("FALCON_LIGHTTRACE") && falcon_knob_flag("FALCON_LT_VISIBILITY", falcon_lt_visibility))
  {
    kernel_features |= KERNEL_FEATURE_NODE_RAYTRACE;
  }
#endif

  return kernel_features;
}

AdaptiveSampling Integrator::get_adaptive_sampling() const
{
  AdaptiveSampling adaptive_sampling;

  adaptive_sampling.use = use_adaptive_sampling;

  /* Disable sample count pass with upscaling. */
  if (use_denoise && denoiser_upscale_factor != 1.0f) {
    adaptive_sampling.use = false;
  }

  if (!adaptive_sampling.use) {
    return adaptive_sampling;
  }

  const int clamped_aa_samples = min(aa_samples, MAX_SAMPLES);

  if (clamped_aa_samples > 0 && adaptive_threshold == 0.0f) {
    adaptive_sampling.threshold = max(0.001f, 1.0f / (float)aa_samples);
    LOG_INFO << "Adaptive sampling: automatic threshold = " << adaptive_sampling.threshold;
  }
  else {
    adaptive_sampling.threshold = adaptive_threshold;
  }

  if (use_sample_subset && clamped_aa_samples > 0) {
    const int subset_samples = max(
        min(sample_subset_offset + sample_subset_length, clamped_aa_samples) -
            sample_subset_offset,
        0);

    adaptive_sampling.threshold *= sqrtf((float)subset_samples / (float)clamped_aa_samples);
  }

  if (adaptive_sampling.threshold > 0 && adaptive_min_samples == 0) {
    /* Threshold 0.1 -> 32, 0.01 -> 64, 0.001 -> 128.
     * This is highly scene dependent, we make a guess that seemed to work well
     * in various test scenes. */
    const int min_samples = (int)ceilf(16.0f / powf(adaptive_sampling.threshold, 0.3f));
    adaptive_sampling.min_samples = max(4, min_samples);
    LOG_INFO << "Adaptive sampling: automatic min samples = " << adaptive_sampling.min_samples;
  }
  else {
    adaptive_sampling.min_samples = max(4, adaptive_min_samples);
  }

  /* Arbitrary factor that makes the threshold more similar to what is was before,
   * and gives arguably more intuitive values. */
  adaptive_sampling.threshold *= 5.0f;

  adaptive_sampling.adaptive_step = 16;

  DCHECK(is_power_of_two(adaptive_sampling.adaptive_step))
      << "Adaptive step must be a power of two for bitwise operations to work";

  return adaptive_sampling;
}

DenoiseParams Integrator::get_denoise_params() const
{
  DenoiseParams denoise_params;

  denoise_params.use = use_denoise;

  denoise_params.type = denoiser_type;

  denoise_params.use_gpu = denoise_use_gpu;

  denoise_params.start_sample = denoise_start_sample;

  denoise_params.passes = denoiser_passes;

  denoise_params.prefilter = denoiser_prefilter;
  denoise_params.quality = denoiser_quality;
  denoise_params.upscale_factor = denoiser_upscale_factor;
  denoise_params.carry_history = denoiser_carry_history;
  denoise_params.preroll_passes = denoiser_preroll_passes;
  denoise_params.preroll_passes_cut = denoiser_preroll_passes_cut;
  denoise_params.cut_warmup = denoiser_cut_warmup;

  return denoise_params;
}

GuidingParams Integrator::get_guiding_params(const Device *device) const
{
  const bool use = use_guiding && device->info.has_guiding;

  GuidingParams guiding_params;
  guiding_params.use_surface_guiding = use && use_surface_guiding &&
                                       surface_guiding_probability > 0.0f;
  guiding_params.use_volume_guiding = use && use_volume_guiding &&
                                      volume_guiding_probability > 0.0f;
  guiding_params.use = guiding_params.use_surface_guiding || guiding_params.use_volume_guiding;
  guiding_params.type = guiding_distribution_type;
  guiding_params.training_samples = guiding_training_samples;
  guiding_params.deterministic = deterministic_guiding;
  guiding_params.sampling_type = guiding_directional_sampling_type;
  // In Blender/Cycles the user set roughness is squared to behave more linear.
  guiding_params.roughness_threshold = guiding_roughness_threshold * guiding_roughness_threshold;
  return guiding_params;
}
CCL_NAMESPACE_END
