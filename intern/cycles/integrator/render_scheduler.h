/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "integrator/adaptive_sampling.h"
#include "integrator/denoiser.h"

#include "session/buffers.h"

#include "util/string.h"

CCL_NAMESPACE_BEGIN

class SessionParams;
class TileManager;

class RenderWork {
 public:
  float resolution_divider = 1;
  float denoised_resolution_divider = 1;

  /* Initialize render buffers.
   * Includes steps like zeroing the buffer on the device, and optional reading of pixels from the
   * baking target. */
  bool init_render_buffers = false;

  /* The sample count restarted but the frame did not change (a DLSS-RR pre-roll
   * pass), so the temporal history lines up with the frame as it is: keep it,
   * and do not warp it by the frame's motion vectors. */
  bool denoise_same_frame_restart = false;

  /* This pass is a DLSS-RR pre-roll pass whose image is thrown away: only the
   * history it leaves behind is wanted. False on the pass that is kept as the
   * frame. */
  bool denoise_preroll_pass = false;

  /* Path tracing samples information. */
  struct {
    int start_sample = 0;
    int num_samples = 0;
    int sample_offset = 0;
  } path_trace;

  struct {
    /* Check for convergency and filter the mask. */
    bool filter = false;

    float threshold = 0.0f;

    /* Reset convergency flag when filtering, forcing a re-check of whether pixel did converge. */
    bool reset = false;
  } adaptive_sampling;

  struct {
    bool postprocess = false;
  } cryptomatte;

  /* Work related on the current tile. */
  struct {
    /* Write render buffers of the current tile.
     *
     * It is up to the path trace to decide whether writing should happen via user-provided
     * callback into the rendering software, or via tile manager into a partial file. */
    bool write = false;

    bool denoise = false;
  } tile;

  /* Work related on the full-frame render buffer. */
  struct {
    /* Write full render result.
     * Implies reading the partial file from disk. */
    bool write = false;
  } full;

  /* Display which is used to visualize render result. */
  struct {
    /* Display needs to be updated for the new render. */
    bool update = false;

    /* Display can use denoised result if available. */
    bool use_denoised_result = true;
  } display;

  /* Re-balance multi-device scheduling after rendering this work.
   * Note that the scheduler does not know anything about devices, so if there is only a single
   * device used, then it is up for the PathTracer to ignore the balancing. */
  bool rebalance = false;

  /* Perform volume guiding buffer denoise. */
  bool volume_guiding_denoise = false;

  /* Conversion to bool, to simplify checks about whether there is anything to be done for this
   * work. */
  operator bool() const
  {
    return path_trace.num_samples || adaptive_sampling.filter || display.update || tile.denoise ||
           tile.write || full.write;
  }
};

class RenderScheduler {
 public:
  RenderScheduler(TileManager &tile_manager, const SessionParams &params);

  /* Specify whether cryptomatte-related works are to be scheduled. */
  void set_need_schedule_cryptomatte(bool need_schedule_cryptomatte);

  /* Allows to disable work re-balancing works, allowing to schedule as much to a single device
   * as possible. */
  void set_need_schedule_rebalance(bool need_schedule_rebalance);

  bool is_background() const;

  void set_denoiser_params(const DenoiseParams &params);
  bool is_denoiser_gpu_used() const;

  /* Rendering a frame of an animation, which enables the DLSS-RR first-frame
   * history pre-roll. */
  void set_is_animation(bool is_animation);

  /* The viewport is playing the timeline. Playback gives the renderer roughly
   * one display frame's worth of time, which is not enough for a single
   * sampling pass at the navigation resolution, so playback frames are
   * rendered smaller and DLSS upscales the rest of the way. */
  void set_playback(bool playback);

  /* Tell the scheduler that DLSS-RR history was already warmed up by a
   * previous frame of this same animation job, even though this Session (and
   * therefore this RenderScheduler) was just freshly constructed -- e.g.
   * because Persistent Data is off and the engine tears the Session down
   * and rebuilds it between frames. Without this, every frame looks cold and
   * re-runs the pre-roll pass count instead of just the first one. */
  void set_dlss_history_warm();

  /* A cut threw the RR history away, so the next frame starts cold again and
   * wants the pre-roll a second time. */
  void set_dlss_history_cold();

  /* Extra renders of the frame that starts with no DLSS-RR history, used to
   * fill that history with independent estimates before the kept pass (0 when
   * there is nothing to pre-roll). */
  int get_dlss_preroll_passes() const;

  /* Samples the pass currently being rendered stops at: the frame's count,
   * except for DLSS-RR pre-roll passes which may render fewer. */
  int get_pass_num_samples() const;

  /* Default cap on samples per pre-roll pass (0 = the frame's own count). */
  static const int DLSS_PREROLL_SPP_DEFAULT = 0;
  bool use_dlss_stream_final() const;

  /* Whether the DLSS stream mode is running: every update is an independent frame that RR
   * converges through its history, instead of Cycles accumulating into the buffer. */
  bool use_dlss_stream() const;

  /* Extra factor the path trace resolution is divided by while the timeline is
   * playing (1.0 = off). See the definition for why. */
  float playback_upscale_factor() const;

  /* Where in the sample sequence the current stream frame starts. Plays the same role for the
   * stream as preroll_sample_base_ does for the pre-roll passes: without it every stream frame
   * re-renders the same sample indices, so RR is shown the same noise over and over. */
  int stream_sample_base() const;
  static bool dlss_final_denoise_only();

  void set_adaptive_sampling(const AdaptiveSampling &adaptive_sampling);
  bool is_adaptive_sampling_used() const;

  /* Setup parameters defining the sampling range.
   *
   * It is a single function setting up multiple parameters because there are inter-dependencies
   * between these parameters.
   *
   * In simple cases the subset is not used and the given num_samples samples is rendered, and the
   * subset length and offset are ignored.
   *
   * It is possible to render a subset of the overall samples. This is typically used to distribute
   * rendering of a single frame across multiple computers. This subset rendering is  enabled by
   * setting use_sample_subset=true, and giving the desired offset and length of the subset. The
   * subset offset is a 0-based sample index to start sampling from, and the length is the number
   * of samples to render in this subset.
   *
   * When the subset rendering is enabled, num_samples is expected to be set to the overall number
   * of samples to be rendered, and it is internally used to clamp the number of samples rendered
   * by a subset. */
  void set_sample_params(const int num_samples,
                         const bool use_sample_subset,
                         const int sample_subset_offset,
                         const int sample_subset_length);

  /* Number of samples to render, starting from start sample.
   * The scheduler will schedule work in the range of
   * [start_sample, start_sample + num_samples - 1], inclusively. */
  int get_num_samples() const;

  /* For sample subset rendering, extra offset to be added to sample index
   * for the sampling pattern to be shifted. */
  int get_sample_offset() const;

  /* Time limit for the path tracing tasks, in minutes.
   * Zero disables the limit. */
  void set_time_limit(const double time_limit);
  double get_time_limit() const;

  /* Get sample up to which rendering has been done.
   * This is an absolute 0-based value.
   *
   * For example, if start sample is 10 and 5 samples were rendered, then this call will
   * return 14.
   *
   * If there were no samples rendered, then the behavior is undefined. */
  int get_rendered_sample() const;

  /* Get number of samples rendered within the current scheduling session.
   *
   * For example, if start sample is 10 and 5 samples were rendered, then this call will
   * return 5.
   *
   * Note that this is based on the scheduling information. In practice this means that if someone
   * requested for work to render the scheduler considers the work done. */
  int get_num_rendered_samples() const;

  /* Reset scheduler, indicating that rendering will happen from scratch.
   * Resets current rendered state, as well as scheduling information. */
  void reset(const BufferParams &buffer_params);

  /* Reset scheduler upon switching to a next tile.
   * Will keep the same number of samples and full-frame render parameters, but will reset progress
   * and allow schedule renders works from the beginning of the new tile. */
  void reset_for_next_tile();

  /* Reschedule adaptive sampling work when all pixels did converge.
   * If there is nothing else to be done for the adaptive sampling (pixels did converge to the
   * final threshold) then false is returned and the render scheduler will stop scheduling path
   * tracing works. Otherwise will modify the work's adaptive sampling settings to continue with
   * a lower threshold. */
  bool render_work_reschedule_on_converge(RenderWork &render_work);

  /* Reschedule adaptive sampling work when the device is mostly on idle, but not all pixels yet
   * converged.
   * If re-scheduling is not possible (adaptive sampling is happening with the final threshold, and
   * the path tracer is to finish the current pixels) then false is returned. */
  bool render_work_reschedule_on_idle(RenderWork &render_work);

  /* Reschedule work when rendering has been requested to cancel.
   *
   * Will skip all work which is not needed anymore because no more samples will be added (for
   * example, adaptive sampling filtering and convergence check will be skipped).
   * Will enable all work needed to make sure all passes are communicated to the software.
   *
   * NOTE: Should be used before passing work to `PathTrace::render_samples()`. */
  void render_work_reschedule_on_cancel(RenderWork &render_work);

  RenderWork get_render_work();

  /* Report that the path tracer started to work, after scene update and loading kernels. */
  void report_work_begin(const RenderWork &render_work);

  /* Report time (in seconds) which corresponding part of work took. */
  void report_path_trace_time(const RenderWork &render_work, const double time, bool is_cancelled);
  void report_path_trace_occupancy(const RenderWork &render_work, const float occupancy);
  void report_adaptive_filter_time(const RenderWork &render_work,
                                   const double time,
                                   bool is_cancelled);
  void report_denoise_time(const RenderWork &render_work, const double time);
  void report_display_update_time(const RenderWork &render_work, const double time);
  void report_rebalance_time(const RenderWork &render_work,
                             const double time,
                             bool balance_changed);
  void report_volume_guiding_denoise_time(const RenderWork &render_work, const double time);

  /* Generate full multi-line report of the rendering process, including rendering parameters,
   * times, and so on. */
  string full_report() const;

  void set_limit_samples_per_update(const int limit_samples);

 protected:
  /* Check whether all work has been scheduled and time limit was not exceeded.
   *
   * NOTE: Tricky bit: if the time limit was reached the done() is considered to be true, but some
   * extra work needs to be scheduled to denoise and write final result. */
  bool done() const;

  /* Update scheduling state for a newly scheduled work.
   * Takes care of things like checking whether work was ever denoised, tile was written and states
   * like that. */
  void update_state_for_render_work(const RenderWork &render_work);

  /* Returns true if any work was scheduled. */
  bool set_postprocess_render_work(RenderWork *render_work);

  /*  Set work which is to be performed after all tiles has been rendered. */
  void set_full_frame_render_work(RenderWork *render_work);

  /* Update start resolution divider based on the accumulated timing information, preserving nice
   * feeling navigation feel. */
  void update_start_resolution_divider();

  /* Calculate desired update interval in seconds based on the current timings and settings.
   * Will give an interval which provides good feeling updates during viewport navigation. */
  double guess_viewport_navigation_update_interval_in_seconds() const;

  /* Check whether denoising is active during interactive update while resolution divider is not
   * unit. */
  bool is_denoise_active_during_update() const;

  /* Heuristic which aims to give perceptually pleasant update of display interval in a way that at
   * lower samples and near the beginning of rendering, updates happen more often, but with higher
   * number of samples and later in the render, updates happen less often but device occupancy
   * goes higher. */
  double guess_display_update_interval_in_seconds() const;
  double guess_display_update_interval_in_seconds_for_num_samples(
      const int num_rendered_samples) const;
  double guess_display_update_interval_in_seconds_for_num_samples_no_limit(
      int num_rendered_samples) const;

  /* Calculate number of samples which can be rendered within current desired update interval which
   * is calculated by `guess_update_interval_in_seconds()`. */
  int calculate_num_samples_per_update() const;

  /* Get start sample and the number of samples which are to be path traces in the current work. */
  int get_start_sample_to_path_trace() const;
  int get_num_samples_to_path_trace() const;

  /* Calculate how many samples there are to be rendered for the very first path trace after reset.
   */
  int get_num_samples_during_navigation(const int resolution_divider) const;

  /* Whether adaptive sampling convergence check and filter is to happen. */
  bool work_need_adaptive_filter() const;

  /* Calculate threshold for adaptive sampling. */
  float work_adaptive_threshold() const;

  /* Check whether current work needs denoising.
   * Denoising is not needed if the denoiser is not configured, or when denoising is happening too
   * often.
   *
   * The delayed will be true when the denoiser is configured for use, but it was delayed for a
   * later sample, to reduce overhead.
   *
   * ready_to_display will be false if we may have a denoised result that is outdated due to
   * increased samples. */
  bool work_need_denoise(bool &delayed, bool &ready_to_display);

  /* Check whether current work need to update display.
   *
   * The `denoiser_delayed` is what `work_need_denoise()` returned as delayed denoiser flag. */
  bool work_need_update_display(const bool denoiser_delayed);

  /* Check whether it is time to perform rebalancing for the render work, */
  bool work_need_rebalance();

  /* Check whether timing of the given work are usable to store timings in the `first_render_time_`
   * for the resolution divider calculation. */
  bool work_is_usable_for_first_render_estimation(const RenderWork &render_work);

  /* Check whether timing report about the given work need to reset accumulated average time. */
  bool work_report_reset_average(const RenderWork &render_work);

  /* Check whether render time limit has been reached (or exceeded), and if so store related
   * information in the state so that rendering is considered finished, and is possible to report
   * average render time information. */
  void check_time_limit_reached();

  /* Helper class to keep track of task timing.
   *
   * Contains two parts: wall time and average. The wall time is an actual wall time of how long it
   * took to complete all tasks of a type. Is always advanced when PathTracer reports time update.
   *
   * The average time is used for scheduling purposes. It is estimated to be a time of how long it
   * takes to perform task on the final resolution. */
  class TimeWithAverage {
   public:
    void reset()
    {
      total_wall_time_ = 0.0;

      average_time_accumulator_ = 0.0;
      num_average_times_ = 0;

      last_sample_time_ = 0.0;
    }

    void add_wall(const double time)
    {
      total_wall_time_ += time;
    }

    void add_average(const double time, const int num_measurements = 1)
    {
      average_time_accumulator_ += time;
      num_average_times_ += num_measurements;
      last_sample_time_ = time / num_measurements;
    }

    double get_wall() const
    {
      return total_wall_time_;
    }

    double get_average() const
    {
      if (num_average_times_ == 0) {
        return 0;
      }
      return average_time_accumulator_ / num_average_times_;
    }

    double get_last_sample_time() const
    {
      return last_sample_time_;
    }

    void reset_average()
    {
      average_time_accumulator_ = 0.0;
      num_average_times_ = 0;
    }

   protected:
    double total_wall_time_ = 0.0;

    double average_time_accumulator_ = 0.0;
    int num_average_times_ = 0;

    double last_sample_time_ = 0.0;
  };

  struct {
    bool user_is_navigating = false;

    int resolution_divider = 1;

    /* Number of rendered samples on top of the start sample. */
    int num_rendered_samples = 0;

    /* Total samples pushed through the continuous DLSS viewport stream since
     * the last reset. The per-work counter above is rewound for every DLSS
     * update, so this is the only record of overall progress; done() uses it
     * to honor the viewport sample limit for DLSS. */
    int num_dlss_stream_samples = 0;

    /* Sample count at the last DLSS denoise of the accumulating viewport
     * (carry ON). Used to re-denoise only when the buffer changed enough
     * (samples doubled); see work_need_denoise. */
    int last_dlss_denoise_samples = 0;

    /* Point in time the latest PathTraceDisplay work has been scheduled. */
    double last_display_update_time = 0.0;
    /* Value of -1 means display was never updated. */
    int last_display_update_sample = -1;

    /* Point in time at which last rebalance has been performed. */
    double last_rebalance_time = 0.0;

    /* Number of rebalance works which has been requested to be performed.
     * The path tracer might ignore the work if there is a single device rendering. */
    int num_rebalance_requested = 0;

    /* Number of rebalance works handled which did change balance across devices. */
    int num_rebalance_changes = 0;

    bool need_rebalance_at_next_work = false;

    /* Denotes whether the latest performed rebalance work cause an actual rebalance of work across
     * devices. */
    bool last_rebalance_changed = false;

    /* Threshold for adaptive sampling which will be scheduled to work when not using progressive
     * noise floor. */
    float adaptive_sampling_threshold = 0.0f;

    bool last_work_tile_was_denoised = false;
    bool tile_result_was_written = false;
    bool postprocess_work_scheduled = false;
    bool full_frame_work_scheduled = false;
    bool full_frame_was_written = false;

    bool path_trace_finished = false;
    bool time_limit_reached = false;

    /* Time at which rendering started and finished. */
    double start_render_time = 0.0;
    double end_render_time = 0.0;

    /* Measured occupancy of the render devices measured normalized to the number of samples.
     *
     * In a way it is "trailing": when scheduling new work this occupancy is measured when the
     * previous work was rendered. */
    int occupancy_num_samples = 0;
    float occupancy = 1.0f;
  } state_;

  /* Timing of tasks which were performed at the very first render work at 100% of the
   * resolution. This timing information is used to estimate resolution divider for fats
   * navigation. */
  struct {
    double path_trace_per_sample;
    double denoise_time;
    double display_update_time;
  } first_render_time_;

  TimeWithAverage path_trace_time_;
  TimeWithAverage adaptive_filter_time_;
  TimeWithAverage denoise_time_;
  TimeWithAverage display_update_time_;
  TimeWithAverage rebalance_time_;
  TimeWithAverage volume_guiding_denoise_time_;

  /* Whether cryptomatte-related work will be scheduled. */
  bool need_schedule_cryptomatte_ = false;

  /* Whether to schedule device load rebalance works.
   * Rebalancing requires some special treatment for update intervals and such, so if it's known
   * that the rebalance will be ignored (due to single-device rendering i.e.) is better to fully
   * ignore rebalancing logic. */
  bool need_schedule_rebalance_works_ = false;

  /* Path tracing work will be scheduled for samples from within
   * [sample_offset_, sample_offset_ + num_samples_ - 1] range, inclusively. */
  int sample_offset_ = 0;
  int num_samples_ = 0;

  /* Limit in seconds for how long path tracing is allowed to happen.
   * Zero means no limit is applied. */
  double time_limit_ = 0.0;

  /* Headless rendering without interface. */
  bool headless_;

  /* Background (offline) rendering. */
  bool background_;

  /* The viewport is playing the timeline (see set_playback). */
  bool playback_ = false;

  /* This render is a frame of an animation, so the DLSS-RR history from the
   * previous frame carries into this one -- except on the very first frame. */
  bool is_animation_ = false;

  /* No DLSS-RR history has been built yet in this render job: the first frame
   * of an animation gets none of the temporal accumulation that later frames
   * inherit, and comes out visibly noisier. Cleared once a frame has been
   * rendered. */
  bool dlss_history_cold_ = true;
  /* Survives the reset() that runs between the cut being noticed and the frame
   * being scheduled -- that reset would otherwise declare the history warm
   * again, on the strength of the shot we just threw away. */
  bool dlss_history_cut_pending_ = false;

  /* Pre-roll of the first frame: extra renders of it, each from a different
   * part of the sample sequence, that only exist to fill the RR history. */
  int preroll_passes_left_ = 0;
  int preroll_sample_base_ = 0;
  bool preroll_same_frame_restart_ = false;

  /* Pixel size is used to force lower resolution render for final pass. Useful for retina or other
   * types of hi-dpi displays. */
  int pixel_size_ = 1;

  TileManager &tile_manager_;

  BufferParams buffer_params_;
  DenoiseParams denoiser_params_;

  AdaptiveSampling adaptive_sampling_;

  /* Progressively lower adaptive sampling threshold level, keeping the image at a uniform noise
   * level. */
  bool use_progressive_noise_floor_ = false;

  /* Default value for the resolution divider which will be used when there is no render time
   * information available yet.
   * It is also what defines the upper limit of the automatically calculated resolution divider. */
  int default_start_resolution_divider_ = 1;

  /* Initial resolution divider which will be used on render scheduler reset. */
  int start_resolution_divider_ = 0;

  /* Calculate smallest resolution divider which will bring down actual rendering time below the
   * desired one. This call assumes linear dependency of render time from number of pixels
   * (quadratic dependency from the resolution divider): resolution divider of 2 brings render time
   * down by a factor of 4. */
  int calculate_resolution_divider_for_time(const double desired_time, const double actual_time);

  /* If the number of samples per rendering progression should be limited because of path guiding
   * being activated or is still inside its training phase */
  int limit_samples_per_update_ = 0;
};

int calculate_resolution_divider_for_resolution(const int width,
                                                const int height,
                                                const int resolution);

int calculate_resolution_for_divider(const int width,
                                     const int height,
                                     const int resolution_divider);

CCL_NAMESPACE_END
