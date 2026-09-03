/* SPDX-FileCopyrightText: 2025 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_DLSS

#  include "integrator/denoiser_dlss.h"
#  include "integrator/pass_accessor_gpu.h"

#  include "device/cuda/device_impl.h"

#  include "util/path.h"

#  include <cstring>

#  include <nvsdk_ngx.h>
#  include <nvsdk_ngx_defs_dlssd.h>

#  ifdef _WIN32
#    include "util/windows.h"

#    define dynamic_library_open(path) LoadLibraryW(path)
#    define dynamic_library_close(lib) FreeLibrary(static_cast<HMODULE>(lib))
#    define dynamic_library_find(lib, symbol) \
      reinterpret_cast<t##symbol>(GetProcAddress(static_cast<HMODULE>(lib), #symbol))
#  else
#    include <dlfcn.h>

#    define dynamic_library_open(path) dlopen(path, RTLD_NOW)
#    define dynamic_library_close(lib) dlclose(lib)
#    define dynamic_library_find(lib, symbol) reinterpret_cast<t##symbol>(dlsym(lib, #symbol))
#  endif

struct NGXDriver {
  using tNVSDK_NGX_CUDA_Init_Ext1 = NVSDK_NGX_Result (*)(unsigned long long,
                                                         const wchar_t *,
                                                         NVSDK_NGX_CUDADevice *,
                                                         NVSDK_NGX_Version,
                                                         const NVSDK_NGX_FeatureCommonInfo *);
  using tNVSDK_NGX_CUDA_Shutdown1 = NVSDK_NGX_Result (*)(NVSDK_NGX_CUDADevice *, unsigned int &);
  using tNVSDK_NGX_CUDA_GetFeatureRequirements = decltype(&NVSDK_NGX_CUDA_GetFeatureRequirements);
  using tNVSDK_NGX_CUDA_CreateFeature1 = decltype(&NVSDK_NGX_CUDA_CreateFeature1);
  using tNVSDK_NGX_CUDA_EvaluateFeature = decltype(&NVSDK_NGX_CUDA_EvaluateFeature);
  using tNVSDK_NGX_CUDA_ReleaseFeature = decltype(&NVSDK_NGX_CUDA_ReleaseFeature);
  using tNVSDK_NGX_CUDA_AllocateParameters = decltype(&NVSDK_NGX_CUDA_AllocateParameters);
  using tNVSDK_NGX_CUDA_DestroyParameters = decltype(&NVSDK_NGX_CUDA_DestroyParameters);

  tNVSDK_NGX_CUDA_Init_Ext1 Init_Ext1 = nullptr;
  tNVSDK_NGX_CUDA_Shutdown1 Shutdown1 = nullptr;
  tNVSDK_NGX_CUDA_GetFeatureRequirements GetFeatureRequirements = nullptr;
  tNVSDK_NGX_CUDA_CreateFeature1 CreateFeature1 = nullptr;
  tNVSDK_NGX_CUDA_EvaluateFeature EvaluateFeature = nullptr;
  tNVSDK_NGX_CUDA_ReleaseFeature ReleaseFeature = nullptr;
  tNVSDK_NGX_CUDA_AllocateParameters AllocateParameters = nullptr;
  tNVSDK_NGX_CUDA_DestroyParameters DestroyParameters = nullptr;

  explicit operator bool() const
  {
    return Init_Ext1 != nullptr && Shutdown1 != nullptr && CreateFeature1 != nullptr &&
           EvaluateFeature != nullptr && ReleaseFeature != nullptr &&
           AllocateParameters != nullptr && DestroyParameters != nullptr;
  }

  bool init()
  {
    if (*this) {
      return true;
    }

#  ifdef _WIN32
    WCHAR ngx_path[MAX_PATH] = L"";
    {
      HKEY ngx_key = nullptr;
      LSTATUS result = RegOpenKeyExW(
          HKEY_LOCAL_MACHINE,
          L"System\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore",
          0,
          KEY_READ,
          &ngx_key);
      if (result != ERROR_SUCCESS) {
        result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                               L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore",
                               0,
                               KEY_READ,
                               &ngx_key);
      }
      if (result == ERROR_SUCCESS) {
        DWORD ngx_path_size = ARRAYSIZE(ngx_path);
        result = RegQueryValueExW(
            ngx_key, L"NGXPath", 0, nullptr, reinterpret_cast<LPBYTE>(ngx_path), &ngx_path_size);
        RegCloseKey(ngx_key);
      }
      if (result != ERROR_SUCCESS) {
        return false;
      }

      wcscat_s(ngx_path, L"\\_nvngx.dll");
    }
#  else
    const char *const ngx_path = "libnvidia-ngx.so.1";
#  endif

    void *const ngx_module = dynamic_library_open(ngx_path);
    if (ngx_module == nullptr) {
      return false;
    }

    Init_Ext1 = dynamic_library_find(ngx_module, NVSDK_NGX_CUDA_Init_Ext1);
    Shutdown1 = dynamic_library_find(ngx_module, NVSDK_NGX_CUDA_Shutdown1);
    GetFeatureRequirements = dynamic_library_find(ngx_module,
                                                  NVSDK_NGX_CUDA_GetFeatureRequirements);
    CreateFeature1 = dynamic_library_find(ngx_module, NVSDK_NGX_CUDA_CreateFeature1);
    EvaluateFeature = dynamic_library_find(ngx_module, NVSDK_NGX_CUDA_EvaluateFeature);
    ReleaseFeature = dynamic_library_find(ngx_module, NVSDK_NGX_CUDA_ReleaseFeature);
    AllocateParameters = dynamic_library_find(ngx_module, NVSDK_NGX_CUDA_AllocateParameters);
    DestroyParameters = dynamic_library_find(ngx_module, NVSDK_NGX_CUDA_DestroyParameters);

    if (*this) {
      return true;
    }
    else {
      dynamic_library_close(ngx_module);
      return false;
    }
  }
} NVSDK_NGX_CUDA;

CCL_NAMESPACE_BEGIN

static const int ApplicationId = 100334311;

/* FALCON_DLSS_COLOR_SCALE: multiply the colour handed to RR by this factor
 * and divide the result back (exposure applied by hand, since RR ignores the
 * exposure parameters). Sweep knob, default 1. */
static float dlss_color_scale()
{
  static const float value = getenv("FALCON_DLSS_COLOR_SCALE") ?
                                 (float)atof(getenv("FALCON_DLSS_COLOR_SCALE")) :
                                 1.0f;
  return value > 0.0f ? value : 1.0f;
}

static bool dlss_demodulate()
{
  static const bool value = getenv("FALCON_DLSS_DEMOD") ? atoi(getenv("FALCON_DLSS_DEMOD")) != 0 : false;
  return value;
}

void DLSSDenoiser::CUDATexture::init(Device *device, int width, int height, int num_components)
{
  CUDA_ARRAY_DESCRIPTOR desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.Format = CU_AD_FORMAT_FLOAT;
  desc.NumChannels = num_components;

  cuda_device_assert(device, cuArrayCreate((CUarray *)&array, &desc));

  CUDA_TEXTURE_DESC tex_desc = {};
  tex_desc.addressMode[0] = CU_TR_ADDRESS_MODE_CLAMP;
  tex_desc.addressMode[1] = CU_TR_ADDRESS_MODE_CLAMP;
  tex_desc.addressMode[2] = CU_TR_ADDRESS_MODE_CLAMP;
  tex_desc.flags = CU_TRSF_NORMALIZED_COORDINATES;

  CUDA_RESOURCE_DESC res_desc = {};
  res_desc.resType = CU_RESOURCE_TYPE_ARRAY;
  res_desc.res.array.hArray = (CUarray)array;

  cuda_device_assert(
      device, cuTexObjectCreate((CUtexObject *)&texture_handle, &res_desc, &tex_desc, nullptr));
  cuda_device_assert(device, cuSurfObjectCreate((CUsurfObject *)&surface_handle, &res_desc));
}
void DLSSDenoiser::CUDATexture::destroy()
{
  cuSurfObjectDestroy((CUsurfObject)surface_handle);
  surface_handle = 0;
  cuTexObjectDestroy((CUtexObject)texture_handle);
  texture_handle = 0;

  cuArrayDestroy((CUarray)array);
  array = 0;
}

bool DLSSDenoiser::use_carry_history(const DenoiseContext &context) const
{
  if (!params_.carry_history || getenv("FALCON_DLSS_NO_CARRY") != nullptr) {
    return false;
  }

  /* The spp-based automatic switch measured better at 32 spp (slowcam 0.02541
   * -> 0.02403) but the difference is not visible by eye, and the person who
   * looks at the picture asked for the smoother motion of a carried history
   * knowing what it costs in ghosting. So the panel's carry_history is the
   * authority: ON carries whatever the spp, OFF never does. The automatic
   * switch stays available as an explicit opt-in (FALCON_DLSS_CARRY_AUTO=1). */
  static const bool carry_auto = getenv("FALCON_DLSS_CARRY_AUTO") ?
                                     atoi(getenv("FALCON_DLSS_CARRY_AUTO")) != 0 :
                                     false;
  if (!carry_auto) {
    return true;
  }

  /* A pre-roll pass is a re-render of the frame that is already being denoised;
   * its chain must not be broken (see the pre-roll notes in denoise_run). */
  if (same_frame_restart_ || preroll_pass_) {
    return true;
  }

  static const int carry_max_spp = getenv("FALCON_DLSS_CARRY_MAX_SPP") ?
                                       atoi(getenv("FALCON_DLSS_CARRY_MAX_SPP")) :
                                       8;
  return context.num_samples < carry_max_spp;
}

bool DLSSDenoiser::is_frame_transition(const DenoiseContext &context) const
{
  /* "Did the frame change?" used to be read off the sample count: a new frame
   * starts its accumulation over, so num_samples dropping back down meant a
   * transition. That holds in the viewport (navigation and edits restart at 1),
   * but not in a final render with a time limit or adaptive sampling, where the
   * spp a frame ends on moves up and down freely. Every frame that happened to
   * finish with *more* samples than the one before it was read as "more samples
   * on the same frame": the motion vectors were zeroed (the history was left
   * unwarped over a moving camera, smearing foliage radially) and Reset was
   * never raised. Measured on the tree scene, frames whose spp went up had a
   * sharpness ratio of 0.89 against 1.04 for the rest.
   *
   * So ask the frame number, which is what the question was about all along.
   * The sample-restart test is kept as an OR: the viewport reuses one frame
   * number across navigation restarts, and there the drop *is* the transition.
   *
   * FALCON_DLSS_FRAME_TRANSITION=spp restores the old sample-only rule. */
  static const bool use_frame_number = []() {
    const char *mode = getenv("FALCON_DLSS_FRAME_TRANSITION");
    return !(mode && strcmp(mode, "spp") == 0);
  }();

  const bool sample_restart = context.num_samples <= last_num_samples_;
  const bool frame_changed = use_frame_number && last_frame_ != INT_MIN &&
                             frame_ != last_frame_;

  return sample_restart || frame_changed;
}

bool DLSSDenoiser::use_transparency_guide(const DenoiseContext &context) const
{
  /* FALCON_DLSS_NO_TRANSPARENCY_GUIDE=1: sweep knob, leave ColorBeforeTransparency unset. */
  static const bool disabled = getenv("FALCON_DLSS_NO_TRANSPARENCY_GUIDE") != nullptr;
  if (disabled) {
    return false;
  }
  return context.buffer_params.get_pass_offset(PASS_TRANSMISSION_DIRECT) != PASS_UNUSED &&
         context.buffer_params.get_pass_offset(PASS_TRANSMISSION_INDIRECT) != PASS_UNUSED;
}

bool DLSSDenoiser::use_layer_guides() const
{
  /* FALCON_DLSS_LAYER_GUIDES=1: hand RR ColorBeforeParticles (= colour, no
   * particle pass exists in Cycles), ColorBeforeTransparency (as before) and
   * ColorBeforeFog (= colour minus the volume passes) together. Default off. */
  static const bool enabled = getenv("FALCON_DLSS_LAYER_GUIDES") ?
                                  atoi(getenv("FALCON_DLSS_LAYER_GUIDES")) != 0 :
                                  false;
  return enabled;
}

bool DLSSDenoiser::use_emissive_guide(const DenoiseContext &context) const
{
  /* GBuffer_Emissive: what the surface emits by itself. RR otherwise has to read a
   * bright emitter as noisy lighting and average it with its neighbours. The pass only
   * exists when FALCON_DLSS_EMISSIVE_GUIDE=1 asked sync.cpp for it. */
  return context.buffer_params.get_pass_offset(PASS_EMISSION) != PASS_UNUSED;
}

bool DLSSDenoiser::use_specular_hit_distance(const DenoiseContext &context) const
{
  return context.buffer_params.get_pass_offset(PASS_DENOISING_SPECULAR_HIT_DISTANCE) !=
             PASS_UNUSED &&
         have_camera_matrices_;
}

void DLSSDenoiser::set_camera_matrices(const float *world_to_view, const float *view_to_clip)
{
  std::copy_n(world_to_view, 16, world_to_view_);
  std::copy_n(view_to_clip, 16, view_to_clip_);
  have_camera_matrices_ = true;
}

bool DLSSDenoiser::use_specular_motion(const DenoiseContext &context) const
{
  return context.buffer_params.get_pass_offset(PASS_DENOISING_SPECULAR_MOTION) != PASS_UNUSED &&
         getenv("FALCON_DLSS_ZERO_SPECULAR_MOTION") == nullptr;
}

DLSSDenoiser::DLSSDenoiser(Device *denoiser_device, const DenoiseParams &params)
    : DenoiserGPU(denoiser_device, params)
{
  CUDADevice *const cuda_device = static_cast<CUDADevice *>(denoiser_device_);
  const CUDAContextScope scope(cuda_device);

  if (!NVSDK_NGX_CUDA.init()) {
    set_error("Failed to load NGX driver");
    return;
  }

#  ifdef _WIN32
  const wstring app_path = string_to_wstring(path_get());
  const wstring user_path = string_to_wstring(path_user_get());
#  else
  string app_path_narrow = path_get();
  string user_path_narrow = path_user_get();
  std::wstring app_path(app_path_narrow.size(), L' ');
  std::wstring user_path(user_path_narrow.size(), L' ');
  app_path.resize(std::mbstowcs(app_path.data(), app_path_narrow.c_str(), app_path_narrow.size()));
  user_path.resize(
      std::mbstowcs(user_path.data(), user_path_narrow.c_str(), user_path_narrow.size()));
#  endif

  const wchar_t *const app_paths[] = {app_path.c_str()};

  NVSDK_NGX_FeatureCommonInfo feature_info = {};
  feature_info.PathListInfo.Path = app_paths;
  feature_info.PathListInfo.Length = 1;
  feature_info.LoggingInfo.LoggingCallback =
      [](const char *message, NVSDK_NGX_Logging_Level loggingLevel, NVSDK_NGX_Feature) {
        switch (loggingLevel) {
          case NVSDK_NGX_LOGGING_LEVEL_OFF:
          case NVSDK_NGX_LOGGING_LEVEL_NUM:
            assert(false);
            break;
          case NVSDK_NGX_LOGGING_LEVEL_ON:
            LOG_INFO << message;
            break;
          case NVSDK_NGX_LOGGING_LEVEL_VERBOSE:
            LOG_INFO << message;
            break;
        }
      };
  feature_info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;

  ngx_device_ = new NVSDK_NGX_CUDADevice{
      cuda_device->cuContext, static_cast<CUDADeviceQueue *>(denoiser_queue_.get())->stream()};

  const NVSDK_NGX_Result result = NVSDK_NGX_CUDA.Init_Ext1(
      ApplicationId, user_path.c_str(), ngx_device_, NVSDK_NGX_Version_API, &feature_info);

  if (NVSDK_NGX_FAILED(result)) {
    set_error("Failed to initialize NGX driver");
  }
}

DLSSDenoiser::~DLSSDenoiser()
{
  CUDADevice *const cuda_device = static_cast<CUDADevice *>(denoiser_device_);
  const CUDAContextScope scope(cuda_device);

  tex_color_.destroy();
  tex_color_before_transparency_.destroy();
  tex_color_before_particles_.destroy();
  tex_color_before_fog_.destroy();
  tex_depth_.destroy();
  tex_diffuse_albedo_.destroy();
  tex_specular_albedo_.destroy();
  tex_normal_roughness_.destroy();
  tex_motion_.destroy();
  tex_specular_motion_.destroy();
  tex_specular_hit_distance_.destroy();
  tex_emissive_.destroy();
  tex_output_.destroy();
  if (tex_exposure_.array) {
    tex_exposure_.destroy();
  }

  if (!NVSDK_NGX_CUDA) {
    return;
  }

  if (handle_ != nullptr) {
    NVSDK_NGX_CUDA.ReleaseFeature(handle_);
  }

  unsigned int n = 0;
  const NVSDK_NGX_Result result = NVSDK_NGX_CUDA.Shutdown1(ngx_device_, n);

  if (NVSDK_NGX_FAILED(result)) {
    set_error("Failed to shutdown NGX driver");
  }

  delete ngx_device_;
}

bool DLSSDenoiser::is_device_supported(const DeviceInfo &device)
{
  if (device.type != DEVICE_CUDA && device.type != DEVICE_OPTIX) {
    return false;
  }

  /* 'NVSDK_NGX_CUDA_GetFeatureRequirements' is an expensive call, so cache the result (since
   * 'is_device_supported' is called a lot). */
  static NVSDK_NGX_Feature_Support_Result supported_cache[8] = {
      NVSDK_NGX_FeatureSupportResult_CheckNotPresent,
      NVSDK_NGX_FeatureSupportResult_CheckNotPresent,
      NVSDK_NGX_FeatureSupportResult_CheckNotPresent,
      NVSDK_NGX_FeatureSupportResult_CheckNotPresent,
      NVSDK_NGX_FeatureSupportResult_CheckNotPresent,
      NVSDK_NGX_FeatureSupportResult_CheckNotPresent,
      NVSDK_NGX_FeatureSupportResult_CheckNotPresent,
      NVSDK_NGX_FeatureSupportResult_CheckNotPresent};

  if (device.num >= 8) {
    return false;
  }
  if (supported_cache[device.num] != NVSDK_NGX_FeatureSupportResult_CheckNotPresent) {
    return supported_cache[device.num] == NVSDK_NGX_FeatureSupportResult_Supported;
  }

  if (!NVSDK_NGX_CUDA.init() || NVSDK_NGX_CUDA.GetFeatureRequirements == nullptr) {
    /* NVIDIA driver is too old (requires 590+). */
    return false;
  }

#  ifdef _WIN32
  const wstring user_path = string_to_wstring(path_user_get());
#  else
  string user_path_narrow = path_user_get();
  std::wstring user_path(user_path_narrow.size(), L' ');
  user_path.resize(
      std::mbstowcs(user_path.data(), user_path_narrow.c_str(), user_path_narrow.size()));
#  endif

  CUdevice cuDevice = 0;
  cuDeviceGet(&cuDevice, device.num);

  NVSDK_NGX_FeatureDiscoveryInfo discovery_info = {};
  discovery_info.SDKVersion = NVSDK_NGX_Version_API;
  discovery_info.FeatureID = NVSDK_NGX_Feature_RayReconstruction;
  discovery_info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Application_Id;
  discovery_info.Identifier.v.ApplicationId = ApplicationId;
  discovery_info.ApplicationDataPath = user_path.c_str();

  NVSDK_NGX_FeatureRequirement requirement = {NVSDK_NGX_FeatureSupportResult_Supported};

  const NVSDK_NGX_Result result = NVSDK_NGX_CUDA.GetFeatureRequirements(
      cuDevice, &discovery_info, &requirement);

  if (NVSDK_NGX_SUCCEED(result)) {
    supported_cache[device.num] = requirement.FeatureSupported;
    return requirement.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported;
  }
  else {
    return false;
  }
}

bool DLSSDenoiser::denoise_create_if_needed(DenoiseContext &context)
{
  const bool recreate_denoiser = last_width_ != context.denoised_buffer_params.width ||
                                 last_height_ != context.denoised_buffer_params.height ||
                                 last_in_width_ != context.buffer_params.width ||
                                 last_in_height_ != context.buffer_params.height ||
                                 last_upscale_factor_ != context.denoise_params.upscale_factor;
  if (handle_ != nullptr && !recreate_denoiser) {
    return true;
  }

  CUDADevice *const cuda_device = static_cast<CUDADevice *>(denoiser_device_);
  const CUDAContextScope scope(cuda_device);

  if (handle_ != nullptr) {
    denoiser_queue_->synchronize();

    NVSDK_NGX_CUDA.ReleaseFeature(handle_);
    handle_ = nullptr;
  }

  /* New feature instance = no temporal history to carry or align. */
  have_history_ = false;
  last_num_samples_ = 0;
  last_frame_ = INT_MIN;
  preroll_history_poisoned_ = false;

  tex_color_.destroy();
  tex_color_before_transparency_.destroy();
  tex_color_before_particles_.destroy();
  tex_color_before_fog_.destroy();
  tex_depth_.destroy();
  tex_diffuse_albedo_.destroy();
  tex_specular_albedo_.destroy();
  tex_normal_roughness_.destroy();
  tex_motion_.destroy();
  tex_specular_motion_.destroy();
  tex_specular_hit_distance_.destroy();
  tex_emissive_.destroy();
  tex_output_.destroy();
  if (tex_exposure_.array) {
    tex_exposure_.destroy();
  }

  if (context.buffer_params.width <= 128 || context.buffer_params.height <= 96) {
    if (getenv("FALCON_DLSS_DEBUG")) {
      fprintf(stderr,
              "[dlss] skip create: buffer %dx%d too small (<=128x96)\n",
              context.buffer_params.width,
              context.buffer_params.height);
    }
    last_width_ = 0;
    last_height_ = 0;
    last_in_width_ = 0;
    last_in_height_ = 0;
    return false;
  }

  NVSDK_NGX_Parameter *params = nullptr;
  if (NVSDK_NGX_FAILED(NVSDK_NGX_CUDA.AllocateParameters(&params))) {
    return false;
  }

  params->Set(NVSDK_NGX_Parameter_Width, context.buffer_params.width);
  params->Set(NVSDK_NGX_Parameter_Height, context.buffer_params.height);
  params->Set(NVSDK_NGX_Parameter_OutWidth, context.denoised_buffer_params.width);
  params->Set(NVSDK_NGX_Parameter_OutHeight, context.denoised_buffer_params.height);

  params->Set(NVSDK_NGX_Parameter_DLSS_Denoise_Mode, NVSDK_NGX_DLSS_Denoise_Mode_DLUnified);
  params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
              NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes);
  params->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
  /* The quality value has to describe the ratio this feature is actually being
   * created with, not the mode the panel asks for: the runtime derives the
   * allowed dynamic-resolution window from it and refuses to evaluate outside
   * that window. Rendering playback frames smaller (see
   * RenderScheduler::playback_upscale_factor) hit exactly that -- created as
   * MaxQuality, evaluated at 3.0x: "RenderSubrect (254x142) outside of Min
   * (381x214) and Max (762x428) dynamic res", every evaluation failed and the
   * viewport showed the raw path-traced buffer. */
  const float create_upscale = (context.buffer_params.width > 0) ?
                                   float(context.denoised_buffer_params.width) /
                                       float(context.buffer_params.width) :
                                   context.denoise_params.upscale_factor;
  params->Set(NVSDK_NGX_Parameter_PerfQualityValue,
              create_upscale <= 1.001f       ? NVSDK_NGX_PerfQuality_Value_DLAA :
              create_upscale <= 1.0f / 0.65f ? NVSDK_NGX_PerfQuality_Value_MaxQuality :
              create_upscale <= 1.0f / 0.57f ? NVSDK_NGX_PerfQuality_Value_Balanced :
              create_upscale <= 1.0f / 0.5f  ? NVSDK_NGX_PerfQuality_Value_MaxPerf :
                                               NVSDK_NGX_PerfQuality_Value_UltraPerformance);
  params->Set(NVSDK_NGX_Parameter_Use_HW_Depth, NVSDK_NGX_DLSS_Depth_Type_Linear);
  params->Set(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, NVSDK_NGX_DLSS_Roughness_Mode_Packed);

  /* The runtime maps the DLAA request to its UltraQuality mode (see the
   * "(UltraQuality) Using Default Overriden Preset" log line), so without the
   * UltraQuality hint the equal-size path silently fell back to Preset D.
   *
   * FALCON_DLSS_PRESET_D restores that older preset for A/B: E is the newer,
   * sharper model, but sharper also means it keeps more high-frequency
   * residue, which can read as noise. */
  const NVSDK_NGX_RayReconstruction_Hint_Render_Preset preset =
      getenv("FALCON_DLSS_PRESET_D") ? NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D :
                                       NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E;
  params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality, preset);
  params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, preset);
  params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality, preset);
  params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced, preset);
  params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance, preset);
  params->Set(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, preset);

  const NVSDK_NGX_Result result = NVSDK_NGX_CUDA.CreateFeature1(
      ngx_device_, NVSDK_NGX_Feature_RayReconstruction, params, &handle_);

  NVSDK_NGX_CUDA.DestroyParameters(params);

  if (NVSDK_NGX_FAILED(result)) {
    if (getenv("FALCON_DLSS_DEBUG")) {
      fprintf(stderr,
              "[dlss] CreateFeature1 failed: result=%d size=%dx%d out=%dx%d\n",
              int(result),
              context.buffer_params.width,
              context.buffer_params.height,
              context.denoised_buffer_params.width,
              context.denoised_buffer_params.height);
    }
    set_error("Failed to create DLSS instance");
    return false;
  }

  if (getenv("FALCON_DLSS_DEBUG")) {
    fprintf(stderr,
            "[dlss] recreate: in=%dx%d out=%dx%d upscale=%.3f\n",
            context.buffer_params.width,
            context.buffer_params.height,
            context.denoised_buffer_params.width,
            context.denoised_buffer_params.height,
            context.denoise_params.upscale_factor);
  }

  tex_color_.init(cuda_device, context.buffer_params.width, context.buffer_params.height, 4);
  tex_color_before_transparency_.init(
      cuda_device, context.buffer_params.width, context.buffer_params.height, 4);
  if (use_layer_guides()) {
    tex_color_before_particles_.init(
        cuda_device, context.buffer_params.width, context.buffer_params.height, 4);
    tex_color_before_fog_.init(
        cuda_device, context.buffer_params.width, context.buffer_params.height, 4);
  }
  tex_depth_.init(cuda_device, context.buffer_params.width, context.buffer_params.height, 1);
  tex_diffuse_albedo_.init(
      cuda_device, context.buffer_params.width, context.buffer_params.height, 4);
  tex_specular_albedo_.init(
      cuda_device, context.buffer_params.width, context.buffer_params.height, 4);
  tex_normal_roughness_.init(
      cuda_device, context.buffer_params.width, context.buffer_params.height, 4);
  tex_motion_.init(cuda_device, context.buffer_params.width, context.buffer_params.height, 2);
  tex_specular_motion_.init(
      cuda_device, context.buffer_params.width, context.buffer_params.height, 2);
  tex_specular_hit_distance_.init(
      cuda_device, context.buffer_params.width, context.buffer_params.height, 1);
  tex_emissive_.init(cuda_device, context.buffer_params.width, context.buffer_params.height, 4);

  tex_output_.init(
      cuda_device, context.denoised_buffer_params.width, context.denoised_buffer_params.height, 4);

  /* Exposure sweep knob: a 1x1 texture holding the exposure scale RR should
   * assume for the HDR input (the SDK's ExposureTexture). Unset = not passed. */
  if (const char *exposure_tex = getenv("FALCON_DLSS_EXPOSURE_TEX")) {
    const float value = atof(exposure_tex);
    tex_exposure_.init(cuda_device, 1, 1, 1);
    cuda_device_assert(cuda_device, cuMemcpyHtoA((CUarray)tex_exposure_.array, 0, &value, sizeof(float)));
  }

  last_width_ = context.denoised_buffer_params.width;
  last_height_ = context.denoised_buffer_params.height;
  last_in_width_ = context.buffer_params.width;
  last_in_height_ = context.buffer_params.height;
  last_upscale_factor_ = context.denoise_params.upscale_factor;

  return !cuda_device->have_error();
}

bool DLSSDenoiser::denoise_configure_if_needed(DenoiseContext & /*context*/)
{
  return true;
}

bool DLSSDenoiser::denoise_filter_color_preprocess(const DenoiseContext &context,
                                                   const DenoisePass &pass)
{
  if (pass.type != PASS_COMBINED) {
    return false;
  }

  // Input params (with resolution divider applied)
  const BufferParams &buffer_params = context.buffer_params;

  const int work_size = buffer_params.width * buffer_params.height;

  const int pass_transmission_direct = buffer_params.get_pass_offset(PASS_TRANSMISSION_DIRECT);
  const int pass_transmission_indirect = buffer_params.get_pass_offset(
      PASS_TRANSMISSION_INDIRECT);

  /* FALCON_DLSS_DEMOD=1: feed RR the colour divided by (diffuse+specular)
   * albedo and multiply it back in the postprocess. Sweep knob, default off. */
  const int pass_specular_albedo = buffer_params.get_pass_offset(PASS_DENOISING_SPECULAR_ALBEDO);
  const int demodulate = dlss_demodulate() ? 1 : 0;
  const float color_scale = dlss_color_scale();

  /* Layer guides (FALCON_DLSS_LAYER_GUIDES): surfaces are 0 when off, the
   * kernel skips the writes. */
  const int layer_guides = use_layer_guides() ? 1 : 0;
  const int pass_volume_direct = layer_guides ? buffer_params.get_pass_offset(PASS_VOLUME_DIRECT) :
                                                PASS_UNUSED;
  const int pass_volume_indirect = layer_guides ?
                                       buffer_params.get_pass_offset(PASS_VOLUME_INDIRECT) :
                                       PASS_UNUSED;

  const DeviceKernelArguments args(&tex_color_.surface_handle,
                                   &tex_color_before_transparency_.surface_handle,
                                   &tex_color_before_particles_.surface_handle,
                                   &tex_color_before_fog_.surface_handle,
                                   &layer_guides,
                                   &pass_volume_direct,
                                   &pass_volume_indirect,
                                   &context.render_buffers->buffer.device_pointer,
                                   &buffer_params.full_x,
                                   &buffer_params.full_y,
                                   &buffer_params.width,
                                   &buffer_params.height,
                                   &buffer_params.offset,
                                   &buffer_params.stride,
                                   &buffer_params.pass_stride,
                                   &pass.denoised_offset,
                                   &context.pass_sample_count,
                                   &pass_transmission_direct,
                                   &pass_transmission_indirect,
                                   &context.num_samples,
                                   &context.pass_denoising_albedo,
                                   &pass_specular_albedo,
                                   &demodulate,
                                   &color_scale);

  return denoiser_queue_->enqueue(
      DEVICE_KERNEL_FILTER_COLOR_PREPROCESS_TO_SURFACE, work_size, args);
}
bool DLSSDenoiser::denoise_filter_color_postprocess(const DenoiseContext &context,
                                                    const DenoisePass &pass)
{
  if (pass.type != PASS_COMBINED) {
    return false;
  }

  // Output params
  const BufferParams &buffer_params = context.denoised_buffer_params;

  const int work_size = buffer_params.width * buffer_params.height;

  const int pass_specular_albedo = context.buffer_params.get_pass_offset(
      PASS_DENOISING_SPECULAR_ALBEDO);
  const int demodulate = dlss_demodulate() ? 1 : 0;
  const float inv_color_scale = 1.0f / dlss_color_scale();

  const DeviceKernelArguments args(&tex_output_.surface_handle,
                                   &context.render_buffers->buffer.device_pointer,
                                   &buffer_params.full_x,
                                   &buffer_params.full_y,
                                   &buffer_params.width,
                                   &buffer_params.height,
                                   &buffer_params.offset,
                                   &buffer_params.stride,
                                   &context.buffer_params.full_x,
                                   &context.buffer_params.full_y,
                                   &context.buffer_params.offset,
                                   &context.buffer_params.stride,
                                   &buffer_params.pass_stride,
                                   &context.num_samples,
                                   &pass.noisy_offset,
                                   &pass.denoised_offset,
                                   &context.pass_sample_count,
                                   &pass.num_components,
                                   &pass.use_compositing,
                                   &params_.upscale_factor,
                                   &context.pass_denoising_albedo,
                                   &pass_specular_albedo,
                                   &demodulate,
                                   &inv_color_scale);

  return denoiser_queue_->enqueue(
      DEVICE_KERNEL_FILTER_COLOR_POSTPROCESS_FROM_SURFACE, work_size, args);
}

bool DLSSDenoiser::denoise_filter_guiding_preprocess(DenoiseContext &context)
{
  const BufferParams &buffer_params = context.buffer_params;

  const int work_size = buffer_params.width * buffer_params.height;

  const int pass_depth = context.buffer_params.get_pass_offset(PASS_DENOISING_DEPTH);
  const int pass_specular_albedo = context.buffer_params.get_pass_offset(
      PASS_DENOISING_SPECULAR_ALBEDO);
  const int pass_roughness = context.buffer_params.get_pass_offset(PASS_DENOISING_ROUGHNESS);
  const int pass_specular_motion = context.buffer_params.get_pass_offset(
      PASS_DENOISING_SPECULAR_MOTION);

  /* The motion pass holds inter-frame motion. It only aligns the history on the
   * first denoise after a frame transition (sample count dropped) when carrying
   * history across frames; every re-denoise of the same frame must use zero
   * motion or the history gets re-warped by the same vector each round and
   * smears along the motion direction. (last_num_samples_ is updated later, in
   * denoise_run.) */
  const bool frame_transition = is_frame_transition(context) && !same_frame_restart_;
  const int zero_motion = (use_carry_history(context) && have_history_ && frame_transition) ? 0 :
                                                                                              1;

  const int pass_specular_hit_distance = context.buffer_params.get_pass_offset(
      PASS_DENOISING_SPECULAR_HIT_DISTANCE);

  const int pass_emission = use_emissive_guide(context) ?
                                context.buffer_params.get_pass_offset(PASS_EMISSION) :
                                PASS_UNUSED;

  /* Cycles stores the motion pass as a sum over the samples that found something
   * to move, with the count in its own weight pass; the guide has to be divided
   * by that count and not by the sample count. FALCON_DLSS_LEGACY_GUIDES=1 puts
   * back the old normalization (and the unnormalized normal) for A/B. */
  /* Far value for the depth guide: the background overwrites the depth pass with FLT_MAX, which
   * the preprocess then divides by the pixel's sample count. Clamp it to a finite far distance
   * (the same 1e4 the specular hit distance already uses for "escaped to the environment") so the
   * sky reads as one fixed distance instead of a per-frame 1e37. FALCON_DLSS_DEPTH_FAR sets it;
   * 0 keeps the raw sentinel. */
  float depth_far = 1e4f;
  if (const char *v = getenv("FALCON_DLSS_DEPTH_FAR")) {
    depth_far = (float)atof(v);
  }

  const bool legacy_guides = getenv("FALCON_DLSS_LEGACY_GUIDES") != nullptr;
  const int pass_motion_weight = legacy_guides ?
                                     PASS_UNUSED :
                                     context.buffer_params.get_pass_offset(PASS_MOTION_WEIGHT);
  const int normalize_normal = legacy_guides ? 0 : 1;

  const DeviceKernelArguments args(&tex_depth_.surface_handle,
                                   &tex_diffuse_albedo_.surface_handle,
                                   &tex_specular_albedo_.surface_handle,
                                   &tex_normal_roughness_.surface_handle,
                                   &tex_motion_.surface_handle,
                                   &tex_specular_motion_.surface_handle,
                                   &tex_specular_hit_distance_.surface_handle,
                                   &pass_specular_hit_distance,
                                   &tex_emissive_.surface_handle,
                                   &pass_emission,
                                   &context.render_buffers->buffer.device_pointer,
                                   &buffer_params.offset,
                                   &buffer_params.stride,
                                   &buffer_params.pass_stride,
                                   &context.pass_sample_count,
                                   &pass_depth,
                                   &context.pass_denoising_albedo,
                                   &pass_specular_albedo,
                                   &context.pass_denoising_normal,
                                   &pass_roughness,
                                   &context.pass_motion,
                                   &pass_motion_weight,
                                   &pass_specular_motion,
                                   &buffer_params.full_x,
                                   &buffer_params.full_y,
                                   &buffer_params.width,
                                   &buffer_params.height,
                                   &context.num_samples,
                                   &zero_motion,
                                   &normalize_normal,
                                   &depth_far);

  return denoiser_queue_->enqueue(
      DEVICE_KERNEL_FILTER_GUIDING_PREPROCESS_TO_SURFACE, work_size, args);
}

bool DLSSDenoiser::transfer_guides(const DenoiseContext &context, const bool dump, const char *dir)
{
  CUDADevice *const cuda_device = static_cast<CUDADevice *>(denoiser_device_);
  const CUDAContextScope scope(cuda_device);

  /* The guiding preprocess that filled the arrays runs on the denoiser queue. */
  denoiser_queue_->synchronize();

  const int width = context.buffer_params.width;
  const int height = context.buffer_params.height;

  const struct {
    const char *name;
    CUDATexture *tex;
    int channels;
  } entries[] = {
      {"depth", &tex_depth_, 1},
      {"diffuse_albedo", &tex_diffuse_albedo_, 4},
      {"specular_albedo", &tex_specular_albedo_, 4},
      {"normal_roughness", &tex_normal_roughness_, 4},
      {"motion", &tex_motion_, 2},
  };

  for (const auto &entry : entries) {
    const size_t row_bytes = size_t(width) * entry.channels * sizeof(float);
    const size_t total_bytes = row_bytes * height;
    vector<uint8_t> host(total_bytes);
    const string filepath = string(dir) + "/" + entry.name + ".raw";

    if (!dump) {
      FILE *file = fopen(filepath.c_str(), "rb");
      if (file == nullptr) {
        LOG_ERROR << "DLSS guide load: cannot open " << filepath;
        return false;
      }
      fseek(file, 0, SEEK_END);
      const size_t file_bytes = size_t(ftell(file));
      fseek(file, 0, SEEK_SET);
      const size_t read_bytes = (file_bytes == total_bytes) ?
                                    fread(host.data(), 1, total_bytes, file) :
                                    0;
      fclose(file);
      if (read_bytes != total_bytes) {
        LOG_ERROR << "DLSS guide load: " << filepath << " holds " << file_bytes << " bytes, need "
                  << total_bytes << " (" << width << "x" << height << "x" << entry.channels
                  << " floats)";
        return false;
      }
    }

    CUDA_MEMCPY2D copy = {};
    copy.WidthInBytes = row_bytes;
    copy.Height = height;
    if (dump) {
      copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
      copy.srcArray = static_cast<CUarray>(entry.tex->array);
      copy.dstMemoryType = CU_MEMORYTYPE_HOST;
      copy.dstHost = host.data();
      copy.dstPitch = row_bytes;
    }
    else {
      copy.srcMemoryType = CU_MEMORYTYPE_HOST;
      copy.srcHost = host.data();
      copy.srcPitch = row_bytes;
      copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
      copy.dstArray = static_cast<CUarray>(entry.tex->array);
    }
    cuda_device_assert(cuda_device, cuMemcpy2D(&copy));

    if (dump) {
      FILE *file = fopen(filepath.c_str(), "wb");
      if (file == nullptr || fwrite(host.data(), 1, total_bytes, file) != total_bytes) {
        LOG_ERROR << "DLSS guide dump: cannot write " << filepath;
        if (file) {
          fclose(file);
        }
        return false;
      }
      fclose(file);
    }
  }

  LOG_INFO << "DLSS guides " << (dump ? "dumped to " : "loaded from ") << dir << " (" << width
           << "x" << height << ")";
  return true;
}

bool DLSSDenoiser::load_particles_guide(const DenoiseContext &context, const char *pattern)
{
  CUDADevice *const cuda_device = static_cast<CUDADevice *>(denoiser_device_);
  const CUDAContextScope scope(cuda_device);
  denoiser_queue_->synchronize();

  string filepath = pattern;
  const size_t hash = filepath.find("####");
  if (hash != string::npos) {
    filepath = filepath.substr(0, hash) + string_printf("%04d", frame_) + filepath.substr(hash + 4);
  }

  const int width = context.buffer_params.width;
  const int height = context.buffer_params.height;
  const size_t row_bytes = size_t(width) * 4 * sizeof(float);
  const size_t total_bytes = row_bytes * height;
  vector<uint8_t> host(total_bytes);

  FILE *file = fopen(filepath.c_str(), "rb");
  if (file == nullptr) {
    LOG_ERROR << "DLSS particles guide: cannot open " << filepath;
    return false;
  }
  const size_t read_bytes = fread(host.data(), 1, total_bytes, file);
  fclose(file);
  if (read_bytes != total_bytes) {
    LOG_ERROR << "DLSS particles guide: " << filepath << " short read " << read_bytes << " need "
              << total_bytes;
    return false;
  }

  CUDA_MEMCPY2D copy = {};
  copy.WidthInBytes = row_bytes;
  copy.Height = height;
  copy.srcMemoryType = CU_MEMORYTYPE_HOST;
  copy.srcHost = host.data();
  copy.srcPitch = row_bytes;
  copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
  copy.dstArray = static_cast<CUarray>(tex_color_before_particles_.array);
  cuda_device_assert(cuda_device, cuMemcpy2D(&copy));
  if (getenv("FALCON_DLSS_DEBUG")) {
    fprintf(stderr, "[dlss] particles guide loaded %s\n", filepath.c_str());
  }
  return true;
}

bool DLSSDenoiser::denoise_run(const DenoiseContext &context, const DenoisePass &pass)
{
  if (pass.type != PASS_COMBINED) {
    return false;
  }

  NVSDK_NGX_Parameter *params = nullptr;
  if (NVSDK_NGX_FAILED(NVSDK_NGX_CUDA.AllocateParameters(&params))) {
    return false;
  }

  CUDADevice *const cuda_device = static_cast<CUDADevice *>(denoiser_device_);
  const CUDAContextScope scope(cuda_device);

  /* Reset the temporal history when the accumulation restarted (num_samples dropped back down),
   * which happens on viewport navigation, scene edits and timeline scrubs. Without this the stale
   * history is reprojected onto the new frame and ghosts. During steady accumulation num_samples
   * keeps growing, so history is preserved and the image converges. */
  const bool frame_transition = is_frame_transition(context);
  /* Carry mode keeps the history across the transition; the guiding preprocess
   * fed the real inter-frame motion vectors for this evaluation so the history
   * is warped into alignment instead of being dropped. */
  bool is_reset = frame_transition && !(use_carry_history(context) && have_history_);

  /* Pre-roll used to hand RR the same frame N times over with Reset=0, so the
   * history it built was N evaluations deep on a picture that never moved. RR
   * reads that as a long, stable history and weights it accordingly, so the
   * *next* frame -- the first one with real motion -- came out ghosted far
   * worse than with no pre-roll at all (slowcam 32 spp, RMSE against 1024 spp:
   * f2 0.0262 with no pre-roll, 0.0935 with seven passes, worse than not
   * denoising at all, and it took some fifteen frames to recover). The damage
   * grew with the pass count, which is the signature of the history depth.
   *
   * So cap the depth: every pre-roll pass resets the history and only the last
   * one is carried into the kept frame. The kept frame still starts from a
   * warmed, one-deep history -- which is what the following frames of an
   * ordinary sequence get -- instead of an artificially deep one.
   * FALCON_DLSS_PREROLL_MODE=0 restores the old behaviour. */
  static const int preroll_mode = getenv("FALCON_DLSS_PREROLL_MODE") ?
                                      atoi(getenv("FALCON_DLSS_PREROLL_MODE")) :
                                      2;
  if (preroll_mode == 1 && preroll_pass_) {
    is_reset = true;
  }

  /* Mode 1 caps the depth at one, and measuring it showed the cap is not enough
   * and costs the pre-roll its whole point: seven passes then measure the same
   * as one (0.0229/0.0310/0.0281 against 0.0231/0.0303/0.0290), because only
   * the last pass is ever carried. A single carried same-frame pass already
   * costs the next frame 0.0262 -> 0.0303. So the depth is not the dial; the
   * carry itself is.
   *
   * Mode 2 keeps the passes chained -- that is what buys the clean first frame
   * (0.0211) -- and instead drops the history once, on the frame after the
   * pre-roll. That frame then starts cold, exactly as the first frame of a
   * sequence used to, and everything after it carries normally. The poisoned
   * history never reaches a frame that moves. */
  if (preroll_mode == 2) {
    if (preroll_pass_ || same_frame_restart_) {
      /* Still inside the pre-rolled frame. */
      preroll_history_poisoned_ = true;
    }
    else if (preroll_history_poisoned_ && frame_transition) {
      is_reset = true;
      preroll_history_poisoned_ = false;
    }
  }

  /* A cut (camera switch, timeline jump, scene edit) dropped the history; RR
   * has to be told, whatever the sample counts did. FALCON_DLSS_CUT_RESET=0
   * goes back to letting the clear pass silently. */
  static const bool cut_reset = getenv("FALCON_DLSS_CUT_RESET") ?
                                    atoi(getenv("FALCON_DLSS_CUT_RESET")) != 0 :
                                    true;
  if (pending_reset_) {
    if (cut_reset) {
      is_reset = true;
    }
    pending_reset_ = false;
  }

  last_num_samples_ = context.num_samples;
  last_frame_ = frame_;
  params->Set(NVSDK_NGX_Parameter_Reset, is_reset ? 1 : 0);

  /* RR places the samples at (its pixel centre + jitter) and reconstructs at
   * those centres. Cycles counts the pixel centre at the integer raster
   * coordinate, RR counts it half a pixel further in both axes, so the jitter
   * has to carry that half pixel or the whole reconstruction lands off by 0.5
   * -- which is what it did: measured +0.446/+0.496 px against a 2048-sample
   * reference, and the error sat on the edges and never fell with the sample
   * count. Adding the half pixel puts the shift at -0.031/+0.002 and halves the
   * error (classroom 32 spp: 0.0399 -> 0.0197). FALCON_DLSS_JITTER_MODE=0
   * restores the old value; the other modes are the ones the probe measured. */
  {
    static const int jitter_mode = getenv("FALCON_DLSS_JITTER_MODE") ?
                                       atoi(getenv("FALCON_DLSS_JITTER_MODE")) :
                                       2;
    float jx = context.pixel_jitter.x;
    float jy = context.pixel_jitter.y;
    switch (jitter_mode) {
      case 1: jx = -jx; jy = -jy; break;
      case 2: jx += 0.5f; jy += 0.5f; break;
      case 3: jx -= 0.5f; jy -= 0.5f; break;
      case 4: jx = -jx + 0.5f; jy = -jy + 0.5f; break;
      case 5: jx = -jx - 0.5f; jy = -jy - 0.5f; break;
      case 6: jx = 0.0f; jy = 0.0f; break;
      case 0:
      default: break;
    }
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jx);
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, jy);
  }


  /* The motion vectors are in pixels, so the scale is one. The SDK's own helper
   * always sets this (substituting 1.0 for a zero) and we never did; the driver
   * turns out to assume the same value, so setting it changes no pixel today
   * (measured on five scenes, differences below 0.3%). It is set anyway because
   * the contract says to: an OTA model update is free to stop assuming. */
  params->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
  params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);

  /* Exposure sweep knobs (2026-08-30, tonal compression investigation). The
   * SDK helper always sets Pre_Exposure / Exposure_Scale (1.0 when zero); we
   * never did. Unset env vars keep the parameters untouched, which is the
   * behaviour every previous measurement was taken with. */
  if (const char *v = getenv("FALCON_DLSS_EXPOSURE_SCALE")) {
    params->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, (float)atof(v));
  }
  if (const char *v = getenv("FALCON_DLSS_PRE_EXPOSURE")) {
    params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, (float)atof(v));
  }
  if (tex_exposure_.array) {
    params->Set(NVSDK_NGX_Parameter_ExposureTexture, &tex_exposure_.texture_handle);
  }

  /* Contract parameters the SDK helper always sets and we never did (2026-08-31).
   *
   * FrameTimeDeltaInMsec: RR weights how much history to keep by how long ago it was drawn.
   * Left unset it is whatever the parameter block last held. Offline there is no wall clock that
   * means anything -- what matters is the interval between the frames being denoised, which is
   * 1000/fps. FALCON_DLSS_FRAME_TIME_MS overrides it.
   *
   * Subrect bases: we hand RR whole textures, so every base is 0. The helper sets all of them on
   * every evaluate; leaving them unset lets a value survive from an earlier feature. Setting them
   * is free and removes that path.
   *
   * FALCON_DLSS_CONTRACT_PARAMS=0 leaves both unset (the behaviour every measurement before
   * 2026-08-31 was taken with). */
  if (getenv("FALCON_DLSS_CONTRACT_PARAMS") == nullptr ||
      atoi(getenv("FALCON_DLSS_CONTRACT_PARAMS")) != 0)
  {
    float frame_time_ms = 1000.0f / 24.0f;
    if (const char *v = getenv("FALCON_DLSS_FRAME_TIME_MS")) {
      frame_time_ms = (float)atof(v);
    }
    params->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, frame_time_ms);

    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_X, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_Y, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_X, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_Y, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_X, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_Y, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_X, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_Y, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0u);
  }

  params->Set(NVSDK_NGX_Parameter_Color, &tex_color_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_Depth, &tex_depth_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_DiffuseAlbedo, &tex_diffuse_albedo_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_SpecularAlbedo, &tex_specular_albedo_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_GBuffer_Normals, &tex_normal_roughness_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_GBuffer_Roughness, &tex_normal_roughness_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_MotionVectors, &tex_motion_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_Output, &tex_output_.surface_handle);

  /* Specular motion tells RR how the reflected image moves, which is not the
   * motion of the surface carrying it. Cycles has no such pass: PASS_DENOISING_
   * SPECULAR_MOTION is declared but nothing ever writes it, so the buffer was
   * all zeros -- claiming reflections stand still while the camera moves, which
   * is what melted the highlights on the chrome chair legs. Until the pass is
   * real, leave the parameter unset so RR falls back to its own estimate rather
   * than trusting a lie. */
  if (use_specular_motion(context)) {
    params->Set(NVSDK_NGX_Parameter_GBuffer_SpecularMvec, &tex_specular_motion_.texture_handle);
  }

  /* Specular hit distance (RR Integration Guide 3.4.9): how far behind the
   * surface the reflected image sits. RR builds the specular motion vector from
   * it -- NVIDIA's own path tracer sample feeds exactly this and no specular
   * motion. It is a world-space length, so it only means anything alongside the
   * camera matrices. */
  if (use_specular_hit_distance(context)) {
    params->Set(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance,
                &tex_specular_hit_distance_.texture_handle);
    params->Set(NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX, world_to_view_);
    params->Set(NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX, view_to_clip_);
  }


  /* GBuffer_Emissive: the surface's own emission. Off unless FALCON_DLSS_EMISSIVE_GUIDE=1
   * put PASS_EMISSION in the buffer. */
  if (use_emissive_guide(context)) {
    params->Set(NVSDK_NGX_Parameter_GBuffer_Emissive, &tex_emissive_.texture_handle);
  }

  /* ColorBeforeTransparency guide (RR Integration Guide 3.4.11): the noisy
   * color with the transmission contribution removed, so RR can tell which
   * part of the pixel is seen through glass instead of blurring it as if it
   * were surface detail. Only provided when the transmission passes exist. */
  if (use_transparency_guide(context)) {
    params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency,
                &tex_color_before_transparency_.texture_handle);
  }
  /* FALCON_DLSS_LAYER_GUIDES=1: the other two ColorBefore* layers of the
   * contract (particles -> transparency -> fog). Fog alone broke the image
   * (8-28); this hands all three at once. */
  if (use_layer_guides()) {
    params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles,
                &tex_color_before_particles_.texture_handle);
    params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeFog, &tex_color_before_fog_.texture_handle);
    if (!use_transparency_guide(context)) {
      params->Set(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency,
                  &tex_color_before_transparency_.texture_handle);
    }
  }

  params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
              context.buffer_params.width);
  params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
              context.buffer_params.height);

  params->Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis, 1);

  /* Debug guide override (see transfer_guides): swap the freshly preprocessed
   * guides for a dump from another render just before RR consumes them. */
  if (const char *dump_dir = getenv("FALCON_DLSS_GUIDE_DUMP")) {
    transfer_guides(context, true, dump_dir);
  }
  else if (const char *load_dir = getenv("FALCON_DLSS_GUIDE_LOAD")) {
    if (!transfer_guides(context, false, load_dir)) {
      NVSDK_NGX_CUDA.DestroyParameters(params);
      return false;
    }
  }
  /* FALCON_DLSS_PARTICLES_GUIDE_RAW=path: replace ColorBeforeParticles with a
   * float4 image baked without the particles ("####" = frame number, raw
   * width*height*4 floats in render-buffer row order). Needs LAYER_GUIDES=1. */
  if (const char *raw = getenv("FALCON_DLSS_PARTICLES_GUIDE_RAW")) {
    if (use_layer_guides() && !load_particles_guide(context, raw)) {
      NVSDK_NGX_CUDA.DestroyParameters(params);
      return false;
    }
  }

  const NVSDK_NGX_Result result = NVSDK_NGX_CUDA.EvaluateFeature(handle_, params, nullptr);

  if (getenv("FALCON_DLSS_DEBUG")) {
    fprintf(stderr,
            "[dlss] evaluate %dx%d samples=%d reset=%d carry=%d result=%d\n",
            context.buffer_params.width,
            context.buffer_params.height,
            context.num_samples,
            is_reset ? 1 : 0,
            use_carry_history(context) ? 1 : 0,
            int(result));
  }

  NVSDK_NGX_CUDA.DestroyParameters(params);

  if (NVSDK_NGX_SUCCEED(result)) {
    have_history_ = true;
  }
  return NVSDK_NGX_SUCCEED(result);
}

CCL_NAMESPACE_END

#endif
