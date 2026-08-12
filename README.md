# Codotaku Video Player

A fast, good-looking video player for Windows that plays your files smoothly
even on 4K/HDR content — and lets you draw on the video, apply upscaling
shaders, and export the result to MP4.

Built with C++23, SDL3, FFmpeg, and libplacebo, it uses your GPU for both
decoding and rendering: high-quality scaling, correct HDR color, and buttery
playback without taxing your CPU.

## How to use it

1. **Download** the latest Windows release from the
   [Releases page](https://github.com/ilyasTawwe/codotaku_video_player/releases)
   (the `*-win64.zip` file).
2. **Unzip** it anywhere — no installer, no admin rights.
3. **Run** `codotaku_video_player.exe` and give it a video file:

   ```sh
   codotaku_video_player.exe my-video.mp4
   ```

4. **Play, pause, seek, draw, export** — everything is on the keyboard (see
   [Controls](#controls) below).

It plays MP4, MKV, MOV, WebM, AVI, and most other formats FFmpeg supports,
including H.264/HEVC/AV1 and AC3/EAC3/AAC/FLAC/Opus audio.

**Quick tour:**

- `Space` pause/resume · `←`/`→` jump 10s · click anywhere to seek
- Press `R`/`E`/`A`/`F`/`T` to draw rectangles, ellipses, arrows, freehand
  lines, or text on top of the video
- Press `X` to export the current video to MP4 (your drawings included)
- Press `S` to toggle Anime4K upscaling shaders on/off

## Why is the download so big?

The `*-win64.zip` is around **90–100 MB** for two reasons:

1. **The entire FFmpeg library set ships with the app** — FFmpeg is a *single
   shared build* that contains every codec, demuxer, and filter, which is what
   lets one small player exe play H.264, HEVC, AV1, MP3, FLAC, Opus, and everything
   else out of the box. The three big DLLs are roughly 98 MB (`avfilter`),
   93 MB (`avcodec`), and 19 MB (`avformat`) uncompressed.
2. **The Anime4K shader pack is bundled** — 49 ready-to-use shaders under
   `packs/Anime4K/glsl/` (~5 MB), so upscaling works without downloading
   anything else.

The zip compresses those down to ~90 MB; there is no installer or extra
download needed — unzip and it runs.

## Custom shaders

Shaders are plain mpv-style `.glsl` files. Pass any number of them at startup;
each one is applied in order, and `S` toggles the whole chain on/off. When a
shader loads, its tunable parameters are printed to the console.

**Bundled — Anime4K** (upscale + sharpen, great for anime and 2D art):

```sh
codotaku_video_player.exe my-video.mkv --shader packs/Anime4K/glsl/Upscale/Anime4K_Upscale_CNN_x2_M.glsl
```

Other categories live in `packs/Anime4K/glsl/`: `Deblur`, `Denoise`, `Restore`,
`Experimental-Effects`, and `Upscale+Denoise`.

**Bundled — greyscale test:**

```sh
codotaku_video_player.exe my-video.mp4 --shader shaders/greyscale.glsl
```

You can chain several for a combined effect:

```sh
codotaku_video_player.exe my-video.mp4 \
  --shader packs/Anime4K/glsl/Restore/Anime4K_Restore_CNN_S.glsl \
  --shader packs/Anime4K/glsl/Upscale/Anime4K_Upscale_CNN_x2_M.glsl
```

### Writing your own

A shader is a GLSL snippet annotated with `//!` directives that tell the
renderer *where* in the pipeline it runs and *which textures* it sees. The
simplest one, `shaders/greyscale.glsl`, is the best starting point:

```glsl
//!DESC Greyscale test
//!HOOK NATIVE
//!BIND NATIVE
//!SAVE NATIVE
//!WIDTH NATIVE.w
//!HEIGHT NATIVE.h
vec4 hook() {
    vec4 c = NATIVE_tex(NATIVE_pos);
    return vec4(c.r, 0.5, 0.5, c.a);
}
```

The pieces:

| Directive    | Meaning |
| ------------ | ------- |
| `//!DESC ...` | One-line description shown in the console |
| `//!HOOK ...` | Where in the pipeline the shader runs (see hook points below) |
| `//!BIND <T>` | Input texture, exposed to GLSL as `T_tex`, `T_pos`, `T_size`, `T_pt` |
| `//!SAVE <T>` | Output texture; the shader result replaces `<T>` |
| `//!WIDTH`/`//!HEIGHT` | Output size, usually expressed in terms of a bound texture |
| `vec4 hook()` | Entry point; returns the output pixel |

**Hook points** (where your shader runs):

- `NATIVE` — the decoded frame in its original color space, before scaling
- `LINEAR` — after conversion to linear light, before scaling
- `SCALED` — after scaling to the target resolution, before color management
- `OUTPUT` — the last stage, right before the frame is presented

`NATIVE` and `LINEAR` are *resizable* (you pick `WIDTH`/`HEIGHT`, which is how
upscalers work); the later stages are fixed-size.

**Tunable parameters** — add a slider the player exposes at load time (it
prints every parameter and its description when the shader loads):

```glsl
//!PARAM strength
//!DESC Blend between original and processed
//!TYPE float
//!MINIMUM 0.0
//!MAXIMUM 1.0
0.8
```

The default value is the last line; then just use `strength` as a variable in
`hook()`.

For the full format, see the [libplacebo user-shader
documentation](https://libplacebo.org/custom-shaders/) (mpv-compatible).

## Features

- **GPU-accelerated everywhere** — Vulkan hardware decoding (with automatic
  software fallback) and zero-copy GPU rendering, so 4K/HDR plays smoothly
- **HDR-aware** — frames are tone-mapped to your display's color space; no
  washed-out or crushed colors
- **Crystal-clear scaling** — high-quality spline36 resampling, plus bundled
  [Anime4K](https://github.com/bloc97/Anime4K) shaders that upscale and
  sharpen anime (toggle with `S`)
- **Rock-solid A/V sync** — audio-master clock with frame-drop resync, like
  ffplay/mpv; audio never drifts from video
- **Seek like a pro** — click to seek, `←`/`→` for ±10s; works even while paused
- **Draw on the video** — rectangles, ellipses, arrows, freehand, and text
  annotations, in multiple colors, baked straight into your exports
- **One-key export** — press `X` to write an MP4 (H.264 + AAC) of the current
  video with your shaders and annotations applied; falls back to video-only if
  the audio can't be re-encoded
- **Custom shaders** — load any mpv-style `.glsl`/`.hook` shader pack at
  startup with `--shader <file>`

## Controls

| Key       | Action                                            |
| --------- | ------------------------------------------------- |
| `Space`   | Pause/resume                                      |
| `X`       | Start export to `export_<timestamp>.mp4` (runs to end of video; `Esc` cancels) |
| `S`       | Toggle user shaders on/off                        |
| `←`/`→`   | Seek backward/forward 10 seconds                  |
| Click     | Seek to that position on the progress bar         |
| `R`/`E`/`A`/`F`/`T` | Annotation tool: rect / ellipse / arrow / freehand / text |
| `C`       | Cycle annotation color                            |
| `Esc`     | Cancel current annotation tool                    |
| `Backspace` | Remove the most recent annotation                 |
| Close btn | Quit                                              |

## For developers

### Requirements

**Windows** — prebuilt libraries are vendored under `external/` (FFmpeg,
libplacebo, stb_truetype). You need:

- [Vulkan SDK](https://vulkan.lunarg.com/sdk/home)
- SDL3 (`find_package(SDL3)`, searched via `$VULKAN_SDK/cmake`)
- A C/C++20+ compiler (the repo is developed with `clang-cl` + Ninja)

**Linux** — uses system packages via pkg-config:

```sh
sudo apt install libavcodec-dev libavformat-dev libavutil-dev \
                 libplacebo-dev libsdl3-dev libvulkan-dev
```

### Building

**Windows:**

```sh
cmake --preset default
cmake --build --preset default-release
```

The build copies the FFmpeg, libplacebo, and SDL3 DLLs next to the executable
automatically. The output lands in `out/build/default`.

**Linux:**

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Project structure

```
main.cpp        SDL3 app: Vulkan device, libplacebo swapchain/renderer, event loop
player.cpp/h    FFmpeg demux + decode (Vulkan hwaccel with software fallback)
exporter.cpp/h  MP4 export: H.264 encode + AC3->AAC re-encode, annotation bake
libav_impl.c    libplacebo <-> libav helpers (PL_LIBAV_IMPLEMENTATION)
annotations.h   CPU rasterizer for drawing annotations over the video (text via
                stb_truetype, system TTF font loaded at runtime)
stb_truetype.cpp TU instantiating the vendored stb_truetype implementation
sync.h          MediaClock + A/V sync helpers
CMakeLists.txt  Build: vendored deps on Windows, pkg-config on Linux
packs/          Bundled user-shader packs (Anime4K) copied next to the exe
shaders/        First-party shaders (e.g. greyscale test)
tests/          Sample video (git-lfs)
```

### Command-line options

```
codotaku_video_player [video-file] [--shader <file>]
```

- `video-file` — optional; if omitted, a bundled test clip plays (dev builds)
- `--shader <file>` — load an mpv-style shader pack at startup

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).

It is distributed as a combined work that links against GPL-3.0 FFmpeg, so
the whole program is covered by GPL-3.0. Its dependencies carry their own
licenses:

- FFmpeg: GPL-3.0 (`external/ffmpeg/LICENSE`)
- libplacebo: LGPL-2.1-or-later
- SDL3: zlib license
- stb_truetype: public domain / MIT (`external/stb/stb_truetype.h`)
- Sample clip (`tests/`): Big Buck Bunny, CC-BY 3.0
