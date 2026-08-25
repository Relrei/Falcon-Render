/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "integrator/path_trace.h"

#include "kernel/integrator/falcon_sharc_size.h"

#include "device/cpu/device.h"
#include "device/device.h"

#include "integrator/pass_accessor.h"
#include "integrator/path_trace_display.h"
#include "integrator/path_trace_tile.h"
#include "integrator/render_scheduler.h"

#include "scene/integrator.h"
#include "scene/pass.h"
#include "scene/scene.h"

#include "session/display_driver.h"
#include "session/tile.h"

#include "util/log.h"
#include "util/progress.h"
#include "util/scoped_defer.h"
#include "util/tbb.h"
#include "util/time.h"

#ifdef WITH_FALCON_SHARC
#  include "util/hash.h"
#  include <cmath>
#  include <cstdint>
#  include <cstdio>
#  include <cstdlib>
#  include <cstring>
#endif

CCL_NAMESPACE_BEGIN

#ifdef WITH_FALCON_SHARC
/* Host-side replica of the SHARC world-space cell hash. Must stay in sync with
 * falcon_sharc_hash() / falcon_sharc_cell_size() in
 * kernel/integrator/falcon_sharc.h.
 *
 * The cell size is a RUNTIME value and has to be passed in. It used to be a
 * hard-coded 0.2 here while the kernel looked cells up at the scene's cell size
 * (0.05 by default), so warmup deposited into one grid and blend read another:
 * the lookups missed and the cache did nothing. Measured 2026-07-30 on
 * classroom -- taking the cache in full (alpha=1) moved the image by 1%, and
 * the whole 4M table held 2408 cells because the deposits were keyed at 0.2. */
static const float FALCON_SHARC_HOST_CELL_SIZE_FALLBACK = 0.2f;
static const unsigned int FALCON_SHARC_HOST_CELL_COUNT = FALCON_SHARC_CELL_COUNT;
static const unsigned int FALCON_SHARC_HOST_CELL_MASK = FALCON_SHARC_HOST_CELL_COUNT - 1u;
static const int FALCON_SHARC_HOST_CELL_STRIDE = 4;

static inline unsigned int falcon_sharc_host_hash(float px,
                                                  float py,
                                                  float pz,
                                                  const float cell_size)
{
  const float inv = 1.0f / cell_size;
  const unsigned int gx = (unsigned int)((int)floorf(px * inv));
  const unsigned int gy = (unsigned int)((int)floorf(py * inv));
  const unsigned int gz = (unsigned int)((int)floorf(pz * inv));
  return hash_uint3(gx, gy, gz) & FALCON_SHARC_HOST_CELL_MASK;
}
#endif

PathTrace::PathTrace(Device *device,
                     Device *denoise_device,
                     Film *film,
                     DeviceScene *device_scene,
                     RenderScheduler &render_scheduler,
                     TileManager &tile_manager)
    : device_(device),
      denoise_device_(denoise_device),
      film_(film),
      device_scene_(device_scene),
      render_scheduler_(render_scheduler),
      tile_manager_(tile_manager)
{
  DCHECK_NE(device_, nullptr);

  {
    vector<DeviceInfo> cpu_devices;
    device_cpu_info(cpu_devices);

    cpu_device_ = device_cpu_create(
        cpu_devices[0], device->stats, device->profiler, device_->headless);
  }

  /* Create path tracing work in advance, so that it can be reused by incremental sampling as much
   * as possible. */
  device_->foreach_device([&](Device *path_trace_device) {
    unique_ptr<PathTraceWork> work = PathTraceWork::create(
        path_trace_device, film, device_scene, &render_cancel_.is_requested);
    if (work) {
      path_trace_works_.emplace_back(std::move(work));
    }
  });

  work_balance_infos_.resize(path_trace_works_.size());
  work_balance_do_initial(work_balance_infos_);

  render_scheduler.set_need_schedule_rebalance(path_trace_works_.size() > 1);
}

PathTrace::~PathTrace()
{
  destroy_gpu_resources();
}

void PathTrace::load_kernels()
{
  if (denoiser_) {
    /* Activate graphics interop while denoiser device is created, so that it can choose a device
     * that supports interop for faster display updates. */
    if (display_ && path_trace_works_.size() > 1) {
      display_->graphics_interop_activate();
    }

    denoiser_->load_kernels(progress_);

    if (display_ && path_trace_works_.size() > 1) {
      display_->graphics_interop_deactivate();
    }
  }
}

void PathTrace::alloc_work_memory()
{
  for (auto &&path_trace_work : path_trace_works_) {
    path_trace_work->alloc_work_memory();
  }
}

bool PathTrace::ready_to_reset()
{
  /* The logic here is optimized for the best feedback in the viewport, which implies having a GPU
   * display. Of there is no such display, the logic here will break. */
  DCHECK(display_);

  /* The logic here tries to provide behavior which feels the most interactive feel to artists.
   * General idea is to be able to reset as quickly as possible, while still providing interactive
   * feel.
   *
   * If the render result was ever drawn after previous reset, consider that reset is now possible.
   * This way camera navigation gives the quickest feedback of rendered pixels, regardless of
   * whether CPU or GPU drawing pipeline is used.
   *
   * Consider reset happening after redraw "slow" enough to not clog anything. This is a bit
   * arbitrary, but seems to work very well with viewport navigation in Blender. */

  if (did_draw_after_reset_) {
    return true;
  }

  return false;
}

void PathTrace::reset(const BufferParams &full_params,
                      const BufferParams &big_tile_params,
                      const bool reset_rendering)
{
  if (big_tile_params_.modified(big_tile_params)) {
    big_tile_params_ = big_tile_params;
    render_state_.need_reset_params = true;
  }

  full_params_ = full_params;

  /* NOTE: GPU display checks for buffer modification and avoids unnecessary re-allocation.
   * It is requires to inform about reset whenever it happens, so that the redraw state tracking is
   * properly updated. */
  if (display_) {
    display_->reset(big_tile_params, reset_rendering);
  }

  render_state_.has_denoised_result = false;
  render_state_.tile_written = false;

  did_draw_after_reset_ = false;
}

void PathTrace::device_free()
{
  /* Free render buffers used by the path trace work to reduce memory peak. */
  BufferParams empty_params;
  empty_params.pass_stride = 0;
  empty_params.update_offset_stride();
  for (auto &&path_trace_work : path_trace_works_) {
    path_trace_work->get_render_buffers()->reset(empty_params);
  }
  render_state_.need_reset_params = true;
}

void PathTrace::set_progress(Progress *progress)
{
  progress_ = progress;
}

void PathTrace::render(const RenderWork &render_work)
{
  /* Indicate that rendering has started and that it can be requested to cancel. */
  {
    const thread_scoped_lock lock(render_cancel_.mutex);
    if (render_cancel_.is_requested) {
      return;
    }
    render_cancel_.is_rendering = true;
  }

  render_pipeline(render_work);

  /* Indicate that rendering has finished, making it so thread which requested `cancel()` can carry
   * on. */
  {
    const thread_scoped_lock lock(render_cancel_.mutex);
    render_cancel_.is_rendering = false;
    render_cancel_.condition.notify_one();
  }
}

void PathTrace::render_pipeline(RenderWork render_work)
{
  /* NOTE: Only check for "instant" cancel here. The user-requested cancel via progress is
   * checked in Session and the work in the event of cancel is to be finished here. */

  render_scheduler_.set_need_schedule_cryptomatte(device_scene_->data.film.cryptomatte_passes !=
                                                  0);

  render_init_kernel_execution();
  SCOPED_DEFER(render_deinit_kernel_execution());

  render_scheduler_.report_work_begin(render_work);

  init_render_buffers(render_work);

  rebalance(render_work);

  /* Reset sample limit. */
  render_scheduler_.set_limit_samples_per_update(0);

  /* Prepare all per-thread guiding structures before we start with the next rendering
   * iteration/progression. */
  const bool use_guiding = device_scene_->data.integrator.use_guiding;
  if (use_guiding) {
    guiding_prepare_structures();
  }

  const bool has_volume = device_scene_->data.integrator.use_volumes;
  if (has_volume) {
    const uint num_rendered_samples = render_scheduler_.get_num_rendered_samples();
    const uint limit = next_power_of_two(num_rendered_samples) - num_rendered_samples;
    render_scheduler_.set_limit_samples_per_update(limit);
  }

#ifdef WITH_FALCON_SHARC
  /* In blend mode, load the warmup cache onto the device (read-only) before
   * rendering so the camera-hit kernel can blend with it. In warmup mode the
   * device cache stays zero, so the kernel blend is a no-op. */
  {
    /* Both "blend" (offline 2-pass) and "live" (viewport temporal accumulation)
     * seed the device cache from the file before rendering so the camera-hit
     * kernel can blend with it. In "live" the same file is re-loaded and re-saved
     * every refresh, so the cache accumulates across frames.
     *
     * Mode and path come from the scene now (Integrator::device_update resolves
     * the socket against the environment override), not from getenv here. */
    const int sharc_mode = device_scene_->falcon_sharc_mode;
    if (sharc_mode == FALCON_SHARC_MODE_BLEND || sharc_mode == FALCON_SHARC_MODE_LIVE) {
      const char *cache_path = device_scene_->falcon_sharc_cache_path.c_str();
      float *cache = device_scene_->falcon_sharc_cache.data();
      const size_t cache_floats = device_scene_->falcon_sharc_cache.size();
      FILE *cf = fopen(cache_path, "rb");
      size_t got = 0;
      if (cf) {
        got = fread(cache, sizeof(float), cache_floats, cf);
        fclose(cf);
        device_scene_->falcon_sharc_cache.copy_to_device();
      }
      LOG_INFO << "Falcon SHARC: loaded " << got << "/" << cache_floats
               << " cache floats from " << cache_path << " onto device for in-kernel blend";
    }
  }
#endif

  path_trace(render_work);
  if (render_cancel_.is_requested) {
    return;
  }

#ifdef WITH_FALCON_SHARC
  /* In warmup mode, deposit this render's converged color into the cache and save
   * it for the blend run (the device blend was a no-op since the cache was empty).
   * The per-pixel first-hit world position comes from the Position pass (no
   * kernel-writable buffer, unlike the standalone 5.2 version). In blend mode
   * there is nothing to do -- the kernel already blended into the combined pass. */
  if (path_trace_works_.size() == 1) {
    /* Falcon Photon GPU bake: download the cache the kernel filled and save it
     * in the same format the host tracer writes. */
    const char *pmode = getenv("FALCON_PHOTON_MODE");
    LOG_INFO << "Falcon Photon: save-hook check, pmode=" << (pmode ? pmode : "null")
             << " cache_size=" << device_scene_->falcon_sharc_cache.size();
    if (pmode && strcmp(pmode, "bake") == 0 &&
        device_scene_->falcon_sharc_cache.size() != 0) {
      device_scene_->falcon_sharc_cache.copy_from_device();
      const char *cache_path = device_scene_->falcon_sharc_cache_path.c_str();
      FILE *f = fopen(cache_path, "wb");
      if (f) {
        fwrite(device_scene_->falcon_sharc_cache.data(),
               sizeof(float),
               device_scene_->falcon_sharc_cache.size(),
               f);
        fclose(f);
        LOG_INFO << "Falcon Photon: GPU-baked cache saved to " << cache_path;
      }

      /* Point map (Round 9): save the raw photon points the bake appended.
       * Format: {magic 'FPH1', uint32 count} + count * 9 floats. Only when
       * FALCON_PHOTON_POINTS names the output file. */
      const char *pts_env = getenv("FALCON_PHOTON_POINTS");
      if (pts_env && device_scene_->falcon_photon_points.size() != 0 &&
          device_scene_->falcon_photon_pcount.size() != 0) {
        device_scene_->falcon_photon_pcount.copy_from_device();
        uint32_t n = device_scene_->falcon_photon_pcount.data()[0];
        const uint32_t max_pts = (uint32_t)(device_scene_->falcon_photon_points.size() / 9);
        if (n > max_pts) {
          n = max_pts; /* counter keeps counting past the cap; clamp */
        }
        device_scene_->falcon_photon_points.copy_from_device();
        FILE *pf = fopen(pts_env, "wb");
        if (pf) {
          const uint32_t header[2] = {0x46504831u, n};
          fwrite(header, sizeof(uint32_t), 2, pf);
          fwrite(device_scene_->falcon_photon_points.data(), sizeof(float), (size_t)n * 9, pf);
          fclose(pf);
          LOG_INFO << "Falcon Photon: point map saved, " << n << " points to " << pts_env;
        }
      }
    }

    const bool is_warmup = device_scene_->falcon_sharc_mode == FALCON_SHARC_MODE_WARMUP;
    const bool is_live = device_scene_->falcon_sharc_mode == FALCON_SHARC_MODE_LIVE;
    if (is_warmup || is_live) {
      /* The grid the kernel will look up with -- deposit into the same one. */
      const float kernel_cell = device_scene_->data.integrator.falcon_sharc_cell_size;
      const float host_cell_size = (kernel_cell > 1e-4f) ? kernel_cell :
                                                           FALCON_SHARC_HOST_CELL_SIZE_FALLBACK;
      const char *cache_path = device_scene_->falcon_sharc_cache_path.c_str();
      path_trace_works_[0]->copy_render_buffers_from_device();
      const RenderBuffers *rb = path_trace_works_[0]->get_render_buffers();
      const BufferParams &p = rb->params;
      const int combined_offset = p.get_pass_offset(PASS_COMBINED);
      const int position_offset = p.get_pass_offset(PASS_POSITION);
      if (combined_offset == PASS_UNUSED || position_offset == PASS_UNUSED) {
        LOG_INFO << "Falcon SHARC: needs both the Combined and Position passes "
                 << "(enable the Position pass); skipping deposit";
      }
      else {
        const float *buf = rb->buffer.data();
        float *cache = device_scene_->falcon_sharc_cache.data();
        const size_t cache_floats = device_scene_->falcon_sharc_cache.size();
        const int w = p.width, h = p.height, stride = p.stride;
        const int pass_stride = p.pass_stride;
        const int num_samples = max(render_scheduler_.get_num_rendered_samples(), 1);
        const float inv_s = 1.0f / num_samples;
        size_t deposited = 0;
        if (is_warmup) {
          /* Warmup rebuilds the cache from scratch each render. */
          memset(cache, 0, cache_floats * sizeof(float));
        }
        else {
          /* Live temporal accumulation: decay every cell once so old frames fade
           * out with an exponential recency window (FALCON_SHARC_KEEP, default
           * 0.9). Decaying sum and count together leaves sum/count (the estimate)
           * unchanged but bounds how much history a cell keeps -- this is what
           * lets a static viewport converge while a moving one still adapts. */
          const float keep = device_scene_->falcon_sharc_keep;
          for (size_t i = 0; i < cache_floats; i++) {
            cache[i] *= keep;
          }
        }
        /* Within-cell dispersion, written next to the cache as a sidecar.
         *
         * How wrong is it to answer a path with this cell's mean? Not "how
         * noisy is the mean" -- two warmup bakes at different seeds agree to
         * 1e-6 here, because a cell averages ~18 pixels x 64 spp (measured
         * 2026-07-30, and it is why the seed-pair version of this measurement
         * was useless). The mean is wrong where the radiance genuinely VARIES
         * inside the cell: geometry detail, a shadow edge, view dependence.
         * That is a spread, so accumulate sum and sum-of-squares of luminance
         * and let the tooling turn it into a relative standard deviation. */
        std::vector<double> lum_sum(FALCON_SHARC_HOST_CELL_COUNT, 0.0);
        std::vector<double> lum_sq(FALCON_SHARC_HOST_CELL_COUNT, 0.0);
        for (int y = 0; y < h; y++) {
          for (int x = 0; x < w; x++) {
            const int idx = p.offset + x + y * stride;
            const float *pos = buf + (size_t)idx * pass_stride + position_offset;
            /* Position pass is overwrite (not sample-averaged); (0,0,0) means the
             * camera ray missed all geometry, so skip it. */
            if (pos[0] == 0.0f && pos[1] == 0.0f && pos[2] == 0.0f) {
              continue;
            }
            const unsigned int cell = falcon_sharc_host_hash(
                pos[0], pos[1], pos[2], host_cell_size);
            const float *px = buf + (size_t)idx * pass_stride + combined_offset;
            const size_t cbase = (size_t)cell * FALCON_SHARC_HOST_CELL_STRIDE;
            const double l = (0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]) * inv_s;
            lum_sum[cell] += l;
            lum_sq[cell] += l * l;
            cache[cbase + 0] += px[0] * inv_s;
            cache[cbase + 1] += px[1] * inv_s;
            cache[cbase + 2] += px[2] * inv_s;
            cache[cbase + 3] += 1.0f;
            deposited++;
          }
        }
        if (is_live) {
          /* Push the updated cache back so this session's next frame blends with
           * it (the file save keeps cross-process/headless accumulation working
           * too). */
          device_scene_->falcon_sharc_cache.copy_to_device();
        }
        FILE *cf = fopen(cache_path, "wb");
        if (cf) {
          fwrite(cache, sizeof(float), cache_floats, cf);
          fclose(cf);
          LOG_INFO << "Falcon SHARC " << (is_live ? "live" : "warmup") << ": " << num_samples
                   << " spp, deposited " << deposited << " pixels, cache " << cache_path;
        }

        /* Sidecar: relative standard deviation of luminance inside each cell,
         * negative where nothing landed. Same 4M layout as the cache so the
         * tooling can hand it to the kernel's error-field slot unchanged. */
        {
          const string var_path = string(cache_path) + ".spread";
          std::vector<float> spread(FALCON_SHARC_HOST_CELL_COUNT, -1.0f);
          for (size_t c = 0; c < FALCON_SHARC_HOST_CELL_COUNT; c++) {
            const float n = cache[c * FALCON_SHARC_HOST_CELL_STRIDE + 3];
            if (n >= 1.0f) {
              const double mean = lum_sum[c] / n;
              const double var = max(lum_sq[c] / n - mean * mean, 0.0);
              spread[c] = (mean > 1e-9) ? (float)(sqrt(var) / mean) : 0.0f;
            }
          }
          FILE *vf = fopen(var_path.c_str(), "wb");
          if (vf) {
            const uint32_t header[2] = {0x46454631u, (uint32_t)FALCON_SHARC_HOST_CELL_COUNT};
            fwrite(header, sizeof(uint32_t), 2, vf);
            fwrite(&host_cell_size, sizeof(float), 1, vf);
            fwrite(spread.data(), sizeof(float), spread.size(), vf);
            fclose(vf);
            LOG_INFO << "Falcon SHARC: cell spread written to " << var_path;
          }
        }

        /* Measure GI dominance for the auto-gate: luminance of Diffuse Indirect
         * over Diffuse Direct + Indirect. SHARC (a radiance cache) helps when
         * indirect light dominates and hurts direct-lit scenes (Phase0 data), so
         * the blend run reads this ratio to scale alpha. Written as a one-float
         * sidecar next to the cache; the blend run picks it up. */
        const int dd_offset = p.get_pass_offset(PASS_DIFFUSE_DIRECT);
        const int di_offset = p.get_pass_offset(PASS_DIFFUSE_INDIRECT);
        if (dd_offset != PASS_UNUSED && di_offset != PASS_UNUSED) {
          double sum_direct = 0.0, sum_indirect = 0.0;
          for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
              const int idx = p.offset + x + y * stride;
              const float *dd = buf + (size_t)idx * pass_stride + dd_offset;
              const float *di = buf + (size_t)idx * pass_stride + di_offset;
              /* Rec.709 luminance, scaled to converged (per-sample) value. */
              sum_direct += (0.2126 * dd[0] + 0.7152 * dd[1] + 0.0722 * dd[2]) * inv_s;
              sum_indirect += (0.2126 * di[0] + 0.7152 * di[1] + 0.0722 * di[2]) * inv_s;
            }
          }
          const double total = sum_direct + sum_indirect;
          const float gi_ratio = total > 1e-6 ? (float)(sum_indirect / total) : 0.0f;
          string meta_path = string(cache_path) + ".meta";
          FILE *mf = fopen(meta_path.c_str(), "w");
          if (mf) {
            fprintf(mf, "%.6f\n", gi_ratio);
            fclose(mf);
            LOG_INFO << "Falcon SHARC: GI ratio " << gi_ratio << " (indirect/total), "
                     << meta_path;
          }
        }
      }
    }
  }
#endif

  /* Update the guiding field using the training data/samples collected during the rendering
   * iteration/progression. */
  const bool train_guiding = device_scene_->data.integrator.train_guiding;
  if (use_guiding && train_guiding) {
    guiding_update_structures();
  }

  adaptive_sample(render_work);
  if (render_cancel_.is_requested) {
    return;
  }

  cryptomatte_postprocess(render_work);
  if (render_cancel_.is_requested) {
    return;
  }

  denoise(render_work);
  if (render_cancel_.is_requested) {
    return;
  }

  denoise_volume_guiding_buffers(render_work, has_volume);
  if (render_cancel_.is_requested) {
    return;
  }

  write_tile_buffer(render_work);
  update_display(render_work);

  progress_update_if_needed(render_work);

  finalize_full_buffer_on_disk(render_work);
}

void PathTrace::render_init_kernel_execution()
{
  for (auto &&path_trace_work : path_trace_works_) {
    path_trace_work->init_execution();
  }
}

void PathTrace::render_deinit_kernel_execution()
{
  for (auto &&path_trace_work : path_trace_works_) {
    path_trace_work->deinit_execution();
  }
}

/* TODO(sergey): Look into `std::function` rather than using a template. Should not be a
 * measurable performance impact at runtime, but will make compilation faster and binary somewhat
 * smaller. */
template<typename Callback>
static void foreach_sliced_buffer_params(const vector<unique_ptr<PathTraceWork>> &path_trace_works,
                                         const vector<WorkBalanceInfo> &work_balance_infos,
                                         const BufferParams &buffer_params,
                                         const int overscan,
                                         const Callback &callback)
{
  const int num_works = path_trace_works.size();
  const int window_height = buffer_params.window_height;

  int current_y = 0;
  for (int i = 0; i < num_works; ++i) {
    const double weight = work_balance_infos[i].weight;
    const int slice_window_full_y = buffer_params.full_y + buffer_params.window_y + current_y;
    const int slice_window_height = max(lround(window_height * weight), 1);

    /* Disallow negative values to deal with situations when there are more compute devices than
     * scan-lines. */
    const int remaining_window_height = max(0, window_height - current_y);

    BufferParams slice_params = buffer_params;

    slice_params.full_y = max(slice_window_full_y - overscan, buffer_params.full_y);
    slice_params.window_y = slice_window_full_y - slice_params.full_y;

    if (i < num_works - 1) {
      slice_params.window_height = min(slice_window_height, remaining_window_height);
    }
    else {
      slice_params.window_height = remaining_window_height;
    }

    slice_params.height = slice_params.window_y + slice_params.window_height + overscan;
    slice_params.height = min(slice_params.height,
                              buffer_params.height + buffer_params.full_y - slice_params.full_y);

    slice_params.update_offset_stride();

    callback(path_trace_works[i].get(), slice_params);

    current_y += slice_params.window_height;
  }
}

void PathTrace::update_allocated_work_buffer_params()
{
  const int overscan = tile_manager_.get_tile_overscan();
  foreach_sliced_buffer_params(path_trace_works_,
                               work_balance_infos_,
                               big_tile_params_,
                               overscan,
                               [](PathTraceWork *path_trace_work, const BufferParams &params) {
                                 RenderBuffers *buffers = path_trace_work->get_render_buffers();
                                 buffers->reset(params);
                               });
}

static BufferParams scale_buffer_params(const BufferParams &params, const float resolution_divider)
{
  BufferParams scaled_params = params;

  scaled_params.width = max(1, int(params.width / resolution_divider));
  scaled_params.height = max(1, int(params.height / resolution_divider));

  scaled_params.window_x = int(params.window_x / resolution_divider);
  scaled_params.window_y = int(params.window_y / resolution_divider);
  scaled_params.window_width = max(1, int(params.window_width / resolution_divider));
  scaled_params.window_height = max(1, int(params.window_height / resolution_divider));

  scaled_params.full_x = int(params.full_x / resolution_divider);
  scaled_params.full_y = int(params.full_y / resolution_divider);
  scaled_params.full_width = max(1, int(params.full_width / resolution_divider));
  scaled_params.full_height = max(1, int(params.full_height / resolution_divider));

  scaled_params.update_offset_stride();

  return scaled_params;
}

void PathTrace::update_effective_work_buffer_params(const RenderWork &render_work)
{
  const float denoised_resolution_divider = render_work.denoised_resolution_divider;
  const float resolution_divider = render_work.resolution_divider / denoised_resolution_divider;

  const BufferParams denoised_big_tile_params = scale_buffer_params(big_tile_params_,
                                                                    denoised_resolution_divider);
  const BufferParams scaled_big_tile_params = scale_buffer_params(denoised_big_tile_params,
                                                                  resolution_divider);

  const int overscan = tile_manager_.get_tile_overscan();

  foreach_sliced_buffer_params(
      path_trace_works_,
      work_balance_infos_,
      denoised_big_tile_params,
      overscan,
      [&](PathTraceWork *path_trace_work, const BufferParams params) {
        /* Scale down the sliced buffer parameters again that were scaled by denoising upscale
         * factor above. This should match the values that would occur when slicing
         * 'scaled_big_tile_params' directly. */
        const BufferParams scaled_params = scale_buffer_params(params, resolution_divider);
        path_trace_work->set_effective_buffer_params(
            scaled_big_tile_params, scaled_params, denoised_big_tile_params, params);
      });

  render_state_.effective_big_tile_params = scaled_big_tile_params;
  render_state_.effective_denoised_big_tile_params = denoised_big_tile_params;
}

void PathTrace::update_work_buffer_params_if_needed(const RenderWork &render_work)
{
  if (render_state_.need_reset_params) {
    update_allocated_work_buffer_params();
  }

  if (render_state_.need_reset_params ||
      render_state_.resolution_divider != render_work.resolution_divider)
  {
    update_effective_work_buffer_params(render_work);
  }

  render_state_.resolution_divider = render_work.resolution_divider;
  render_state_.need_reset_params = false;
}

void PathTrace::init_render_buffers(const RenderWork &render_work)
{
  update_work_buffer_params_if_needed(render_work);

  /* Handle initialization scheduled by the render scheduler. */
  if (render_work.init_render_buffers) {
    parallel_for_each(path_trace_works_, [&](unique_ptr<PathTraceWork> &path_trace_work) {
      path_trace_work->zero_render_buffers();
    });

    tile_buffer_read();
  }
}

void PathTrace::path_trace(RenderWork &render_work)
{
  if (!render_work.path_trace.num_samples) {
    return;
  }

  LOG_DEBUG << "Will path trace " << render_work.path_trace.num_samples
            << " samples at the resolution divider " << render_work.resolution_divider;

  const double start_time = time_dt();

  const int num_works = path_trace_works_.size();

  thread_capture_fp_settings();

  parallel_for(0, num_works, [&](int i) {
    const double work_start_time = time_dt();
    const int num_samples = render_work.path_trace.num_samples;

    PathTraceWork *path_trace_work = path_trace_works_[i].get();
    if (path_trace_work->get_device()->have_error()) {
      return;
    }

    PathTraceWork::RenderStatistics statistics;
    path_trace_work->render_samples(statistics,
                                    render_work.path_trace.start_sample,
                                    num_samples,
                                    render_work.path_trace.sample_offset);

    DCHECK(isfinite(statistics.occupancy));

    const double work_time = time_dt() - work_start_time;
    work_balance_infos_[i].time_spent += work_time;
    work_balance_infos_[i].occupancy = statistics.occupancy;

    LOG_INFO << "Rendered " << num_samples << " samples in " << work_time << " seconds ("
             << work_time / num_samples
             << " seconds per sample), occupancy: " << statistics.occupancy;
  });

  float occupancy_accum = 0.0f;
  for (const WorkBalanceInfo &balance_info : work_balance_infos_) {
    occupancy_accum += balance_info.occupancy;
  }
  const float occupancy = occupancy_accum / num_works;
  render_scheduler_.report_path_trace_occupancy(render_work, occupancy);

  render_scheduler_.report_path_trace_time(
      render_work, time_dt() - start_time, is_cancel_requested());
}

void PathTrace::adaptive_sample(RenderWork &render_work)
{
  if (!render_work.adaptive_sampling.filter) {
    return;
  }

  bool did_reschedule_on_idle = false;

  while (true) {
    LOG_DEBUG << "Will filter adaptive stopping buffer, threshold "
              << render_work.adaptive_sampling.threshold;
    if (render_work.adaptive_sampling.reset) {
      LOG_DEBUG << "Will re-calculate convergency flag for currently converged pixels.";
    }

    const double start_time = time_dt();

    uint num_active_pixels = 0;
    parallel_for_each(path_trace_works_, [&](unique_ptr<PathTraceWork> &path_trace_work) {
      const uint num_active_pixels_in_work =
          path_trace_work->adaptive_sampling_converge_filter_count_active(
              render_work.adaptive_sampling.threshold, render_work.adaptive_sampling.reset);
      if (num_active_pixels_in_work) {
        atomic_add_and_fetch_u(&num_active_pixels, num_active_pixels_in_work);
      }
    });

    render_scheduler_.report_adaptive_filter_time(
        render_work, time_dt() - start_time, is_cancel_requested());

    if (num_active_pixels == 0) {
      LOG_DEBUG << "All pixels converged.";
      if (!render_scheduler_.render_work_reschedule_on_converge(render_work)) {
        break;
      }
      LOG_DEBUG << "Continuing with lower threshold.";
    }
    else if (did_reschedule_on_idle) {
      break;
    }
    else if (num_active_pixels < 128 * 128) {
      /* NOTE: The hardcoded value of 128^2 is more of an empirical value to keep GPU busy so that
       * there is no performance loss from the progressive noise floor feature.
       *
       * A better heuristic is possible here: for example, use maximum of 128^2 and percentage of
       * the final resolution. */
      if (!render_scheduler_.render_work_reschedule_on_idle(render_work)) {
        LOG_DEBUG << "Rescheduling is not possible: final threshold is reached.";
        break;
      }
      LOG_DEBUG << "Rescheduling lower threshold.";
      did_reschedule_on_idle = true;
    }
    else {
      break;
    }
  }
}

void PathTrace::clear_denoiser_temporal_history()
{
  if (denoiser_) {
    denoiser_->clear_temporal_history();
  }
}

void PathTrace::set_denoiser_params(const DenoiseParams &params)
{
  if (!params.use) {
    denoiser_.reset();
    render_scheduler_.set_denoiser_params(params);
    return;
  }

  GraphicsInteropDevice interop_device;
  if (display_) {
    interop_device = display_->graphics_interop_get_device();
  }

  Device *effective_denoise_device;
  Device *cpu_fallback_device = cpu_device_.get();
  DenoiseParams effective_denoise_params = get_effective_denoise_params(
      denoise_device_, cpu_fallback_device, params, interop_device, effective_denoise_device);

  /* Carrying the DLSS-RR temporal history is allowed in both modes now:
   * final (background) renders carry it across animation frames
   * (denoising_carry_history), the viewport carries it across navigation
   * restarts aligned by the interactive motion passes
   * (preview_denoising_carry_history). Each mode's sync sets its own flag. */

  bool need_to_recreate_denoiser = false;
  if (denoiser_) {
    const DenoiseParams old_denoiser_params = denoiser_->get_params();

    const bool is_cpu_denoising = old_denoiser_params.type == DENOISER_OPENIMAGEDENOISE &&
                                  old_denoiser_params.use_gpu == false;
    const bool always_gpu_denoising = effective_denoise_params.type == DENOISER_DLSS ||
                                      effective_denoise_params.type == DENOISER_OPTIX;
    const bool requested_gpu_denoising = always_gpu_denoising ||
                                         (effective_denoise_params.type ==
                                              DENOISER_OPENIMAGEDENOISE &&
                                          effective_denoise_params.use_gpu == true);
    if (requested_gpu_denoising && is_cpu_denoising &&
        effective_denoise_device->info.type == DEVICE_CPU)
    {
      /* It won't be possible to use GPU denoising when according to user settings we have
       * only CPU as available denoising device. So we just exiting early to avoid
       * unnecessary denoiser recreation or parameters update. */
      return;
    }

    const bool is_same_denoising_device_type = old_denoiser_params.use_gpu ==
                                               effective_denoise_params.use_gpu;
    /* Optix Denoiser is not supporting CPU devices, so use_gpu option is not
     * shown in the UI and changes in the option value should not be checked. */
    if (old_denoiser_params.type == effective_denoise_params.type &&
        (is_same_denoising_device_type || always_gpu_denoising))
    {
      denoiser_->set_params(effective_denoise_params);
    }
    else {
      need_to_recreate_denoiser = true;
    }
  }
  else {
    /* if there is no denoiser and param.use is true, then we need to create it. */
    need_to_recreate_denoiser = true;
  }

  if (need_to_recreate_denoiser) {
    denoiser_ = Denoiser::create(
        effective_denoise_device, cpu_fallback_device, effective_denoise_params, interop_device);

    if (denoiser_) {
      /* Only take into account the "immediate" cancel to have interactive rendering responding to
       * navigation as quickly as possible, but allow to run denoiser after user hit Escape key
       * while doing offline rendering. */
      denoiser_->is_cancelled_cb = [this]() { return render_cancel_.is_requested; };
    }
  }

  /* Use actual parameters, if available */
  if (denoise_device_ && denoiser_) {
    render_scheduler_.set_denoiser_params(denoiser_->get_params());
  }
  else {
    render_scheduler_.set_denoiser_params(effective_denoise_params);
  }
}

void PathTrace::set_adaptive_sampling(const AdaptiveSampling &adaptive_sampling)
{
  render_scheduler_.set_adaptive_sampling(adaptive_sampling);
}

void PathTrace::cryptomatte_postprocess(const RenderWork &render_work)
{
  if (!render_work.cryptomatte.postprocess) {
    return;
  }
  LOG_DEBUG << "Perform cryptomatte work.";

  parallel_for_each(path_trace_works_, [&](unique_ptr<PathTraceWork> &path_trace_work) {
    path_trace_work->cryptomatte_postproces();
  });
}

void PathTrace::denoise(const RenderWork &render_work)
{
  if (!render_work.tile.denoise) {
    return;
  }

  if (!denoiser_) {
    /* Denoiser was not configured, so nothing to do here. */
    return;
  }

  LOG_DEBUG << "Perform denoising work.";

  denoiser_->set_same_frame_restart(render_work.denoise_same_frame_restart);

  {
    /* The specular hit distance is a world-space length; RR needs the camera
     * matrices to place the reflection it describes.
     *
     * NGX wants these row-major and left-multiply (v * M). Cycles stores its
     * matrices row-major but right-multiply (M * v), so they have to be
     * transposed -- NVIDIA's own sample gets away with passing glm matrices
     * untouched precisely because glm's column-major storage already amounts to
     * the transpose. Handing ours over as-is (which is what the first attempt at
     * this did) feeds RR a garbled matrix, and it quietly ignores the guide.
     *
     * Cycles keeps world->camera and world->NDC; view->clip is world->NDC with
     * the camera undone, and NDC ([0,1]) remapped to clip ([-1,1]). */
    const KernelCamera &cam = device_scene_->data.cam;

    const ProjectionTransform camera_to_world(transform_inverse(cam.worldtocamera));
    ProjectionTransform ndc_to_clip = projection_identity();
    ndc_to_clip.x = make_float4(2.0f, 0.0f, 0.0f, -1.0f);
    ndc_to_clip.y = make_float4(0.0f, 2.0f, 0.0f, -1.0f);

    const ProjectionTransform world_to_view(cam.worldtocamera);
    const ProjectionTransform view_to_clip = ndc_to_clip * cam.worldtondc * camera_to_world;

    const ProjectionTransform world_to_view_ngx = projection_transpose(world_to_view);
    const ProjectionTransform view_to_clip_ngx = projection_transpose(view_to_clip);

    denoiser_->set_camera_matrices(&world_to_view_ngx.x.x, &view_to_clip_ngx.x.x);
  }

  const double start_time = time_dt();

  RenderBuffers *buffer_to_denoise = nullptr;
  bool allow_inplace_modification = false;

  Device *denoiser_device = denoiser_->get_denoiser_device();
  if (path_trace_works_.size() > 1 && denoiser_device && !big_tile_denoise_work_) {
    big_tile_denoise_work_ = PathTraceWork::create(denoiser_device, film_, device_scene_, nullptr);
  }

  if (big_tile_denoise_work_) {
    big_tile_denoise_work_->set_effective_buffer_params(
        render_state_.effective_big_tile_params,
        render_state_.effective_big_tile_params,
        render_state_.effective_denoised_big_tile_params,
        render_state_.effective_denoised_big_tile_params);

    buffer_to_denoise = big_tile_denoise_work_->get_render_buffers();
    buffer_to_denoise->reset(render_state_.effective_denoised_big_tile_params);

    copy_to_render_buffers(buffer_to_denoise);

    allow_inplace_modification = true;
  }
  else {
    DCHECK_EQ(path_trace_works_.size(), 1);

    buffer_to_denoise = path_trace_works_.front()->get_render_buffers();
  }

  if (denoiser_->denoise_buffer(render_state_.effective_big_tile_params,
                                render_state_.effective_denoised_big_tile_params,
                                buffer_to_denoise,
                                get_num_samples_in_buffer(),
                                allow_inplace_modification,
                                device_scene_->data.integrator.pixel_jitter))
  {
    render_state_.has_denoised_result = true;
  }

  render_scheduler_.report_denoise_time(render_work, time_dt() - start_time);
}

void PathTrace::denoise_volume_guiding_buffers(const RenderWork &render_work,
                                               const bool has_volume)
{
  if (!has_volume || !render_work.volume_guiding_denoise) {
    return;
  }

  LOG_DEBUG << "Denoise volume guiding buffers.";

  const double start_time = time_dt();

  /* TODO: in the multi-GPU case, we can denoise on one device and copy to the rest, instead of
   * denoising on each device separately. */
  parallel_for_each(path_trace_works_, [&](unique_ptr<PathTraceWork> &path_trace_work) {
    path_trace_work->denoise_volume_guiding_buffers();
  });

  render_scheduler_.report_volume_guiding_denoise_time(render_work, time_dt() - start_time);
}

void PathTrace::set_output_driver(unique_ptr<OutputDriver> driver)
{
  output_driver_ = std::move(driver);
}

void PathTrace::set_display_driver(unique_ptr<DisplayDriver> driver)
{
  /* The display driver is the source of the drawing context which might be used by
   * path trace works. Make sure there is no graphics interop using resources from
   * the old display, as it might no longer be available after this call. */
  destroy_gpu_resources();

  if (driver) {
    display_ = make_unique<PathTraceDisplay>(std::move(driver));
  }
  else {
    display_ = nullptr;
  }
}

void PathTrace::zero_display()
{
  if (display_) {
    display_->zero();
  }
}

void PathTrace::draw()
{
  if (!display_) {
    return;
  }

  did_draw_after_reset_ |= display_->draw();
}

void PathTrace::flush_display()
{
  if (!display_) {
    return;
  }

  display_->flush();
}

void PathTrace::update_display(const RenderWork &render_work)
{
  if (!render_work.display.update) {
    return;
  }

  if (!display_ && !output_driver_) {
    LOG_DEBUG << "Ignore display update.";
    return;
  }

  if (full_params_.width == 0 || full_params_.height == 0) {
    LOG_DEBUG << "Skipping PathTraceDisplay update due to 0 size of the render buffer.";
    return;
  }

  const double start_time = time_dt();

  if (output_driver_) {
    LOG_DEBUG << "Invoke buffer update callback.";

    const PathTraceTile tile(*this);
    output_driver_->update_render_tile(tile);
  }

  if (display_) {
    LOG_DEBUG << "Perform copy to GPUDisplay work.";

    const PassType pass_type = film_->get_display_pass();
    const bool show_denoised =
        ((render_work.display.use_denoised_result && has_denoised_result() &&
          big_tile_params_.get_pass_offset(pass_type, PassMode::DENOISED) != PASS_UNUSED) ||
         is_volume_guiding_pass(pass_type));

    const int texture_width = show_denoised ?
                                  render_state_.effective_denoised_big_tile_params.window_width :
                                  render_state_.effective_big_tile_params.window_width;
    const int texture_height = show_denoised ?
                                   render_state_.effective_denoised_big_tile_params.window_height :
                                   render_state_.effective_big_tile_params.window_height;
    if (!display_->update_begin(texture_width, texture_height)) {
      LOG_ERROR << "Error beginning GPUDisplay update.";
      return;
    }

    const PassMode pass_mode = show_denoised ? PassMode::DENOISED : PassMode::NOISY;

    /* TODO(sergey): When using multi-device rendering map the GPUDisplay once and copy data from
     * all works in parallel. */
    const int num_samples = get_num_samples_in_buffer();
    if (big_tile_denoise_work_ && render_state_.has_denoised_result) {
      big_tile_denoise_work_->copy_to_display(display_.get(), pass_mode, num_samples);
    }
    else {
      for (auto &&path_trace_work : path_trace_works_) {
        path_trace_work->copy_to_display(display_.get(), pass_mode, num_samples);
      }
    }

    display_->update_end();
  }

  render_scheduler_.report_display_update_time(render_work, time_dt() - start_time);
}

void PathTrace::rebalance(const RenderWork &render_work)
{
  if (!render_work.rebalance) {
    return;
  }

  const int num_works = path_trace_works_.size();

  if (num_works == 1) {
    LOG_DEBUG << "Ignoring rebalance work due to single device render.";
    return;
  }

  const double start_time = time_dt();

  if (LOG_IS_ON(LOG_LEVEL_DEBUG)) {
    LOG_DEBUG << "Perform rebalance work.";
    LOG_DEBUG << "Per-device path tracing time (seconds):";
    for (int i = 0; i < num_works; ++i) {
      LOG_DEBUG << path_trace_works_[i]->get_device()->info.description << ": "
                << work_balance_infos_[i].time_spent;
    }
  }

  const bool did_rebalance = work_balance_do_rebalance(work_balance_infos_);

  if (LOG_IS_ON(LOG_LEVEL_DEBUG)) {
    LOG_DEBUG << "Calculated per-device weights for works:";
    for (int i = 0; i < num_works; ++i) {
      LOG_DEBUG << path_trace_works_[i]->get_device()->info.description << ": "
                << work_balance_infos_[i].weight;
    }
  }

  if (!did_rebalance) {
    LOG_DEBUG << "Balance in path trace works did not change.";
    render_scheduler_.report_rebalance_time(render_work, time_dt() - start_time, false);
    return;
  }

  RenderBuffers big_tile_cpu_buffers(cpu_device_.get());
  big_tile_cpu_buffers.reset(render_state_.effective_big_tile_params);

  copy_to_render_buffers(&big_tile_cpu_buffers);

  render_state_.need_reset_params = true;
  update_work_buffer_params_if_needed(render_work);

  copy_from_render_buffers(&big_tile_cpu_buffers);

  render_scheduler_.report_rebalance_time(render_work, time_dt() - start_time, true);
}

void PathTrace::write_tile_buffer(const RenderWork &render_work)
{
  if (!render_work.tile.write) {
    return;
  }

  LOG_DEBUG << "Write tile result.";

  render_state_.tile_written = true;

  const bool has_multiple_tiles = tile_manager_.has_multiple_tiles();

  /* Write render tile result, but only if not using tiled rendering.
   *
   * Tiles are written to a file during rendering, and written to the software at the end
   * of rendering (wither when all tiles are finished, or when rendering was requested to be
   * canceled).
   *
   * Important thing is: tile should be written to the software via callback only once. */
  if (!has_multiple_tiles) {
    LOG_DEBUG << "Write tile result via buffer write callback.";
    tile_buffer_write();
  }
  /* Write tile to disk, so that the render work's render buffer can be re-used for the next tile.
   */
  else {
    LOG_DEBUG << "Write tile result to disk.";
    tile_buffer_write_to_disk();
  }
}

void PathTrace::finalize_full_buffer_on_disk(const RenderWork &render_work)
{
  if (!render_work.full.write) {
    return;
  }

  LOG_DEBUG << "Handle full-frame render buffer work.";

  if (!tile_manager_.has_written_tiles()) {
    LOG_DEBUG << "No tiles on disk.";
    return;
  }

  /* Make sure writing to the file is fully finished.
   * This will include writing all possible missing tiles, ensuring validness of the file. */
  tile_manager_.finish_write_tiles();

  /* NOTE: The rest of full-frame post-processing (such as full-frame denoising) will be done after
   * all scenes and layers are rendered by the Session (which happens after freeing Session memory,
   * so that we never hold scene and full-frame buffer in memory at the same time). */
}

void PathTrace::cancel()
{
  thread_scoped_lock lock(render_cancel_.mutex);

  /* Only cancel in the middle of rendering when there is at least one sample in the output.
   * Otherwise interactivity becomes bad. */
  if (get_num_samples_in_buffer() > 1) {
    render_cancel_.is_requested = true;
  }

  while (render_cancel_.is_rendering) {
    render_cancel_.condition.wait(lock);
  }

  render_cancel_.is_requested = false;
}

int PathTrace::get_num_samples_in_buffer()
{
  return render_scheduler_.get_num_rendered_samples();
}

bool PathTrace::is_cancel_requested()
{
  if (render_cancel_.is_requested) {
    return true;
  }

  if (progress_ != nullptr) {
    if (progress_->get_cancel()) {
      return true;
    }
  }

  return false;
}

void PathTrace::tile_buffer_write()
{
  if (!output_driver_) {
    return;
  }

  const PathTraceTile tile(*this);
  output_driver_->write_render_tile(tile);
}

void PathTrace::tile_buffer_read()
{
  if (!device_scene_->data.bake.use) {
    return;
  }

  if (!output_driver_) {
    return;
  }

  /* Read buffers back from device. */
  parallel_for_each(path_trace_works_, [&](unique_ptr<PathTraceWork> &path_trace_work) {
    path_trace_work->copy_render_buffers_from_device();
  });

  /* Read (subset of) passes from output driver. */
  const PathTraceTile tile(*this);
  if (output_driver_->read_render_tile(tile)) {
    /* Copy buffers to device again. */
    parallel_for_each(path_trace_works_, [](unique_ptr<PathTraceWork> &path_trace_work) {
      path_trace_work->copy_render_buffers_to_device();
    });
  }
}

void PathTrace::tile_buffer_write_to_disk()
{
  /* Sample count pass is required to support per-tile partial results stored in the file. */
  DCHECK_NE(big_tile_params_.get_pass_offset(PASS_SAMPLE_COUNT), PASS_UNUSED);

  const int num_rendered_samples = render_scheduler_.get_num_rendered_samples();

  if (num_rendered_samples == 0) {
    /* The tile has zero samples, no need to write it. */
    return;
  }

  /* Get access to the CPU-side render buffers of the current big tile. */
  RenderBuffers *buffers;
  RenderBuffers big_tile_cpu_buffers(cpu_device_.get());

  if (path_trace_works_.size() == 1) {
    path_trace_works_[0]->copy_render_buffers_from_device();
    buffers = path_trace_works_[0]->get_render_buffers();
  }
  else {
    big_tile_cpu_buffers.reset(render_state_.effective_big_tile_params);
    copy_to_render_buffers(&big_tile_cpu_buffers);

    buffers = &big_tile_cpu_buffers;
  }

  if (!tile_manager_.write_tile(*buffers)) {
    device_->set_error("Error writing tile to file");
  }
}

void PathTrace::progress_update_if_needed(const RenderWork &render_work)
{
  if (progress_ != nullptr) {
    const int2 tile_size = get_render_tile_size();
    const uint64_t num_samples_added = uint64_t(tile_size.x) * tile_size.y *
                                       render_work.path_trace.num_samples;
    const int current_sample = render_work.path_trace.start_sample +
                               render_work.path_trace.num_samples -
                               render_work.path_trace.sample_offset;
    progress_->add_samples(num_samples_added, current_sample);
  }

  if (progress_update_cb) {
    progress_update_cb();
  }
}

void PathTrace::progress_set_status(const string &status, const string &substatus)
{
  if (progress_ != nullptr) {
    progress_->set_status(status, substatus);
  }
}

void PathTrace::copy_to_render_buffers(RenderBuffers *render_buffers)
{
  parallel_for_each(path_trace_works_,
                    [&render_buffers](unique_ptr<PathTraceWork> &path_trace_work) {
                      path_trace_work->copy_to_render_buffers(render_buffers);
                    });
  render_buffers->copy_to_device();
}

void PathTrace::copy_from_render_buffers(RenderBuffers *render_buffers)
{
  render_buffers->copy_from_device();
  parallel_for_each(path_trace_works_,
                    [&render_buffers](unique_ptr<PathTraceWork> &path_trace_work) {
                      path_trace_work->copy_from_render_buffers(render_buffers);
                    });
}

bool PathTrace::copy_render_tile_from_device()
{
  if (full_frame_state_.render_buffers) {
    /* Full-frame buffer is always allocated on CPU. */
    return true;
  }

  if (big_tile_denoise_work_ && render_state_.has_denoised_result) {
    return big_tile_denoise_work_->copy_render_buffers_from_device();
  }

  bool success = true;

  parallel_for_each(path_trace_works_, [&](unique_ptr<PathTraceWork> &path_trace_work) {
    if (!success) {
      return;
    }
    if (!path_trace_work->copy_render_buffers_from_device()) {
      success = false;
    }
  });

  return success;
}

static string get_layer_view_name(const RenderBuffers &buffers)
{
  string result;

  if (!buffers.params.layer.empty()) {
    result += string(buffers.params.layer);
  }

  if (!buffers.params.view.empty()) {
    if (!result.empty()) {
      result += ", ";
    }
    result += string(buffers.params.view);
  }

  return result;
}

void PathTrace::process_full_buffer_from_disk(string_view filename)
{
  LOG_DEBUG << "Processing full frame buffer file " << filename;

  progress_set_status("Reading full buffer from disk");

  RenderBuffers full_frame_buffers(cpu_device_.get());

  DenoiseParams denoise_params;
  if (!tile_manager_.read_full_buffer_from_disk(filename, &full_frame_buffers, &denoise_params)) {
    const string error_message = "Error reading tiles from file";
    if (progress_) {
      progress_->set_error(error_message);
      progress_->set_cancel(error_message);
    }
    else {
      LOG_ERROR << error_message;
    }
    return;
  }

  const string layer_view_name = get_layer_view_name(full_frame_buffers);

  render_state_.has_denoised_result = false;

  if (denoise_params.use && denoiser_ && !progress_->get_cancel()) {
    progress_set_status(layer_view_name, "Denoising");

    /* If GPU should be used is not based on file metadata. */
    denoise_params.use_gpu = render_scheduler_.is_denoiser_gpu_used();

    /* Re-use the denoiser as much as possible, avoiding possible device re-initialization.
     *
     * It will not conflict with the regular rendering as:
     *  - Rendering is supposed to be finished here.
     *  - The next rendering will go via Session's `run_update_for_next_iteration` which will
     *    ensure proper denoiser is used. */
    set_denoiser_params(denoise_params);

    /* Number of samples doesn't matter too much, since the samples count pass will be used. */
    denoiser_->denoise_buffer(
        full_frame_buffers.params, full_frame_buffers.params, &full_frame_buffers, 0, false);

    render_state_.has_denoised_result = true;
  }

  full_frame_state_.render_buffers = &full_frame_buffers;

  progress_set_status(layer_view_name, "Finishing");

  /* Write the full result pretending that there is a single tile.
   * Requires some state change, but allows to use same communication API with the software. */
  tile_buffer_write();

  full_frame_state_.render_buffers = nullptr;
}

int PathTrace::get_num_render_tile_samples() const
{
  if (full_frame_state_.render_buffers) {
    return full_frame_state_.render_buffers->params.samples;
  }

  return render_scheduler_.get_num_rendered_samples();
}

bool PathTrace::get_render_tile_pixels(const PassAccessor &pass_accessor,
                                       const PassAccessor::Destination &destination)
{
  if (full_frame_state_.render_buffers) {
    return pass_accessor.get_render_tile_pixels(full_frame_state_.render_buffers, destination);
  }

  if (big_tile_denoise_work_ && render_state_.has_denoised_result) {
    /* Only use the big tile denoised buffer to access the denoised passes.
     * The guiding passes are allowed to be modified in-place for the needs of the denoiser,
     * so copy those from the original devices buffers. */
    if (pass_accessor.get_pass_access_info().mode == PassMode::DENOISED) {
      return big_tile_denoise_work_->get_render_tile_pixels(pass_accessor, destination);
    }
  }

  bool success = true;

  parallel_for_each(path_trace_works_, [&](unique_ptr<PathTraceWork> &path_trace_work) {
    if (!success) {
      return;
    }
    if (!path_trace_work->get_render_tile_pixels(pass_accessor, destination)) {
      success = false;
    }
  });

  return success;
}

bool PathTrace::set_render_tile_pixels(PassAccessor &pass_accessor,
                                       const PassAccessor::Source &source)
{
  bool success = true;

  parallel_for_each(path_trace_works_, [&](unique_ptr<PathTraceWork> &path_trace_work) {
    if (!success) {
      return;
    }
    if (!path_trace_work->set_render_tile_pixels(pass_accessor, source)) {
      success = false;
    }
  });

  return success;
}

int2 PathTrace::get_render_tile_size() const
{
  if (full_frame_state_.render_buffers) {
    return make_int2(full_frame_state_.render_buffers->params.window_width,
                     full_frame_state_.render_buffers->params.window_height);
  }

  const Tile &tile = tile_manager_.get_current_tile();
  return make_int2(tile.window_width, tile.window_height);
}

int2 PathTrace::get_render_tile_offset() const
{
  if (full_frame_state_.render_buffers) {
    return make_int2(0, 0);
  }

  const Tile &tile = tile_manager_.get_current_tile();
  return make_int2(tile.x + tile.window_x, tile.y + tile.window_y);
}

int2 PathTrace::get_render_size() const
{
  return tile_manager_.get_size();
}

const BufferParams &PathTrace::get_render_tile_params() const
{
  if (full_frame_state_.render_buffers) {
    return full_frame_state_.render_buffers->params;
  }

  return big_tile_params_;
}

bool PathTrace::has_denoised_result() const
{
  return render_state_.has_denoised_result;
}

void PathTrace::destroy_gpu_resources()
{
  /* Destroy any GPU resource which was used for graphics interop.
   * Need to have access to the PathTraceDisplay as it is the only source of drawing context which
   * is used for interop. */
  if (display_) {
    for (auto &&path_trace_work : path_trace_works_) {
      path_trace_work->destroy_gpu_resources(display_.get());
    }

    if (big_tile_denoise_work_) {
      big_tile_denoise_work_->destroy_gpu_resources(display_.get());
    }
  }
}

/* --------------------------------------------------------------------
 * Report generation.
 */

static const char *device_type_for_description(const DeviceType type)
{
  switch (type) {
    case DEVICE_NONE:
      return "None";

    case DEVICE_CPU:
      return "CPU";
    case DEVICE_CUDA:
      return "CUDA";
    case DEVICE_OPTIX:
      return "OptiX";
    case DEVICE_HIP:
      return "HIP";
    case DEVICE_HIPRT:
      return "HIPRT";
    case DEVICE_ONEAPI:
      return "oneAPI";
    case DEVICE_DUMMY:
      return "Dummy";
    case DEVICE_MULTI:
      return "Multi";
    case DEVICE_METAL:
      return "Metal";
  }

  return "UNKNOWN";
}

/* Construct description of the device which will appear in the full report. */
/* TODO(sergey): Consider making it more reusable utility. */
static string full_device_info_description(const DeviceInfo &device_info)
{
  string full_description = device_info.description;

  full_description += " (" + string(device_type_for_description(device_info.type)) + ")";

  if (device_info.display_device) {
    full_description += " (display)";
  }

  if (device_info.type == DEVICE_CPU) {
    full_description += " (" + to_string(device_info.cpu_threads) + " threads)";
  }

  full_description += " [" + device_info.id + "]";

  return full_description;
}

/* Construct string which will contain information about devices, possibly multiple of the devices.
 *
 * In the simple case the result looks like:
 *
 *   Message: Full Device Description
 *
 * If there are multiple devices then the result looks like:
 *
 *   Message: Full First Device Description
 *            Full Second Device Description
 *
 * Note that the newlines are placed in a way so that the result can be easily concatenated to the
 * full report. */
static string device_info_list_report(const string &message, const DeviceInfo &device_info)
{
  string result = "\n" + message + ": ";
  const string pad(message.length() + 2, ' ');

  if (device_info.multi_devices.empty()) {
    result += full_device_info_description(device_info) + "\n";
    result += pad +
              "    Hardware Ray-Tracing: " + (device_info.use_hardware_raytracing ? "On" : "Off") +
              "\n";
    return result;
  }

  bool is_first = true;
  for (const DeviceInfo &sub_device_info : device_info.multi_devices) {
    if (!is_first) {
      result += pad;
    }

    result += full_device_info_description(sub_device_info) + "\n";
    result += pad + "    Hardware Ray-Tracing: " +
              (sub_device_info.use_hardware_raytracing ? "On" : "Off") + "\n";

    is_first = false;
  }

  return result;
}

static string path_trace_devices_report(const vector<unique_ptr<PathTraceWork>> &path_trace_works)
{
  DeviceInfo device_info;
  device_info.type = DEVICE_MULTI;

  for (auto &&path_trace_work : path_trace_works) {
    device_info.multi_devices.push_back(path_trace_work->get_device()->info);
  }

  return device_info_list_report("Path tracing on", device_info);
}

static string denoiser_device_report(const Denoiser *denoiser)
{
  if (!denoiser) {
    return "";
  }

  if (!denoiser->get_params().use) {
    return "";
  }

  const Device *denoiser_device = denoiser->get_denoiser_device();
  if (!denoiser_device) {
    return "";
  }

  return device_info_list_report("Denoising on", denoiser_device->info);
}

string PathTrace::full_report() const
{
  string result = "Full path tracing report:\n";

  result += path_trace_devices_report(path_trace_works_);
  result += denoiser_device_report(denoiser_.get());

  /* Report from the render scheduler, which includes:
   * - Render mode (interactive, offline, headless)
   * - Adaptive sampling and denoiser parameters
   * - Breakdown of timing. */
  result += render_scheduler_.full_report();

  return result;
}

void PathTrace::set_guiding_params(const GuidingParams &guiding_params, const bool reset)
{
#if defined(WITH_PATH_GUIDING)
  if (guiding_params_.modified(guiding_params)) {
    guiding_params_ = guiding_params;

#  if !(OPENPGL_VERSION_MAJOR == 0 && OPENPGL_VERSION_MINOR <= 5)
#    define OPENPGL_USE_FIELD_CONFIG
#  endif

    if (guiding_params_.use) {
#  ifdef OPENPGL_USE_FIELD_CONFIG
      openpgl::cpp::FieldConfig field_config;
#  else
      PGLFieldArguments field_args;
#  endif
      switch (guiding_params_.type) {
        default:
        /* Parallax-aware von Mises-Fisher mixture models. */
        case GUIDING_TYPE_PARALLAX_AWARE_VMM: {
#  ifdef OPENPGL_USE_FIELD_CONFIG
          field_config.Init(
              PGL_SPATIAL_STRUCTURE_TYPE::PGL_SPATIAL_STRUCTURE_KDTREE,
              PGL_DIRECTIONAL_DISTRIBUTION_TYPE::PGL_DIRECTIONAL_DISTRIBUTION_PARALLAX_AWARE_VMM,
              guiding_params.deterministic);
#  else
          pglFieldArgumentsSetDefaults(
              field_args,
              PGL_SPATIAL_STRUCTURE_TYPE::PGL_SPATIAL_STRUCTURE_KDTREE,
              PGL_DIRECTIONAL_DISTRIBUTION_TYPE::PGL_DIRECTIONAL_DISTRIBUTION_PARALLAX_AWARE_VMM);
#  endif
          break;
        }
        /* Directional quad-trees. */
        case GUIDING_TYPE_DIRECTIONAL_QUAD_TREE: {
#  ifdef OPENPGL_USE_FIELD_CONFIG
          field_config.Init(
              PGL_SPATIAL_STRUCTURE_TYPE::PGL_SPATIAL_STRUCTURE_KDTREE,
              PGL_DIRECTIONAL_DISTRIBUTION_TYPE::PGL_DIRECTIONAL_DISTRIBUTION_QUADTREE,
              guiding_params.deterministic);
#  else
          pglFieldArgumentsSetDefaults(
              field_args,
              PGL_SPATIAL_STRUCTURE_TYPE::PGL_SPATIAL_STRUCTURE_KDTREE,
              PGL_DIRECTIONAL_DISTRIBUTION_TYPE::PGL_DIRECTIONAL_DISTRIBUTION_QUADTREE);
#  endif
          break;
        }
        /* von Mises-Fisher mixture models. */
        case GUIDING_TYPE_VMM: {
#  ifdef OPENPGL_USE_FIELD_CONFIG
          field_config.Init(PGL_SPATIAL_STRUCTURE_TYPE::PGL_SPATIAL_STRUCTURE_KDTREE,
                            PGL_DIRECTIONAL_DISTRIBUTION_TYPE::PGL_DIRECTIONAL_DISTRIBUTION_VMM,
                            guiding_params.deterministic);
#  else
          pglFieldArgumentsSetDefaults(
              field_args,
              PGL_SPATIAL_STRUCTURE_TYPE::PGL_SPATIAL_STRUCTURE_KDTREE,
              PGL_DIRECTIONAL_DISTRIBUTION_TYPE::PGL_DIRECTIONAL_DISTRIBUTION_VMM);
#  endif
          break;
        }
      }
#  ifdef OPENPGL_USE_FIELD_CONFIG
      field_config.SetSpatialStructureArgMaxDepth(16);
#  else
      field_args.deterministic = guiding_params.deterministic;
      reinterpret_cast<PGLKDTreeArguments *>(field_args.spatialSturctureArguments)->maxDepth = 16;
#  endif
      openpgl::cpp::Device *guiding_device = static_cast<openpgl::cpp::Device *>(
          device_->get_guiding_device());
      if (guiding_device) {
        guiding_sample_data_storage_ = make_unique<openpgl::cpp::SampleStorage>();
#  ifdef OPENPGL_USE_FIELD_CONFIG
        guiding_field_ = make_unique<openpgl::cpp::Field>(guiding_device, field_config);
#  else
        guiding_field_ = make_unique<openpgl::cpp::Field>(guiding_device, field_args);
#  endif
      }
      else {
        guiding_sample_data_storage_ = nullptr;
        guiding_field_ = nullptr;
      }
    }
    else {
      guiding_sample_data_storage_ = nullptr;
      guiding_field_ = nullptr;
    }
  }
  else if (reset) {
    if (guiding_field_) {
      guiding_field_->Reset();
    }
  }
#else
  (void)guiding_params;
  (void)reset;
#endif
}

void PathTrace::guiding_prepare_structures()
{
#if defined(WITH_PATH_GUIDING)
  const bool train = (guiding_params_.training_samples == 0) ||
                     (guiding_field_->GetIteration() < guiding_params_.training_samples);

  for (auto &&path_trace_work : path_trace_works_) {
    path_trace_work->guiding_init_kernel_globals(
        guiding_field_.get(), guiding_sample_data_storage_.get(), train);
  }

  if (train) {
    /* For training the guiding distribution we need to force the number of samples
     * per update to be limited, for reproducible results and reasonable training size.
     *
     * Idea: we could stochastically discard samples with a probability of 1/num_samples_per_update
     * we can then update only after the num_samples_per_update iterations are rendered. */
    render_scheduler_.set_limit_samples_per_update(4);
  }
  else {
    render_scheduler_.set_limit_samples_per_update(0);
  }
#endif
}

void PathTrace::guiding_update_structures()
{
#if defined(WITH_PATH_GUIDING)
  LOG_DEBUG << "Update path guiding structures";

  LOG_TRACE << "Number of surface samples: " << guiding_sample_data_storage_->GetSizeSurface();
  LOG_TRACE << "Number of volume samples: " << guiding_sample_data_storage_->GetSizeVolume();

  const size_t num_valid_samples = guiding_sample_data_storage_->GetSizeSurface() +
                                   guiding_sample_data_storage_->GetSizeVolume();

  /* we wait until we have at least 1024 samples */
  if (num_valid_samples >= 1024) {
    guiding_field_->Update(*guiding_sample_data_storage_);
    guiding_update_count++;

    LOG_TRACE << "Path guiding field valid: " << guiding_field_->Validate();

    guiding_sample_data_storage_->Clear();
  }
#endif
}

CCL_NAMESPACE_END
