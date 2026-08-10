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
CMakeLists.txt  Build: vendored deps on Windows, pkg-config on Linux
tests/          Sample video (git-lfs)
```

## License

See `external/ffmpeg/LICENSE` for FFmpeg's licensing terms.
