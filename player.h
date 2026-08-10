#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
}

struct PlayerInfo {
  int width = 0;
  int height = 0;
  int64_t duration_us = 0;
  AVRational time_base{};
  const char *codec_name = nullptr;
  bool hwaccel = false;
};

// Demuxes and decodes a single video stream. Frames are returned as new
// references and may be in a hardware pixel format (AV_PIX_FMT_VULKAN) when
// hardware decoding is active, or a software format otherwise.
class Player {
public:
  Player() = default;
  ~Player();
  Player(const Player &) = delete;
  Player &operator=(const Player &) = delete;

  // Throws std::runtime_error on failure.
  void open(const char *path, AVBufferRef *hwdev);
  void close();

  // Returns a new reference the caller must av_frame_free(); nullptr when EOF
  // has been reached.
  AVFrame *next_frame();

  // Seek back to the start and resume decoding (for looping).
  void rewind();

  bool eof() const { return eof_; }
  const PlayerInfo &info() const { return info_; }
  AVRational time_base() const { return info_.time_base; }

private:
  void open_codec(const AVCodec *codec, AVBufferRef *hwdev);
  bool vulkan_hw_config(const AVCodec *codec) const;

  AVFormatContext *fmt_ = nullptr;
  AVCodecContext *dec_ = nullptr;
  AVPacket *pkt_ = nullptr;
  int stream_ = -1;
  bool eof_ = false;
  PlayerInfo info_;
};
