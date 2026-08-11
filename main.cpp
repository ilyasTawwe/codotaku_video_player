#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <format>
#include <print>
#include <source_location>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/log.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
}

#include <libplacebo/filters.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/swapchain.h>
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>
#include <libplacebo/vulkan.h>

#include "player.h"
#include "sync.h"

struct SDLException : std::runtime_error {
  SDLException(std::source_location loc = std::source_location::current())
      : std::runtime_error(std::format(
            "{}:{}:{} ({}) SDL Error: {}", loc.file_name(), loc.line(),
            loc.column(), loc.function_name(), SDL_GetError())) {}
};

auto chk(bool result,
         std::source_location loc = std::source_location::current()) -> void {
  if (!result)
    throw SDLException{loc};
}

auto chk_av(int err,
            std::source_location loc = std::source_location::current())
    -> void {
  if (err < 0) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(err, buf, sizeof(buf));
    throw std::runtime_error(std::format(
        "{}:{}:{} ({}) FFmpeg Error: {}", loc.file_name(), loc.line(),
        loc.column(), loc.function_name(), buf));
  }
}

auto fmt_clock(double v) -> std::string {
  return std::isnan(v) ? "n/a" : std::format("{:.3f}", v);
}

struct App {
  SDL_Window *window;
  AVBufferRef *hwdev;
  VkInstance inst;
  VkSurfaceKHR surface;
  pl_log log;
  pl_vulkan vk;
  pl_gpu gpu;
  pl_swapchain swapchain;
  pl_renderer renderer;
  Player player;
  pl_tex frame_tex[4]{};
  SDL_AudioStream *audio_stream = nullptr;

  // FPS counter: rendered frames within a rolling ~2s logging window.
  Uint64 fps_log_ns = 0;
  Uint64 fps_frames = 0;

  // Per-second A/V sync log. Values are NAN until the corresponding clock is
  // wired up in later milestones.
  Uint64 sync_log_ns = 0;
  MediaClock vidclk;
  MediaClock wall;   // set to 0 at startup; verifies the clock primitive
  double sync_delay = NAN;   // delay used for the last displayed frame
  int frame_drops = 0;
  Uint64 audio_frames = 0;
  Uint64 audio_frames_prev = 0;

  // Audio master clock state: end pts (media seconds) of the last audio frame
  // handed to SDL, minus the converted bytes still waiting to be played.
  double audio_pts_end = 0.0;
  int audio_out_bytes_per_sec = 0;

  // Video pacing state (ffplay frame_timer model).
  double frame_timer = 0.0;  // wall time when the current frame is due
  double prev_frame_pts = NAN;
  bool paused = false;
};

auto append_ext(std::string &buf, const char *ext) -> void {
  if (ext == nullptr)
    return;
  if (!buf.empty())
    buf += '+';
  buf += ext;
}

// Master clock: where the audio currently playing is, in media seconds.
// The pts of the last frame handed to SDL, minus the converted output bytes
// that the device has not consumed yet.
auto get_audio_clock(const App &app) -> double {
  if (app.audio_stream == nullptr || app.audio_out_bytes_per_sec <= 0)
    return NAN;
  int avail = SDL_GetAudioStreamAvailable(app.audio_stream);
  if (avail < 0)
    return NAN;
  return app.audio_pts_end -
         static_cast<double>(avail) / app.audio_out_bytes_per_sec;
}

// A frame's presentation timestamp in media seconds.
auto frame_pts_s(const Player &player, const AVFrame *frame) -> double {
  int64_t pts = frame->pts;
  if (pts == AV_NOPTS_VALUE)
    pts = frame->best_effort_timestamp;
  if (pts == AV_NOPTS_VALUE)
    return NAN;
  return pts * av_q2d(player.time_base());
}

// The clock every other clock corrects toward: audio when present, otherwise
// the external/wall clock (wired up in a later milestone).
auto get_master_clock(const App &app) -> double {
  if (app.player.info().has_audio)
    return get_audio_clock(app);
  return clock_get(app.wall);
}

// --- A/V sync tuning, ported from fftools/ffplay.c ---------------------
constexpr double kSyncThresholdMin = 0.04;
constexpr double kSyncThresholdMax = 0.1;
constexpr double kSyncFramedupThreshold = 0.1;
constexpr double kNoSyncThreshold = 10.0;    // above this drift, don't correct
constexpr double kMaxFrameDuration = 3600.0; // pts gap this large is a discontinuity

// How long the next frame should stay on screen, adjusted to pull the video
// clock back toward the master (audio) clock.
auto compute_target_delay(double delay, const App &app) -> double {
  double sync_threshold =
      std::clamp(delay, kSyncThresholdMin, kSyncThresholdMax);
  double diff = clock_get(app.vidclk) - get_master_clock(app);
  if (!std::isnan(diff) && std::fabs(diff) < kNoSyncThreshold) {
    // Video behind: show sooner. Video ahead: hold longer (or double the
    // delay to stretch the frame out).
    if (diff <= -sync_threshold)
      delay = std::max(0.0, delay + diff);
    else if (diff >= sync_threshold && delay > kSyncFramedupThreshold)
      delay += diff;
    else if (diff >= sync_threshold)
      delay = 2.0 * delay;
  }
  return delay;
}

// Duration of the current frame: the pts gap since the previous frame, or the
// nominal frame duration when pts are missing or discontinuous.
auto frame_duration(double prev_pts, double pts, double fallback) -> double {
  if (!std::isnan(prev_pts) && !std::isnan(pts)) {
    double d = pts - prev_pts;
    if (d > 0.0 && d <= kMaxFrameDuration)
      return d;
  }
  return fallback;
}

auto build_vk_dict(const char *const *inst_exts, int num_inst_exts)
    -> AVDictionary * {
  AVDictionary *dict = nullptr;

  std::string inst;
  for (int i = 0; i < num_inst_exts; i++)
    append_ext(inst, inst_exts[i]);
  for (const char *ext :
       {VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
        VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
        VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME})
    append_ext(inst, ext);
  if (!inst.empty())
    chk_av(av_dict_set(&dict, "instance_extensions", inst.c_str(), 0));

  std::string dev;
  append_ext(dev, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  for (int i = 0; i < pl_vulkan_num_recommended_extensions; i++)
    append_ext(dev, pl_vulkan_recommended_extensions[i]);
  chk_av(av_dict_set(&dict, "device_extensions", dev.c_str(), 0));

  return dict;
}

auto create_vulkan(App &app) -> void {
  Uint32 num_ext = 0;
  const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&num_ext);
  if (exts == nullptr)
    throw std::runtime_error(
        std::format("SDL_Vulkan_GetInstanceExtensions failed: {}",
                    SDL_GetError()));

  AVDictionary *dict = build_vk_dict(exts, num_ext);
  chk_av(av_hwdevice_ctx_create(&app.hwdev, AV_HWDEVICE_TYPE_VULKAN, nullptr,
                                dict, 0));
  av_dict_free(&dict);

  auto *dev = reinterpret_cast<AVHWDeviceContext *>(app.hwdev->data);
  auto *hwctx = reinterpret_cast<AVVulkanDeviceContext *>(dev->hwctx);

  if (hwctx->get_proc_addr !=
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(
          SDL_Vulkan_GetVkGetInstanceProcAddr()))
    std::println(stderr,
                 "warning: FFmpeg and SDL use different vkGetInstanceProcAddr, "
                 "surface creation may fail later");

  struct pl_log_params log_params = {
      .log_cb = pl_log_simple,
      .log_priv = stderr,
      .log_level = PL_LOG_WARN,
  };
  app.log = pl_log_create(PL_API_VER, &log_params);

  struct pl_vulkan_import_params params = {
      .instance = hwctx->inst,
      .get_proc_addr = hwctx->get_proc_addr,
      .phys_device = hwctx->phys_dev,
      .device = hwctx->act_dev,
      .extensions = hwctx->enabled_dev_extensions,
      .num_extensions = hwctx->nb_enabled_dev_extensions,
      .queue_graphics = {VK_QUEUE_FAMILY_IGNORED, 0, 0},
      .queue_compute = {VK_QUEUE_FAMILY_IGNORED, 0, 0},
      .queue_transfer = {VK_QUEUE_FAMILY_IGNORED, 0, 0},
      .features = &hwctx->device_features,
  };
  for (int i = 0; i < hwctx->nb_qf; i++) {
    const auto *qf = &hwctx->qf[i];
    if (qf->flags & VK_QUEUE_GRAPHICS_BIT) {
      params.queue_graphics.index = qf->idx;
      params.queue_graphics.count = qf->num;
    }
    if (qf->flags & VK_QUEUE_COMPUTE_BIT) {
      params.queue_compute.index = qf->idx;
      params.queue_compute.count = qf->num;
    }
    if (qf->flags & VK_QUEUE_TRANSFER_BIT) {
      params.queue_transfer.index = qf->idx;
      params.queue_transfer.count = qf->num;
    }
  }

  app.vk = pl_vulkan_import(app.log, &params);
  if (app.vk == nullptr)
    throw std::runtime_error(
        "pl_vulkan_import failed; see the log above for details");
  app.gpu = app.vk->gpu;
}

auto create_swapchain(App &app) -> void {
  auto *dev = reinterpret_cast<AVHWDeviceContext *>(app.hwdev->data);
  auto *hwctx = reinterpret_cast<AVVulkanDeviceContext *>(dev->hwctx);
  app.inst = hwctx->inst;

  chk(SDL_Vulkan_CreateSurface(app.window, app.inst, nullptr, &app.surface));

  struct pl_vulkan_swapchain_params params = {
      .surface = app.surface,
      .present_mode = VK_PRESENT_MODE_MAILBOX_KHR,
  };
  app.swapchain = pl_vulkan_create_swapchain(app.vk, &params);
  if (app.swapchain == nullptr)
    throw std::runtime_error(
        "pl_vulkan_create_swapchain failed; see the log above for details");

  int w = 0;
  int h = 0;
  chk(SDL_GetWindowSizeInPixels(app.window, &w, &h));
  if (!pl_swapchain_resize(app.swapchain, &w, &h))
    throw std::runtime_error(
        std::format("pl_swapchain_resize to {}x{} failed", w, h));

  app.renderer = pl_renderer_create(app.log, app.gpu);
  if (app.renderer == nullptr)
    throw std::runtime_error(
        "pl_renderer_create failed; see the log above for details");
}

auto render_frame(App &app, AVFrame *frame) -> SDL_AppResult {
  struct pl_frame pic {};
  struct pl_avframe_params map_params = {
      .frame = frame,
      .tex = app.frame_tex,
      .map_dovi = true,
  };
  if (!pl_map_avframe_ex(app.gpu, &pic, &map_params)) {
    std::println(stderr, "pl_map_avframe failed");
    return SDL_APP_FAILURE;
  }

  struct pl_swapchain_frame sw {};
  if (!pl_swapchain_start_frame(app.swapchain, &sw)) {
    std::println(stderr, "pl_swapchain_start_frame failed");
    pl_unmap_avframe(app.gpu, &pic);
    return SDL_APP_CONTINUE;
  }

  struct pl_color_space hint {};
  pl_color_space_from_avframe(&hint, frame);
  pl_swapchain_colorspace_hint(app.swapchain, &hint);

  struct pl_frame target {};
  pl_frame_from_swapchain(&target, &sw);
  target.crop = {0, 0, static_cast<float>(sw.fbo->params.w),
                 static_cast<float>(sw.fbo->params.h)};
  pl_rect2df_aspect_copy(&target.crop, &pic.crop, 0.5f);

  struct pl_render_params params = pl_render_default_params;
  params.upscaler = &pl_filter_spline36;
  params.downscaler = &pl_filter_spline36;

  if (!pl_render_image(app.renderer, &pic, &target, &params)) {
    std::println(stderr, "pl_render_image failed");
    pl_unmap_avframe(app.gpu, &pic);
    return SDL_APP_FAILURE;
  }
  pl_unmap_avframe(app.gpu, &pic);

  if (!pl_swapchain_submit_frame(app.swapchain)) {
    std::println(stderr, "pl_swapchain_submit_frame failed");
    return SDL_APP_FAILURE;
  }
  pl_swapchain_swap_buffers(app.swapchain);
  return SDL_APP_CONTINUE;
}

// Convert one decoded audio frame to SDL and queue it on the output stream.
// SDL handles sample-rate, channel, and format conversion internally.
auto push_audio(App &app, AVFrame *frame) -> void {
  app.audio_frames++;

  // Track the end pts (media seconds) of the frame just pushed; the audio
  // master clock derives the current position by subtracting what SDL has not
  // played yet.
  double pts_s = NAN;
  int64_t pts = frame->pts;
  if (pts == AV_NOPTS_VALUE)
    pts = frame->best_effort_timestamp;
  if (pts != AV_NOPTS_VALUE)
    pts_s = pts * av_q2d(app.player.info().audio_time_base);
  if (std::isnan(pts_s))
    pts_s = app.audio_pts_end;
  double dur_s =
      static_cast<double>(frame->nb_samples) / frame->sample_rate;
  app.audio_pts_end = pts_s + dur_s;
  SDL_AudioFormat fmt = SDL_AUDIO_UNKNOWN;
  switch (static_cast<AVSampleFormat>(frame->format)) {
  case AV_SAMPLE_FMT_U8:
    fmt = SDL_AUDIO_U8;
    break;
  case AV_SAMPLE_FMT_S16:
  case AV_SAMPLE_FMT_S16P:
    fmt = SDL_AUDIO_S16;
    break;
  case AV_SAMPLE_FMT_S32:
  case AV_SAMPLE_FMT_S32P:
    fmt = SDL_AUDIO_S32;
    break;
  case AV_SAMPLE_FMT_FLT:
  case AV_SAMPLE_FMT_FLTP:
    fmt = SDL_AUDIO_F32;
    break;
  default:
    std::println(stderr, "unsupported audio sample format: {}",
                 av_get_sample_fmt_name(
                     static_cast<AVSampleFormat>(frame->format)));
    av_frame_free(&frame);
    return;
  }

  SDL_AudioSpec spec = {.format = fmt,
                        .channels = frame->ch_layout.nb_channels,
                        .freq = frame->sample_rate};
  if (!SDL_SetAudioStreamFormat(app.audio_stream, &spec, nullptr)) {
    std::println(stderr, "SDL_SetAudioStreamFormat: {}", SDL_GetError());
    av_frame_free(&frame);
    return;
  }

  bool ok = false;
  if (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format))) {
    ok = SDL_PutAudioStreamPlanarData(
        app.audio_stream,
        reinterpret_cast<const void *const *>(frame->extended_data),
        frame->ch_layout.nb_channels, frame->nb_samples);
  } else {
    int bytes =
        frame->nb_samples *
        av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame->format)) *
        frame->ch_layout.nb_channels;
    ok = SDL_PutAudioStreamData(app.audio_stream, frame->data[0], bytes);
  }
  if (!ok)
    std::println(stderr, "SDL_PutAudioStreamData: {}", SDL_GetError());
  av_frame_free(&frame);
}

auto drain_audio(App &app) -> void {
  if (app.audio_stream == nullptr)
    return;
  while (AVFrame *frame = app.player.take_audio_frame())
    push_audio(app, frame);
}

auto SDL_AppInit(void **appstate, int argc, char **argv) -> SDL_AppResult try {
  auto app = new App{};
  *appstate = app;

  // Only surface actual problems from FFmpeg and libplacebo.
  av_log_set_level(AV_LOG_WARNING);

  chk(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO));

  app->window = SDL_CreateWindow("Codotaku Video Player", 800, 600,
                                 SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN |
                                     SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                     SDL_WINDOW_VULKAN);
  chk(app->window);

  create_vulkan(*app);
  create_swapchain(*app);

  const char *path = argc > 1 ? argv[1] : CODOTAKU_DEFAULT_VIDEO_PATH;
  app->player.open(path, app->hwdev);
  const PlayerInfo &info = app->player.info();
  std::println("opened: {}x{}, {} decoder, hwaccel: {}",
               info.width, info.height, info.codec_name,
               info.hwaccel ? "yes" : "no");
  if (info.duration_us > 0)
    std::println("duration: {:.1f}s", info.duration_us / 1e6);

  if (info.has_audio) {
    app->audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                                  nullptr, nullptr, nullptr);
    chk(app->audio_stream);
    SDL_AudioSpec dst{};
    chk(SDL_GetAudioStreamFormat(app->audio_stream, nullptr, &dst));
    app->audio_out_bytes_per_sec =
        dst.freq * dst.channels * SDL_AUDIO_BYTESIZE(dst.format);
    chk(SDL_ResumeAudioStreamDevice(app->audio_stream));
    std::println("audio: {} channel(s) @ {} Hz -> out {} ch @ {} Hz ({} b/s)",
                 info.audio_channels, info.audio_sample_rate, dst.channels,
                 dst.freq, app->audio_out_bytes_per_sec);
  }

  clock_set_at(app->wall, 0.0, 0, now_s());
  chk(SDL_ShowWindow(app->window));
  return SDL_APP_CONTINUE;
} catch (const std::exception &e) {
  std::println(stderr, "{}", e.what());
  return SDL_APP_FAILURE;
}

auto SDL_AppIterate(void *appstate) -> SDL_AppResult try {
  auto *app = static_cast<App *>(appstate);

  if (app->paused) {
    SDL_Delay(16);
    return SDL_APP_CONTINUE;
  }

  drain_audio(*app);

  double frame_pts = NAN;
  AVFrame *frame = nullptr;
  for (;;) {
    frame = app->player.next_frame();
    if (frame == nullptr) {
      if (app->player.eof()) {
        app->player.rewind();
        if (app->audio_stream != nullptr)
          SDL_ClearAudioStream(app->audio_stream);
        app->audio_pts_end = 0.0;
        app->frame_timer = 0.0; // re-primed below on the first frame
        app->prev_frame_pts = NAN;
        app->vidclk = MediaClock{};
        frame = app->player.next_frame();
      }
      if (frame == nullptr)
        return SDL_APP_SUCCESS;
    }

    // How long this frame should be displayed, corrected toward the master
    // (audio) clock.
    frame_pts = frame_pts_s(app->player, frame);
    double nominal_duration =
        app->player.info().fps > 0.0 ? 1.0 / app->player.info().fps : 1.0 / 60.0;
    double duration =
        frame_duration(app->prev_frame_pts, frame_pts, nominal_duration);
    double delay = compute_target_delay(duration, *app);
    app->sync_delay = delay;

    double time = now_s();
    if (app->frame_timer == 0.0)
      app->frame_timer = time;

    // Drop a frame that is already overdue: displaying it now would keep the
    // video clock stuck behind the audio master. Skipping frames (advancing
    // frame_timer as if it were shown) lets video catch up instead of
    // accumulating drift. Never drop at EOF so the tail always renders.
    if (time > app->frame_timer + duration && !app->player.eof()) {
      app->frame_drops++;
      app->frame_timer += delay;
      if (app->frame_timer < time - kSyncThresholdMax)
        app->frame_timer = time;
      av_frame_free(&frame);
      continue;
    }

    // Wait until the frame is due (ffplay frame_timer pacing).
    if (time < app->frame_timer + delay)
      SDL_Delay(static_cast<int>((app->frame_timer + delay - time) * 1000.0));
    time = now_s();
    app->frame_timer += delay;
    if (delay > 0.0 && time - app->frame_timer > kSyncThresholdMax)
      app->frame_timer = time;
    break;
  }

  SDL_AppResult result = render_frame(*app, frame);

  // The video clock is the pts of the frame currently on screen; it advances
  // with wall time between updates.
  if (!std::isnan(frame_pts))
    clock_set(app->vidclk, frame_pts, 0);
  app->prev_frame_pts = frame_pts;
  av_frame_free(&frame);

  // Log the render throughput every ~2 seconds.
  app->fps_frames++;
  Uint64 now_ns = SDL_GetTicksNS();
  if (app->fps_log_ns == 0)
    app->fps_log_ns = now_ns;
  double elapsed_s =
      static_cast<double>(now_ns - app->fps_log_ns) / SDL_NS_PER_SECOND;
  if (elapsed_s >= 2.0) {
    std::println(stderr, "fps: {:.1f}", app->fps_frames / elapsed_s);
    app->fps_log_ns = now_ns;
    app->fps_frames = 0;
  }

  // Per-second A/V sync diagnostic. Values stay "n/a" until clocks are wired.
  Uint64 sync_now_ns = SDL_GetTicksNS();
  if (app->sync_log_ns == 0)
    app->sync_log_ns = sync_now_ns;
  double sync_elapsed_s =
      static_cast<double>(sync_now_ns - app->sync_log_ns) / SDL_NS_PER_SECOND;
  if (sync_elapsed_s >= 1.0) {
    double afps =
        (app->audio_frames - app->audio_frames_prev) / sync_elapsed_s;
    double master = get_master_clock(*app);
    double video = clock_get(app->vidclk);
    std::println(stderr,
                 "sync: master={} video={} avdiff={:+.0f}ms delay={} "
                 "drops={} aqueue={} afps={:.0f} wall={}",
                 fmt_clock(master), fmt_clock(video),
                 (video - master) * 1000.0, fmt_clock(app->sync_delay),
                 app->frame_drops, app->player.audio_queue_depth(), afps,
                 fmt_clock(clock_get(app->wall)));
    app->audio_frames_prev = app->audio_frames;
    app->sync_log_ns = sync_now_ns;
  }
  return result;
} catch (const std::exception &e) {
  std::println(stderr, "{}", e.what());
  return SDL_APP_FAILURE;
}

auto SDL_AppEvent(void *appstate, SDL_Event *event) -> SDL_AppResult try {
  switch (event->type) {
  case SDL_EVENT_QUIT:
    return SDL_APP_SUCCESS;
  case SDL_EVENT_WINDOW_RESIZED:
  case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
    auto *app = static_cast<App *>(appstate);
    int w = 0;
    int h = 0;
    if (SDL_GetWindowSizeInPixels(app->window, &w, &h))
      pl_swapchain_resize(app->swapchain, &w, &h);
    break;
  }
  case SDL_EVENT_KEY_DOWN:
    if (event->key.key == SDLK_SPACE) {
      auto *app = static_cast<App *>(appstate);
      app->paused = !app->paused;
      // Freeze the video clock while paused so it doesn't keep drifting with
      // wall time; re-prime pacing when playback resumes.
      app->vidclk.paused = app->paused;
      app->frame_timer = 0.0;
      if (app->audio_stream != nullptr) {
        if (app->paused)
          SDL_PauseAudioStreamDevice(app->audio_stream);
        else
          SDL_ResumeAudioStreamDevice(app->audio_stream);
      }
      std::println("{}", app->paused ? "paused" : "playing");
    }
    break;
  default:
    return SDL_APP_CONTINUE;
  }
  return SDL_APP_CONTINUE;
} catch (const std::exception &e) {
  std::println(stderr, "{}", e.what());
  return SDL_APP_FAILURE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) try {
  auto *app = static_cast<App *>(appstate);
  if (app->audio_stream != nullptr)
    SDL_DestroyAudioStream(app->audio_stream);
  if (app->renderer != nullptr)
    pl_renderer_destroy(&app->renderer);
  if (app->gpu != nullptr)
    for (auto &tex : app->frame_tex)
      pl_tex_destroy(app->gpu, &tex);
  if (app->swapchain != nullptr)
    pl_swapchain_destroy(&app->swapchain);
  if (app->surface != VK_NULL_HANDLE)
    SDL_Vulkan_DestroySurface(app->inst, app->surface, nullptr);
  if (app->vk != nullptr)
    pl_vulkan_destroy(&app->vk);
  if (app->log != nullptr)
    pl_log_destroy(&app->log);
  if (app->hwdev != nullptr)
    av_buffer_unref(&app->hwdev);
  delete app;
} catch (const std::exception &e) {
  std::println(stderr, "{}", e.what());
}
