cmake_minimum_required(VERSION 3.10.0)
project(codotaku_video_player VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

if(UNIX AND NOT APPLE)
    # Linux: use system packages via pkg-config instead of the prebuilt
    # libraries under external/ (those are Windows-only).
    find_package(PkgConfig REQUIRED)
    find_package(Vulkan REQUIRED) # for <vulkan/vulkan.h>
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil)
    pkg_check_modules(PLACEBO REQUIRED IMPORTED_TARGET libplacebo)
    pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)

    add_executable(codotaku_video_player main.cpp player.cpp libav_impl.c stb_truetype.cpp)
    target_link_libraries(codotaku_video_player PRIVATE
        PkgConfig::FFMPEG
        PkgConfig::PLACEBO
        PkgConfig::SDL3
        Vulkan::Headers
        Threads::Threads
    )
else()
    # Windows (and other): prebuilt vendored dependencies.
    set(FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg" CACHE PATH "Path to FFmpeg install dir")
    set(FFMPEG_LIBS avcodec avformat avutil swscale swresample avfilter avdevice)

    set(PLACEBO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/libplacebo" CACHE PATH "Path to libplacebo install dir")

    foreach(LIB ${FFMPEG_LIBS})
        add_library(FFmpeg::${LIB} UNKNOWN IMPORTED)

        # Detect library filename based on toolchain (e.g., libavcodec.a, avcodec.lib, or libavcodec.dll.a)
        find_library(FFMPEG_${LIB}_LOCATION
            NAMES ${LIB} lib${LIB}
            PATHS "${FFMPEG_DIR}/lib"
            NO_DEFAULT_PATH
        )

        set_target_properties(FFmpeg::${LIB} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_DIR}/include"
            IMPORTED_LOCATION "${FFMPEG_${LIB}_LOCATION}"
        )
    endforeach()

    find_package(Vulkan REQUIRED)

    list(APPEND CMAKE_PREFIX_PATH $ENV{VULKAN_SDK}/cmake)
    find_package(SDL3 REQUIRED)

    add_library(Placebo::placebo UNKNOWN IMPORTED)

    find_library(PLACEBO_LOCATION
        NAMES placebo libplacebo
        PATHS "${PLACEBO_DIR}/lib"
        NO_DEFAULT_PATH
    )

    set_target_properties(Placebo::placebo PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${PLACEBO_DIR}/include"
        IMPORTED_LOCATION "${PLACEBO_LOCATION}"
    )

    # Attach system dependencies directly to Placebo::placebo
    target_link_libraries(Placebo::placebo INTERFACE Threads::Threads)
    if(Vulkan_FOUND)
        target_link_libraries(Placebo::placebo INTERFACE Vulkan::Headers)
    endif()

    add_executable(codotaku_video_player main.cpp player.cpp libav_impl.c stb_truetype.cpp)
    target_link_libraries(codotaku_video_player PRIVATE
        FFmpeg::avformat
        FFmpeg::avcodec
        FFmpeg::avutil
        Placebo::placebo
        SDL3::SDL3
    )
endif()

target_compile_definitions(codotaku_video_player PRIVATE
    SDL_MAIN_USE_CALLBACKS
    CODOTAKU_DEFAULT_VIDEO_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/bbb_sunflower_1080p_60fps_normal.mp4")
if(WIN32)
    target_compile_definitions(codotaku_video_player PRIVATE NOMINMAX)
endif()
if(MSVC)
    target_compile_options(codotaku_video_player PRIVATE /EHsc)
endif()

if(WIN32)
    add_custom_command(TARGET codotaku_video_player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${FFMPEG_DIR}/bin"
        "$<TARGET_FILE_DIR:codotaku_video_player>"
        COMMENT "Copying FFmpeg DLLs to output directory..."
    )
    add_custom_command(TARGET codotaku_video_player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${PLACEBO_DIR}/bin"
        "$<TARGET_FILE_DIR:codotaku_video_player>"
        COMMENT "Copying libplacebo DLLs to output directory..."
    )
    add_custom_command(TARGET codotaku_video_player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SDL3::SDL3>"
        "$<TARGET_FILE_DIR:codotaku_video_player>"
        COMMENT "Copying SDL3.dll to output directory..."
    )

    # Copy each pack's mpv-style shaders next to the exe so the build output is
    # self-contained. Only the glsl subfolder is needed at runtime; the full
    # Anime4K checkout is ~285MB of models/experiments.
    file(GLOB PACK_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/packs/*")
    foreach(PACK_DIR ${PACK_DIRS})
        get_filename_component(PACK_NAME "${PACK_DIR}" NAME)
        if(EXISTS "${PACK_DIR}/glsl")
            add_custom_command(TARGET codotaku_video_player POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PACK_DIR}/glsl"
                "$<TARGET_FILE_DIR:codotaku_video_player>/packs/${PACK_NAME}/glsl"
                COMMENT "Copying ${PACK_NAME} shaders to output directory..."
            )
        endif()
    endforeach()

    # First-party shaders under shaders/ (e.g. the greyscale test).
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
        add_custom_command(TARGET codotaku_video_player POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
            "$<TARGET_FILE_DIR:codotaku_video_player>/shaders"
            COMMENT "Copying project shaders to output directory..."
        )
    endif()
endif()
