#pragma once

#include <atomic>
#include <deque>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/threadmessage.h>
}

struct PlayerInfo {
  int width = 0;
  int height = 0;
  int64_t duration_us = 0;
  AVRational time_base{};
  const char *codec_name = nullptr;
  bool hwaccel = false;
  bool has_audio = false;
  int audio_sample_rate = 0;
  int audio_channels = 0;
};

// Demuxes and decodes a single video stream. Frames are returned as new
// references and may be in a hardware pixel format (AV_PIX_FMT_VULKAN) when
// hardware decoding is active, or a software format otherwise.
//
// Demuxing runs on a dedicated thread that pushes AVPackets through an
// AVThreadMessageQueue; decoding happens on the caller's thread.
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
  // has been reached. Blocks while the demux thread has no packet ready.
  AVFrame *next_frame();

  // Returns the next decoded audio frame, or nullptr when none is queued.
  // The caller takes ownership and must av_frame_free() it.
  AVFrame *take_audio_frame();

  bool audio_pending() const { return !audio_queue_.empty(); }

  // Seek back to the start and resume decoding (for looping).
  void rewind();

  bool eof() const { return eof_; }
  const PlayerInfo &info() const { return info_; }
  AVRational time_base() const { return info_.time_base; }

private:
  struct DemuxInterrupt {
    std::atomic<int> abort{0};
  };

  void open_codec(const AVCodec *codec, AVBufferRef *hwdev);
  void open_audio();
  void drain_audio();
  bool vulkan_hw_config(const AVCodec *codec) const;
  void start_demux();
  void stop_demux();
  void demux_loop();
  static int demux_interrupt_cb(void *opaque);

  AVFormatContext *fmt_ = nullptr;
  AVCodecContext *dec_ = nullptr;
  AVCodecContext *adec_ = nullptr;
  AVPacket *pending_pkt_ = nullptr;
  AVThreadMessageQueue *packet_q_ = nullptr;
  std::thread demux_thread_;
  DemuxInterrupt interrupt_;
  bool demux_eof_ = false;
  int stream_ = -1;
  int audio_stream_ = -1;
  bool eof_ = false;
  PlayerInfo info_;
  std::deque<AVFrame *> audio_queue_;
};
