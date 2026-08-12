# Codotaku Video Player

A minimal video player built with C++23, SDL3, FFmpeg, and libplacebo.
It demuxes and decodes video with FFmpeg (Vulkan hardware decoding with a
software fallback) and renders through libplacebo on a Vulkan swapchain,
giving you high-quality scaling and correct HDR color handling.

## Features

- Vulkan hardware-accelerated decoding (`AV_PIX_FMT_VULKAN`) with automatic
  software fallback
- Zero-copy GPU rendering via libplacebo (`pl_map_avframe_ex`)
- HDR-aware rendering: frames are tone-mapped to the swapchain's color space
- High-quality scaling (spline36)
- Real-time playback pacing against the wall clock
- Loops the video at EOF
- Space toggles pause

## Requirements

### Windows

Prebuilt libraries are vendored under `external/`:

- FFmpeg (`external/ffmpeg`)
- libplacebo (`external/libplacebo`)
- stb_truetype (`external/stb`, single public-domain header)

System dependencies:

- [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) (found via `find_package(Vulkan)`)
- SDL3 (`find_package(SDL3)`, searched via `$VULKAN_SDK/cmake`)
- A C/C++20+ compiler (the repo is developed with `clang-cl` + Ninja)

### Linux

Uses system packages via pkg-config:

```sh
sudo apt install libavcodec-dev libavformat-dev libavutil-dev \
                 libplacebo-dev libsdl3-dev libvulkan-dev
```

## Building

### Windows

```sh
cmake --preset default
cmake --build --preset default-release
```

The build copies the FFmpeg, libplacebo, and SDL3 DLLs next to the executable
automatically. The output lands in `out/build/default`.

### Linux

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Usage

```sh
codotaku_video_player [video-file]
```

If no file is given, the player falls back to the bundled test clip
(`tests/bbb_sunflower_1080p_60fps_normal.mp4`), which is committed via
[Git LFS](https://git-lfs.com) — clone with LFS installed to get it.

### Controls

| Key       | Action        |
| --------- | ------------- |
| `Space`   | Pause/resume  |
| Close btn | Quit          |

## Project structure

```
main.cpp        SDL3 app: Vulkan device, libplacebo swapchain/renderer, event loop
player.cpp/h    FFmpeg demux + decode (Vulkan hwaccel with software fallback)
libav_impl.c    libplacebo <-> libav helpers (PL_LIBAV_IMPLEMENTATION)
annotations.h   CPU rasterizer for drawing annotations over the video (text via
                stb_truetype, system TTF font loaded at runtime)
stb_truetype.cpp TU instantiating the vendored stb_truetype implementation
CMakeLists.txt  Build: vendored deps on Windows, pkg-config on Linux
tests/          Sample video (git-lfs)
```

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
