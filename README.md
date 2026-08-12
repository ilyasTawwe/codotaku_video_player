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
