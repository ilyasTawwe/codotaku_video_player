#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
#include <libplacebo/shaders/custom.h>
#include <libplacebo/swapchain.h>
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>
#include <libplacebo/vulkan.h>

#include "annotations.h"
#include "exporter.h"
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
  // User shaders (mpv-style .glsl/.hook packs) parsed with
  // pl_mpv_user_shader_parse; owned and destroyed on quit.
  std::vector<const pl_hook *> hooks;
  bool shaders_enabled = true;  // toggled at runtime with the 's' key
  SDL_AudioStream *audio_stream = nullptr;

  // FPS counter: rendered frames within a rolling ~2s logging window.
  Uint64 fps_log_ns = 0;
  Uint64 fps_frames = 0;

  // Per-second A/V sync log (audio master when present, wall clock otherwise).
  Uint64 sync_log_ns = 0;
  MediaClock vidclk;
  MediaClock wall;   // external/wall clock: master for files without audio
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
  // When a seek lands while paused, one frame is decoded and shown so the new
  // position is visible without resuming playback.
  bool pending_seek_redraw = false;

  // --- Annotations --------------------------------------------------------
  // Timeline of user drawings; committed items persist between frames until
  // the user removes them (which records the removal timestamp for export).
  AnnoStore annotations;
  AnnoRaster raster;
  // Active editing tool; nullopt == cursor mode (left-click seeks).
  std::optional<AnnoShape> tool;
  int color_idx = 0;
  bool drawing = false;             // left-drag in progress
  AnnoPoint draw_anchor{};          // drag start (normalized video coords)
  AnnoPoint draw_cur{};             // current mouse pos (normalized video coords)
  std::vector<AnnoPoint> draw_pts;  // freehand polyline being drawn
  std::string text_buffer;          // pending text for the Text tool

  // GPU overlay pipeline: video is composited (with shaders) into an offscreen
  // texture, the CPU-rasterized annotation overlay is uploaded on top of it,
  // and a final pass blends the two into the swapchain FBO.
  pl_tex video_tex = nullptr;
  pl_tex overlay_tex = nullptr;
  pl_pass blend_pass = nullptr;
  int overlay_w = 0;
  int overlay_h = 0;
  pl_fmt overlay_video_fmt = nullptr;
  // Last aspect-corrected video display rectangle, in window pixels (y-down).
  // Used to map mouse input to normalized video coordinates.
  pl_rect2df video_rect{};
  // Set whenever annotation state changes; the paused branch of SDL_AppIterate
  // consumes it to re-blend the overlay onto the last composited video frame.
  bool overlay_dirty = false;

  // --- Export -------------------------------------------------------------
  // The exporter is non-null while an export is running; SDL_AppIterate then
  // drives decode/encode exclusively (no playback pacing or audio output).
  // The pipeline mirrors the overlay path: video is composited into
  // `export_tex`, the CPU raster is uploaded to `export_overlay_tex`, a blend
  // pass merges them into `export_out_tex`, which is read back and encoded.
  std::unique_ptr<Exporter> exporter;
  pl_tex export_tex = nullptr;
  pl_tex export_overlay_tex = nullptr;
  pl_tex export_out_tex = nullptr;
  pl_pass export_blend_pass = nullptr;
  int export_w = 0;
  int export_h = 0;
  AnnoRaster export_raster;  // annotation raster at video resolution
  std::vector<uint8_t> export_rgba;  // readback buffer (w*h*4)
  double export_start_wall = 0.0;    // wall time when export started
  Uint64 export_log_ns = 0;
  int64_t export_frames = 0;
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

// Load an mpv-style user shader (e.g. Anime4K / RAVU packs) and register its
// hooks for the compositor. Prints any tunable parameters it exports.
auto load_shader(App &app, const char *path) -> bool {
  FILE *f = nullptr;
#ifdef _WIN32
  if (fopen_s(&f, path, "rb") != 0)
    f = nullptr;
#else
  f = fopen(path, "rb");
#endif
  if (f == nullptr) {
    std::println(stderr, "could not open shader: {}", path);
    return false;
  }
  std::string text;
  char buf[8192];
  for (size_t n = fread(buf, 1, sizeof(buf), f); n > 0;
       n = fread(buf, 1, sizeof(buf), f))
    text.append(buf, n);
  fclose(f);

  const pl_hook *hook =
      pl_mpv_user_shader_parse(app.gpu, text.c_str(), text.size());
  if (hook == nullptr) {
    std::println(stderr, "failed to parse shader: {}", path);
    return false;
  }
  app.hooks.push_back(hook);
  std::println("shader: {} ({} tunable parameter(s))", path,
               hook->num_parameters);
  for (int i = 0; i < hook->num_parameters; i++) {
    const pl_hook_par *par = &hook->parameters[i];
    std::println("  {}: {}", par->name,
                 par->description != nullptr ? par->description : "");
  }
  return true;
}

// --- Annotation overlay pipeline ----------------------------------------
// The video (with shaders) is composited into an offscreen texture, the
// CPU-rasterized annotation overlay is uploaded as a second texture, and a
// final pass blends the two into the swapchain FBO. This keeps annotation
// geometry in normalized video space (resolution-independent) and leaves a
// clean seam for the future export feature, which can run the same two passes
// into an offscreen texture instead of the swapchain.

constexpr float kAnnoColors[][3] = {
    {1.00f, 0.30f, 0.30f}, {0.35f, 1.00f, 0.45f}, {0.35f, 0.65f, 1.00f},
    {1.00f, 0.85f, 0.25f}, {1.00f, 0.55f, 0.95f}, {0.35f, 1.00f, 1.00f},
    {1.00f, 1.00f, 1.00f}, {0.90f, 0.45f, 0.20f},
};

auto anno_color(const App &app) -> const float * {
  return kAnnoColors[app.color_idx %
                     (sizeof(kAnnoColors) / sizeof(kAnnoColors[0]))];
}

// Media time used to anchor annotations (appear time / removal time).
auto anno_time(const App &app) -> double {
  double t = get_master_clock(app);
  if (std::isnan(t))
    t = clock_get(app.vidclk);
  return std::isnan(t) ? 0.0 : t;
}

auto tool_name(AnnoShape s) -> std::string_view {
  switch (s) {
    case AnnoShape::Rect: return "rect";
    case AnnoShape::Ellipse: return "ellipse";
    case AnnoShape::Arrow: return "arrow";
    case AnnoShape::Freehand: return "freehand";
    case AnnoShape::Text: return "text";
  }
  return "?";
}

// Map a window-space point (pixels, y-down) to normalized video coordinates.
auto window_to_anno(const App &app, float x, float y) -> AnnoPoint {
  float rw = app.video_rect.x1 - app.video_rect.x0;
  float rh = app.video_rect.y1 - app.video_rect.y0;
  if (rw <= 0.0f || rh <= 0.0f)
    return {};
  AnnoPoint p;
  p.x = std::clamp((x - app.video_rect.x0) / rw, 0.0f, 1.0f);
  p.y = std::clamp((y - app.video_rect.y0) / rh, 0.0f, 1.0f);
  return p;
}

struct OverlayVtx {
  float x;
  float y;
  float _pad[2];  // keep the vertex stride a multiple of any GPU alignment
};

// Create the annotation blend pass: samples the composited video texture and
// the CPU-rasterized overlay texture and merges them into a `target_format`
// render target. Shared by the on-screen and export pipelines so both produce
// byte-identical blending.
auto create_blend_pass(pl_gpu gpu, pl_fmt target_format) -> pl_pass {
  static const char *kVertexShader = R"glsl(
#version 450
layout(location = 0) in vec2 pos;
void main() {
  gl_Position = vec4(pos, 0.0, 1.0);
}
)glsl";
  static const char *kFragmentShader = R"glsl(
#version 450
layout(binding = 0) uniform sampler2D video_tex;
layout(binding = 1) uniform sampler2D overlay_tex;
layout(push_constant) uniform Push {
  vec2 resolution;
} push;
layout(location = 0) out vec4 out_color;
void main() {
  vec2 uv = gl_FragCoord.xy / push.resolution;
  vec4 video = texture(video_tex, uv);
  vec4 ov = texture(overlay_tex, uv);
  vec3 rgb = video.rgb * (1.0 - ov.a) + ov.rgb * ov.a;
  out_color = vec4(rgb, video.a);
}
)glsl";

  pl_fmt pos_fmt =
      pl_find_fmt(gpu, PL_FMT_FLOAT, 2, 32, 0, PL_FMT_CAP_VERTEX);
  if (pos_fmt == nullptr)
    return nullptr;
  struct pl_vertex_attrib va = {
      .name = "pos",
      .fmt = pos_fmt,
      .offset = 0,
      .location = 0,
  };
  struct pl_desc descs[2] = {
      {.name = "video_tex", .type = PL_DESC_SAMPLED_TEX, .binding = 0},
      {.name = "overlay_tex", .type = PL_DESC_SAMPLED_TEX, .binding = 1},
  };
  struct pl_pass_params pp = {};
  pp.type = PL_PASS_RASTER;
  pp.descriptors = descs;
  pp.num_descriptors = 2;
  pp.push_constants_size = sizeof(float) * 2;
  pp.glsl_shader = kFragmentShader;
  pp.vertex_type = PL_PRIM_TRIANGLE_LIST;
  pp.vertex_attribs = &va;
  pp.num_vertex_attribs = 1;
  pp.vertex_stride = sizeof(OverlayVtx);
  pp.vertex_shader = kVertexShader;
  pp.target_format = target_format;
  pp.load_target = false;
  return pl_pass_create(gpu, &pp);
}

// Upload `raster` to `overlay_tex` and run the blend pass merging `video_tex`
// over it into `target`.
auto run_overlay_blend(App &app, pl_pass pass, pl_tex video_tex,
                       pl_tex overlay_tex, pl_tex target,
                       AnnoRaster &raster) -> void {
  struct pl_tex_transfer_params upload = {};
  upload.tex = overlay_tex;
  upload.row_pitch = static_cast<size_t>(raster.w) * 4;
  upload.ptr = raster.buf.data();
  pl_tex_upload(app.gpu, &upload);

  static const OverlayVtx kTri[3] = {
      {-1.0f, -1.0f, {}}, {3.0f, -1.0f, {}}, {-1.0f, 3.0f, {}},
  };
  struct pl_desc_binding bindings[2] = {
      {.object = video_tex, .address_mode = PL_TEX_ADDRESS_CLAMP,
       .sample_mode = PL_TEX_SAMPLE_NEAREST},
      {.object = overlay_tex, .address_mode = PL_TEX_ADDRESS_CLAMP,
       .sample_mode = PL_TEX_SAMPLE_NEAREST},
  };
  float resolution[2] = {static_cast<float>(raster.w),
                         static_cast<float>(raster.h)};
  struct pl_pass_run_params run = {};
  run.pass = pass;
  run.desc_bindings = bindings;
  run.push_constants = resolution;
  run.target = target;
  run.vertex_data = kTri;
  run.vertex_count = 3;
  pl_pass_run(app.gpu, &run);
}

// (Re)create the offscreen video texture, the overlay texture, and the blend
// pass whenever the swapchain changes size or format.
auto ensure_overlay_resources(App &app, pl_fmt fbo_fmt, int w, int h) -> void {
  if (app.video_tex != nullptr && app.overlay_w == w && app.overlay_h == h &&
      app.overlay_video_fmt == fbo_fmt)
    return;
  std::println(stderr, "annotation: recreating overlay resources ({}x{} fmt={})",
               w, h, fbo_fmt != nullptr ? fbo_fmt->name : "?");

  if (app.video_tex != nullptr)
    pl_tex_destroy(app.gpu, &app.video_tex);
  if (app.overlay_tex != nullptr)
    pl_tex_destroy(app.gpu, &app.overlay_tex);
  if (app.blend_pass != nullptr)
    pl_pass_destroy(app.gpu, &app.blend_pass);
  app.video_tex = nullptr;
  app.overlay_tex = nullptr;
  app.blend_pass = nullptr;
  app.overlay_w = w;
  app.overlay_h = h;
  app.overlay_video_fmt = fbo_fmt;

  // Same format as the swapchain FBO so the colors rendered by libplacebo
  // stay byte-identical to the old direct-to-FBO path. The renderer clears the
  // letterbox border of the target via a blit, so the format also needs
  // PL_FMT_CAP_BLITTABLE.
  auto video_fmt_ok = [](pl_fmt f) {
    constexpr int need = PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE |
                         PL_FMT_CAP_BLITTABLE;
    return f != nullptr && (f->caps & need) == need;
  };
  pl_fmt video_fmt = nullptr;
  if (video_fmt_ok(fbo_fmt))
    video_fmt = fbo_fmt;
  if (!video_fmt_ok(video_fmt))
    video_fmt = pl_find_named_fmt(app.gpu, "rgba16f");
  if (!video_fmt_ok(video_fmt))
    video_fmt = pl_find_named_fmt(app.gpu, "rgba8");
  if (!video_fmt_ok(video_fmt)) {
    std::println(stderr, "annotation: no renderable/blittable video format");
    return;
  }
  struct pl_tex_params vp = {};
  vp.w = w;
  vp.h = h;
  vp.format = video_fmt;
  vp.sampleable = true;
  vp.renderable = true;
  vp.blit_dst = true;
  app.video_tex = pl_tex_create(app.gpu, &vp);

  pl_fmt ov_fmt = pl_find_named_fmt(app.gpu, "rgba8");
  if (ov_fmt == nullptr || !(ov_fmt->caps & PL_FMT_CAP_SAMPLEABLE)) {
    std::println(stderr, "annotation: no sampleable RGBA format");
    return;
  }
  struct pl_tex_params op = {};
  op.w = w;
  op.h = h;
  op.format = ov_fmt;
  op.sampleable = true;
  op.host_writable = true;
  app.overlay_tex = pl_tex_create(app.gpu, &op);

  if (app.video_tex == nullptr || app.overlay_tex == nullptr) {
    std::println(stderr, "annotation: failed to create overlay textures");
    return;
  }

  pl_fmt target_fmt = fbo_fmt;
  if (target_fmt == nullptr || !(target_fmt->caps & PL_FMT_CAP_RENDERABLE))
    target_fmt = video_fmt;
  app.blend_pass = create_blend_pass(app.gpu, target_fmt);
  if (app.blend_pass == nullptr)
    std::println(stderr, "annotation: failed to create blend pass");
}

// Upload the rasterized overlay and blend it over the video into `target`.
auto blend_overlay(App &app, pl_tex target) -> void {
  if (app.blend_pass == nullptr || app.video_tex == nullptr ||
      app.overlay_tex == nullptr)
    return;

  double t = anno_time(app);
  app.raster.resize(app.overlay_w, app.overlay_h);
  app.raster.set_display_rect(
      static_cast<int>(app.video_rect.x0), static_cast<int>(app.video_rect.y0),
      static_cast<int>(std::ceil(app.video_rect.x1 - app.video_rect.x0)),
      static_cast<int>(std::ceil(app.video_rect.y1 - app.video_rect.y0)));
  app.raster.clear();
  for (const Annotation &a : app.annotations.items)
    if (a.visible_at(t))
      app.raster.draw_annotation(a);

  if (app.tool.has_value()) {
    Annotation preview{};
    preview.shape = *app.tool;
    const float *c = anno_color(app);
    preview.color[0] = c[0];
    preview.color[1] = c[1];
    preview.color[2] = c[2];
    preview.color[3] = 0.6f;
    if (*app.tool == AnnoShape::Text) {
      // Live preview of the pending text at the mouse cursor, where the
      // next click will place it.
      if (!app.text_buffer.empty()) {
        float mx = 0.0f;
        float my = 0.0f;
        SDL_GetMouseState(&mx, &my);
        preview.pts = {window_to_anno(app, mx, my)};
        preview.text = app.text_buffer;
        app.raster.draw_annotation(preview);
      }
    } else if (app.drawing) {
      preview.pts = *app.tool == AnnoShape::Freehand
                        ? app.draw_pts
                        : std::vector<AnnoPoint>{app.draw_anchor, app.draw_cur};
      preview.text = app.text_buffer;
      app.raster.draw_annotation(preview);
    }
  }

  run_overlay_blend(app, app.blend_pass, app.video_tex, app.overlay_tex,
                    target, app.raster);
}

// Re-render just the annotation overlay while paused. `video_tex` already
// holds the last composited video frame, so only the blend pass needs to run
// again — no decode is required.
auto redraw_overlay(App &app) -> void {
  if (app.video_tex == nullptr || app.overlay_tex == nullptr ||
      app.blend_pass == nullptr)
    return;
  struct pl_swapchain_frame sw {};
  if (!pl_swapchain_start_frame(app.swapchain, &sw)) {
    std::println(stderr, "pl_swapchain_start_frame failed");
    return;
  }
  // If the window was resized while paused, the cached overlay textures no
  // longer match the swapchain FBO; leave the redraw to the next render_frame
  // (triggered by the resize handler) instead of blending a stale-size overlay.
  if (sw.fbo->params.w == app.overlay_w && sw.fbo->params.h == app.overlay_h)
    blend_overlay(app, sw.fbo);
  if (!pl_swapchain_submit_frame(app.swapchain)) {
    std::println(stderr, "pl_swapchain_submit_frame failed");
    return;
  }
  pl_swapchain_swap_buffers(app.swapchain);
}

// Composite a decoded frame into an already-resolved target frame (e.g. the
// current swapchain FBO). This is the seam between compositing and
// presentation: everything libplacebo needs is passed as data, so the same
// call can later drive offscreen targets for export. The target's crop must
// span the full output area; unless `aspect_fit` is false, it is replaced
// with the aspect-corrected video rect here. With aspect_fit=false the full
// coded frame maps to the whole target (square export at source resolution).
auto composite_frame(App &app, AVFrame *frame, pl_frame *target,
                     const struct pl_render_params *params,
                     bool aspect_fit = true) -> bool {
  struct pl_frame pic {};
  struct pl_avframe_params map_params = {
      .frame = frame,
      .tex = app.frame_tex,
      .map_dovi = true,
  };
  if (!pl_map_avframe_ex(app.gpu, &pic, &map_params)) {
    std::println(stderr, "pl_map_avframe failed");
    return false;
  }

  if (aspect_fit)
    pl_rect2df_aspect_copy(&target->crop, &pic.crop, 0.5f);

  bool ok = pl_render_image(app.renderer, &pic, target, params);
  pl_unmap_avframe(app.gpu, &pic);
  if (!ok)
    std::println(stderr, "pl_render_image failed");
  return ok;
}

// --- Export ---------------------------------------------------------------
auto destroy_export_resources(App &app) -> void;
auto seek_app(App &app, double seconds) -> void;
auto finish_export(App &app) -> void;
auto cancel_export(App &app) -> void;

// (Re)create the offscreen render target, overlay texture, readback target,
// and blend pass used to bake annotations into the exported video.
auto ensure_export_resources(App &app, int w, int h) -> bool {
  if (app.export_tex != nullptr && app.export_w == w && app.export_h == h)
    return true;

  destroy_export_resources(app);
  app.export_w = w;
  app.export_h = h;

  pl_fmt rgba8 = pl_find_named_fmt(app.gpu, "rgba8");
  constexpr int kNeed = PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE |
                        PL_FMT_CAP_HOST_READABLE;
  if (rgba8 == nullptr || (rgba8->caps & kNeed) != kNeed) {
    std::println(stderr, "export: rgba8 lacks render/sample/readback caps");
    return false;
  }

  struct pl_tex_params vp = {};
  vp.w = w;
  vp.h = h;
  vp.format = rgba8;
  vp.sampleable = true;
  vp.renderable = true;
  vp.blit_dst = true;  // renderer may clear the target border via a blit
  app.export_tex = pl_tex_create(app.gpu, &vp);

  struct pl_tex_params op = {};
  op.w = w;
  op.h = h;
  op.format = rgba8;
  op.sampleable = true;
  op.host_writable = true;
  app.export_overlay_tex = pl_tex_create(app.gpu, &op);

  struct pl_tex_params rp = {};
  rp.w = w;
  rp.h = h;
  rp.format = rgba8;
  rp.renderable = true;
  rp.sampleable = true;
  rp.host_readable = true;
  app.export_out_tex = pl_tex_create(app.gpu, &rp);

  app.export_blend_pass = create_blend_pass(app.gpu, rgba8);

  if (app.export_tex == nullptr || app.export_overlay_tex == nullptr ||
      app.export_out_tex == nullptr || app.export_blend_pass == nullptr) {
    std::println(stderr, "export: failed to create offscreen resources");
    destroy_export_resources(app);
    return false;
  }
  app.export_raster.resize(w, h);
  app.export_raster.set_display_rect(0, 0, w, h);
  app.export_rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
  return true;
}

auto destroy_export_resources(App &app) -> void {
  if (app.gpu == nullptr)
    return;
  if (app.export_tex != nullptr)
    pl_tex_destroy(app.gpu, &app.export_tex);
  if (app.export_overlay_tex != nullptr)
    pl_tex_destroy(app.gpu, &app.export_overlay_tex);
  if (app.export_out_tex != nullptr)
    pl_tex_destroy(app.gpu, &app.export_out_tex);
  if (app.export_blend_pass != nullptr)
    pl_pass_destroy(app.gpu, &app.export_blend_pass);
  app.export_tex = nullptr;
  app.export_overlay_tex = nullptr;
  app.export_out_tex = nullptr;
  app.export_blend_pass = nullptr;
  app.export_w = 0;
  app.export_h = 0;
}

auto start_export(App &app) -> void {
  if (app.exporter != nullptr) {
    std::println(stderr, "export: already in progress");
    return;
  }
  const PlayerInfo &info = app.player.info();
  if (info.width <= 0 || info.height <= 0) {
    std::println(stderr, "export: no video loaded");
    return;
  }

  std::string path = std::format("export_{}.mp4",
                                 static_cast<int64_t>(now_s()));
  auto exp = std::make_unique<Exporter>();
  try {
    exp->open(path.c_str(), info.width, info.height,
              info.fps > 0.0 ? info.fps : 30.0, info.sar,
              info.audio_time_base, app.player.audio_codecpar());
  } catch (const std::exception &e) {
    std::println(stderr, "export: failed to start: {}", e.what());
    return;
  }
  if (exp->has_audio())
    app.player.set_audio_capture(true);
  app.exporter = std::move(exp);
  app.export_frames = 0;
  app.export_log_ns = 0;
  app.export_start_wall = now_s();

  // Freeze playback: pause the audio device and rewind. Audio capture (when the
  // export has an audio track) is enabled before the seek so the demux thread
  // routes packets from t=0.
  if (app.audio_stream != nullptr)
    SDL_PauseAudioStreamDevice(app.audio_stream);
  app.paused = true;
  seek_app(app, 0.0);

  std::println("export: {}x{} @ {:.2f} fps -> {} (audio: {})", info.width,
               info.height, info.fps, path,
               app.exporter->has_audio() ? "AAC re-encode" : "video-only");
}

// Composite one decoded frame into the export target, read it back, and hand
// it to the encoder. Runs once per SDL_AppIterate while exporting.
auto pump_export(App &app) -> SDL_AppResult {
  AVFrame *frame = app.player.next_frame();
  if (frame == nullptr) {
    if (app.player.eof()) {
      app.exporter->finish();
      finish_export(app);
    }
    return SDL_APP_CONTINUE;
  }

  const PlayerInfo &info = app.player.info();
  if (!ensure_export_resources(app, info.width, info.height)) {
    av_frame_free(&frame);
    cancel_export(app);
    return SDL_APP_CONTINUE;
  }

  // Pass 1: video (+ shaders) -> export_tex, matching the display path but
  // targeting BT.709/BT.1886 8-bit SDR so the baked colors are correct.
  struct pl_frame target = {};
  target.planes[0].texture = app.export_tex;
  target.planes[0].flipped = false;
  target.planes[0].components = 4;
  target.planes[0].component_mapping[0] = 0;
  target.planes[0].component_mapping[1] = 1;
  target.planes[0].component_mapping[2] = 2;
  target.planes[0].component_mapping[3] = 3;
  target.num_planes = 1;
  target.crop = {0.0f, 0.0f, static_cast<float>(info.width),
                 static_cast<float>(info.height)};
  target.repr.sys = PL_COLOR_SYSTEM_RGB;
  target.repr.levels = PL_COLOR_LEVELS_PC;
  target.repr.alpha = PL_ALPHA_NONE;
  target.repr.bits = {8, 8, 0};
  target.color.primaries = PL_COLOR_PRIM_BT_709;
  target.color.transfer = PL_COLOR_TRC_BT_1886;

  struct pl_render_params params = pl_render_default_params;
  params.upscaler = &pl_filter_spline36;
  params.downscaler = &pl_filter_spline36;
  if (app.shaders_enabled) {
    params.hooks = app.hooks.empty() ? nullptr : app.hooks.data();
    params.num_hooks = static_cast<int>(app.hooks.size());
  }

  if (!composite_frame(app, frame, &target, &params, false)) {
    std::println(stderr, "export: composite failed");
    av_frame_free(&frame);
    cancel_export(app);
    return SDL_APP_CONTINUE;
  }

  // Annotations anchored to this frame's media time (the playback clocks are
  // frozen during export, so use the decoded frame's own timestamp).
  double t = frame_pts_s(app.player, frame);
  if (std::isnan(t))
    t = info.fps > 0.0 ? app.export_frames / info.fps : 0.0;
  app.export_raster.clear();
  for (const Annotation &a : app.annotations.items)
    if (a.visible_at(t))
      app.export_raster.draw_annotation(a);

  // Pass 2: blend overlay over video into export_out_tex.
  run_overlay_blend(app, app.export_blend_pass, app.export_tex,
                    app.export_overlay_tex, app.export_out_tex,
                    app.export_raster);

  struct pl_tex_transfer_params dl = {};
  dl.tex = app.export_out_tex;
  dl.row_pitch = static_cast<size_t>(app.export_w) * 4;
  dl.ptr = app.export_rgba.data();
  if (!pl_tex_download(app.gpu, &dl)) {
    std::println(stderr, "export: pl_tex_download failed");
    av_frame_free(&frame);
    cancel_export(app);
    return SDL_APP_CONTINUE;
  }

  int64_t pts = frame->pts;
  if (pts == AV_NOPTS_VALUE)
    pts = frame->best_effort_timestamp;
  app.exporter->push_video(app.export_rgba.data(),
                           static_cast<size_t>(app.export_w) * 4, pts,
                           app.player.time_base());

  // Mux captured audio packets through; dispose of any decoded audio frames
  // still in flight so the audio path can't backpressure the demux thread.
  while (AVPacket *pkt = app.player.take_audio_packet())
    app.exporter->push_audio(pkt);
  while (AVFrame *af = app.player.take_audio_frame())
    av_frame_free(&af);

  app.export_frames++;
  av_frame_free(&frame);

  Uint64 now_ns = SDL_GetTicksNS();
  if (app.export_log_ns == 0)
    app.export_log_ns = now_ns;
  double elapsed_s =
      static_cast<double>(now_ns - app.export_log_ns) / SDL_NS_PER_SECOND;
  if (elapsed_s >= 2.0) {
    double media = info.fps > 0.0 ? app.export_frames / info.fps : 0.0;
    double wall = now_s() - app.export_start_wall;
    std::println(stderr, "export: {} frames, {:.1f}s media in {:.1f}s wall "
                         "({:.2f}x)",
                 app.export_frames, media, wall, wall > 0.0 ? wall / media : 0.0);
    app.export_log_ns = now_ns;
  }
  return SDL_APP_CONTINUE;
}

// End an export: write the trailer, tear down the exporter/resources, rewind
// to the start, and resume normal playback.
auto finish_export(App &app) -> void {
  if (app.exporter == nullptr)
    return;
  std::string path = app.exporter->path();
  app.exporter->finish();
  app.exporter.reset();
  app.player.set_audio_capture(false);
  if (app.audio_stream != nullptr)
    SDL_ResumeAudioStreamDevice(app.audio_stream);
  app.paused = false;
  destroy_export_resources(app);
  seek_app(app, 0.0);
  std::println("export: finished -> {} ({} frames)", path, app.export_frames);
}

// Abort an export: delete the partial file and resume playback.
auto cancel_export(App &app) -> void {
  if (app.exporter == nullptr)
    return;
  std::string path = app.exporter->path();
  app.exporter->cancel();
  app.exporter.reset();
  app.player.set_audio_capture(false);
  if (app.audio_stream != nullptr)
    SDL_ResumeAudioStreamDevice(app.audio_stream);
  app.paused = false;
  destroy_export_resources(app);
  seek_app(app, 0.0);
  std::println("export: cancelled -> {}", path);
}

auto render_frame(App &app, AVFrame *frame) -> SDL_AppResult {
  struct pl_swapchain_frame sw {};
  if (!pl_swapchain_start_frame(app.swapchain, &sw)) {
    std::println(stderr, "pl_swapchain_start_frame failed");
    return SDL_APP_CONTINUE;
  }

  struct pl_color_space hint {};
  pl_color_space_from_avframe(&hint, frame);
  pl_swapchain_colorspace_hint(app.swapchain, &hint);

  struct pl_frame target {};
  pl_frame_from_swapchain(&target, &sw);
  target.crop = {0, 0, static_cast<float>(sw.fbo->params.w),
                 static_cast<float>(sw.fbo->params.h)};

  struct pl_render_params params = pl_render_default_params;
  params.upscaler = &pl_filter_spline36;
  params.downscaler = &pl_filter_spline36;
  if (app.shaders_enabled) {
    params.hooks = app.hooks.empty() ? nullptr : app.hooks.data();
    params.num_hooks = static_cast<int>(app.hooks.size());
  }

  int w = sw.fbo->params.w;
  int h = sw.fbo->params.h;
  ensure_overlay_resources(app, sw.fbo->params.format, w, h);

  if (app.video_tex == nullptr || app.overlay_tex == nullptr) {
    // Fallback: composite straight into the swapchain FBO (no overlay).
    if (!composite_frame(app, frame, &target, &params))
      return SDL_APP_FAILURE;
    app.video_rect = target.crop;
  } else {
    // Pass 1: video + shaders -> offscreen texture.
    struct pl_frame vtarget = target;
    vtarget.planes[0].texture = app.video_tex;
    vtarget.planes[0].flipped = false;
    vtarget.num_planes = 1;
    vtarget.crop = {0.0f, 0.0f, static_cast<float>(w),
                    static_cast<float>(h)};
    if (!composite_frame(app, frame, &vtarget, &params))
      return SDL_APP_FAILURE;
    app.video_rect = vtarget.crop;

    // Pass 2: rasterize annotations and blend them over the video.
    blend_overlay(app, sw.fbo);
  }

  if (!pl_swapchain_submit_frame(app.swapchain)) {
    std::println(stderr, "pl_swapchain_submit_frame failed");
    return SDL_APP_FAILURE;
  }
  pl_swapchain_swap_buffers(app.swapchain);
  return SDL_APP_CONTINUE;
}

// Seek the player to a media time and reset all playback clocks/state so the
// next frame decoded is the one at (or just before) the target. When paused,
// a single frame is rendered so the new position becomes visible.
auto seek_app(App &app, double seconds) -> void {
  app.player.seek_to(seconds);
  app.frame_timer = 0.0;
  app.prev_frame_pts = NAN;
  app.vidclk = MediaClock{};
  if (app.audio_stream != nullptr)
    SDL_ClearAudioStream(app.audio_stream);
  app.audio_pts_end = 0.0;
  app.pending_seek_redraw = app.paused;
  std::println(stderr, "seek: {:.2f}s", seconds);
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

  constexpr std::string_view kShaderFlag = "--shader=";
  std::string video_path = CODOTAKU_DEFAULT_VIDEO_PATH;
  for (int i = 1; i < argc; i++) {
    std::string_view arg = argv[i];
    if (arg == "--shader") {
      if (i + 1 >= argc)
        throw std::runtime_error("--shader requires a file path");
      if (!load_shader(*app, argv[++i]))
        throw std::runtime_error(std::format("failed to load shader: {}", argv[i]));
    } else if (arg.starts_with(kShaderFlag)) {
      std::string shader_path(arg.substr(kShaderFlag.size()));
      if (!load_shader(*app, shader_path.c_str()))
        throw std::runtime_error(std::format("failed to load shader: {}", shader_path));
    } else {
      video_path = argv[i];
    }
  }
  app->player.open(video_path.c_str(), app->hwdev);
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
  // Flush init logs so they are visible even if the process is killed or
  // crashes mid-playback (stdout is block-buffered when redirected).
  std::fflush(stdout);

  clock_set_at(app->wall, 0.0, 0, now_s());
  chk(SDL_ShowWindow(app->window));
  return SDL_APP_CONTINUE;
} catch (const std::exception &e) {
  std::println(stderr, "{}", e.what());
  return SDL_APP_FAILURE;
}

auto SDL_AppIterate(void *appstate) -> SDL_AppResult try {
  auto *app = static_cast<App *>(appstate);

  // While exporting, drive decode/encode directly (no playback pacing).
  if (app->exporter != nullptr)
    return pump_export(*app);

  if (app->paused) {
    if (app->pending_seek_redraw) {
      // Show the frame at the seek target without resuming playback.
      app->pending_seek_redraw = false;
      AVFrame *frame = app->player.next_frame();
      if (frame != nullptr) {
        render_frame(*app, frame);
        double pts = frame_pts_s(app->player, frame);
        if (!std::isnan(pts)) {
          clock_set(app->vidclk, pts, 0);
          app->vidclk.paused = true;
        }
        app->prev_frame_pts = pts;
        av_frame_free(&frame);
      }
    } else if (app->overlay_dirty) {
      // Annotations changed while paused; re-blend them over the frame that is
      // already on screen without decoding a new one.
      app->overlay_dirty = false;
      redraw_overlay(*app);
    } else {
      SDL_Delay(16);
    }
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
                 "drops={} aqueue={} afps={:.0f}",
                 fmt_clock(master), fmt_clock(video),
                 (video - master) * 1000.0, fmt_clock(app->sync_delay),
                 app->frame_drops, app->player.audio_queue_depth(), afps);
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
  case SDL_EVENT_MOUSE_BUTTON_DOWN: {
    auto *app = static_cast<App *>(appstate);
    if (event->button.button != SDL_BUTTON_LEFT)
      break;
    if (app->tool.has_value()) {
      AnnoPoint p = window_to_anno(*app, event->button.x, event->button.y);
      if (*app->tool == AnnoShape::Text) {
        if (!app->text_buffer.empty()) {
          Annotation a{};
          a.shape = AnnoShape::Text;
          const float *c = anno_color(*app);
          a.color[0] = c[0];
          a.color[1] = c[1];
          a.color[2] = c[2];
          a.color[3] = 1.0f;
          a.pts = {p};
          a.text = app->text_buffer;
          a.start_pts = anno_time(*app);
          int id = app->annotations.add(std::move(a));
          app->overlay_dirty = true;
          std::println(stderr,
                       "annotation: text id={} at {:.2f}s text='{}' len={} "
                       "pos=({:.3f},{:.3f})",
                       id, anno_time(*app), app->text_buffer,
                       app->text_buffer.size(), p.x, p.y);
        }
      } else {
        app->drawing = true;
        app->draw_anchor = p;
        app->draw_cur = p;
        app->draw_pts = {p};
        app->overlay_dirty = true;
      }
    } else {
      double dur = app->player.duration();
      if (dur <= 0.0) {
        std::println(stderr, "seek: duration unknown");
        break;
      }
      int w = 0;
      int h = 0;
      if (!SDL_GetWindowSizeInPixels(app->window, &w, &h) || w <= 0)
        break;
      seek_app(*app, dur * static_cast<double>(event->button.x) / w);
    }
    break;
  }
  case SDL_EVENT_MOUSE_BUTTON_UP: {
    auto *app = static_cast<App *>(appstate);
    if (event->button.button == SDL_BUTTON_LEFT && app->drawing) {
      app->drawing = false;
      AnnoPoint p = window_to_anno(*app, event->button.x, event->button.y);
      app->draw_cur = p;
      Annotation a{};
      a.shape = *app->tool;
      const float *c = anno_color(*app);
      a.color[0] = c[0];
      a.color[1] = c[1];
      a.color[2] = c[2];
      a.color[3] = 1.0f;
      a.pts = a.shape == AnnoShape::Freehand
                  ? app->draw_pts
                  : std::vector<AnnoPoint>{app->draw_anchor, p};
      if (a.pts.size() >= 2) {
        float dx = a.pts.back().x - a.pts.front().x;
        float dy = a.pts.back().y - a.pts.front().y;
        if (std::hypot(dx, dy) > 0.001f) {
          a.start_pts = anno_time(*app);
          int id = app->annotations.add(std::move(a));
          app->overlay_dirty = true;
          std::println(stderr, "annotation: {} id={} at {:.2f}s",
                       tool_name(a.shape), id, anno_time(*app));
        }
      }
    }
    break;
  }
  case SDL_EVENT_MOUSE_MOTION: {
    auto *app = static_cast<App *>(appstate);
    if (app->drawing && app->tool.has_value()) {
      app->draw_cur = window_to_anno(*app, event->motion.x, event->motion.y);
      app->overlay_dirty = true;
      if (*app->tool == AnnoShape::Freehand) {
        AnnoPoint &last = app->draw_pts.back();
        if (std::hypot(app->draw_cur.x - last.x, app->draw_cur.y - last.y) >
            0.002f)
          app->draw_pts.push_back(app->draw_cur);
      }
    } else if (app->tool.has_value() && *app->tool == AnnoShape::Text &&
               !app->text_buffer.empty()) {
      // Keep the text preview anchored to the cursor.
      app->overlay_dirty = true;
    }
    break;
  }
  case SDL_EVENT_TEXT_INPUT: {
    auto *app = static_cast<App *>(appstate);
    if (app->tool.has_value() && *app->tool == AnnoShape::Text) {
      app->text_buffer += event->text.text;
      app->overlay_dirty = true;
      std::println(stderr, "text-input: got='{}' len={} buffer='{}'",
                   event->text.text, app->text_buffer.size(),
                   app->text_buffer);
    }
    break;
  }
  case SDL_EVENT_KEY_DOWN: {
    auto *app = static_cast<App *>(appstate);
    if (app->exporter != nullptr) {
      // While exporting, swallow all input except Esc (cancel).
      if (event->key.key == SDLK_ESCAPE)
        cancel_export(*app);
      break;
    }
    const bool in_text =
        app->tool.has_value() && *app->tool == AnnoShape::Text;
    if (in_text && event->key.key == SDLK_ESCAPE) {
      SDL_StopTextInput(app->window);
      std::println(stderr, "text: escape, buffer '{}' discarded",
                   app->text_buffer);
      app->tool.reset();
      app->drawing = false;
      app->text_buffer.clear();
      app->overlay_dirty = true;
      std::println(stderr, "annotation tool: none");
    } else if (in_text && event->key.key == SDLK_RETURN) {
      std::println(stderr, "text: enter clears buffer '{}'", app->text_buffer);
      app->text_buffer.clear();
      app->overlay_dirty = true;
      std::println(stderr, "annotation text: (cleared)");
    } else if (in_text && event->key.key == SDLK_BACKSPACE) {
      if (!app->text_buffer.empty()) {
        std::println(stderr, "text: backspace '{}' -> '{}'",
                     app->text_buffer.back(), app->text_buffer);
        app->text_buffer.pop_back();
        app->overlay_dirty = true;
        std::println(stderr, "text: buffer now '{}'", app->text_buffer);
      }
    } else if (in_text) {
      // Text mode swallows all other shortcuts; characters arrive via
      // SDL_EVENT_TEXT_INPUT so they must not toggle tools/pause/etc.
    } else if (event->key.key == SDLK_SPACE) {
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
    } else if (event->key.key == SDLK_S) {
      if (app->hooks.empty()) {
        std::println(stderr,
                     "no shaders loaded; start with --shader <file>");
      } else {
        app->shaders_enabled = !app->shaders_enabled;
        std::println(stderr, "shaders: {} ({} loaded)",
                     app->shaders_enabled ? "on" : "off", app->hooks.size());
      }
    } else if (event->key.key == SDLK_X) {
      start_export(*app);
    } else if (event->key.key == SDLK_LEFT || event->key.key == SDLK_RIGHT) {
      double t = clock_get(app->vidclk);
      if (std::isnan(t))
        t = 0.0;
      seek_app(*app, t + (event->key.key == SDLK_LEFT ? -10.0 : 10.0));
    } else if (event->key.key == SDLK_R || event->key.key == SDLK_E ||
               event->key.key == SDLK_A || event->key.key == SDLK_F ||
               event->key.key == SDLK_T) {
      AnnoShape shape = event->key.key == SDLK_R
                            ? AnnoShape::Rect
                            : event->key.key == SDLK_E
                                  ? AnnoShape::Ellipse
                                  : event->key.key == SDLK_A
                                        ? AnnoShape::Arrow
                                        : event->key.key == SDLK_F
                                              ? AnnoShape::Freehand
                                              : AnnoShape::Text;
      app->tool = (app->tool.has_value() && *app->tool == shape)
                      ? std::nullopt
                      : std::optional(shape);
      app->overlay_dirty = true;
      if (shape == AnnoShape::Text) {
        if (app->tool.has_value()) {
          SDL_StartTextInput(app->window);
          std::println(stderr, "text: input started (tool=text)");
        } else {
          SDL_StopTextInput(app->window);
          std::println(stderr, "text: input stopped (tool=none)");
        }
      }
      std::println(stderr, "annotation tool: {}",
                   app->tool ? tool_name(*app->tool) : "none");
    } else if (event->key.key == SDLK_C) {
      app->color_idx = (app->color_idx + 1) %
                       (sizeof(kAnnoColors) / sizeof(kAnnoColors[0]));
      app->overlay_dirty = true;
      std::println(stderr, "annotation color: {}", app->color_idx);
    } else if (event->key.key == SDLK_ESCAPE) {
      if (app->tool.has_value()) {
        app->tool.reset();
        app->drawing = false;
        app->text_buffer.clear();
        app->overlay_dirty = true;
        std::println(stderr, "annotation tool: none");
      }
    } else if (event->key.key == SDLK_BACKSPACE) {
      int id = app->annotations.last_id();
      if (id >= 0) {
        double t = anno_time(*app);
        app->annotations.remove_at(t, id);
        app->overlay_dirty = true;
        std::println(stderr, "annotation: removed id={} at {:.2f}s", id, t);
      }
    } else if (event->key.key == SDLK_DELETE) {
      float mx = 0.0f;
      float my = 0.0f;
      SDL_GetMouseState(&mx, &my);
      AnnoPoint p = window_to_anno(*app, mx, my);
      double t = anno_time(*app);
      int id = app->annotations.hit_test(p, t);
      if (id >= 0) {
        app->annotations.remove_at(t, id);
        app->overlay_dirty = true;
        std::println(stderr, "annotation: removed id={} at {:.2f}s", id, t);
      }
    }
    break;
  }
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
  if (app->exporter != nullptr) {
    app->exporter->cancel();
    app->exporter.reset();
  }
  destroy_export_resources(*app);
  if (app->audio_stream != nullptr)
    SDL_DestroyAudioStream(app->audio_stream);
  if (app->renderer != nullptr)
    pl_renderer_destroy(&app->renderer);
  for (const pl_hook *hook : app->hooks)
    pl_mpv_user_shader_destroy(&hook);
  app->hooks.clear();
  if (app->gpu != nullptr) {
    for (auto &tex : app->frame_tex)
      pl_tex_destroy(app->gpu, &tex);
    if (app->video_tex != nullptr)
      pl_tex_destroy(app->gpu, &app->video_tex);
    if (app->overlay_tex != nullptr)
      pl_tex_destroy(app->gpu, &app->overlay_tex);
    if (app->blend_pass != nullptr)
      pl_pass_destroy(app->gpu, &app->blend_pass);
  }
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
