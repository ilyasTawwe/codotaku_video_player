#pragma once

#include <atomic>
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
  AVRational sar{};  // pixel aspect ratio of the source video
  const char *codec_name = nullptr;
  bool hwaccel = false;
  double fps = 0.0;
  bool has_audio = false;
  int audio_sample_rate = 0;
  int audio_channels = 0;
  AVRational audio_time_base{};
};

// Demuxes and decodes a single video stream. Frames are returned as new
// references and may be in a hardware pixel format (AV_PIX_FMT_VULKAN) when
// hardware decoding is active, or a software format otherwise.
//
// Demuxing runs on a dedicated thread that dispatches AVPackets through two
// AVThreadMessageQueues (video and audio). Video decoding happens on the
// caller's thread; audio decoding runs on its own thread that pushes decoded
// AVFrames onto a third queue consumed by the caller.
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

  bool audio_pending() const { return audio_queue_depth() > 0; }

  int audio_queue_depth() const {
    return audio_frame_q_ == nullptr
               ? 0
               : av_thread_message_queue_nb_elems(audio_frame_q_);
  }

  // Seek back to the start and resume decoding (for looping).
  void rewind();

  // Seek to a media time in seconds (clamped to [0, duration]). Throws
  // std::runtime_error on failure. Worker threads are torn down and restarted,
  // so packets/frames in flight from before the seek are discarded.
  void seek_to(double seconds);

  // Media duration in seconds, or 0 when unknown (some containers omit it).
  double duration() const;

  bool eof() const { return eof_; }
  const PlayerInfo &info() const { return info_; }
  AVRational time_base() const { return info_.time_base; }

  // --- Export audio passthrough ------------------------------------------
  // When enabled, demuxed audio packets are routed to a dedicated capture
  // queue (instead of the decoder) so the exporter can mux them unchanged.
  // The toggle only takes effect for packets demuxed after the next seek.
  void set_audio_capture(bool on);

  // Returns the next captured audio packet (caller frees), or nullptr when
  // none is queued.
  AVPacket *take_audio_packet();

  // Encoded parameters of the source audio stream, or nullptr when the file
  // has no audio. Used to set up the exporter's audio stream.
  const AVCodecParameters *audio_codecpar() const;

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
  void start_adec();
  void stop_adec();
  void demux_loop();
  void audio_decode_loop();
  static int demux_interrupt_cb(void *opaque);

  AVFormatContext *fmt_ = nullptr;
  AVCodecContext *dec_ = nullptr;
  AVCodecContext *adec_ = nullptr;
  AVPacket *pending_pkt_ = nullptr;
  AVThreadMessageQueue *video_q_ = nullptr;
  AVThreadMessageQueue *audio_q_ = nullptr;
  AVThreadMessageQueue *audio_frame_q_ = nullptr;
  // Raw encoded audio packets for export, fed by the demux thread when
  // capture_audio_ is set.
  AVThreadMessageQueue *audio_pkt_q_ = nullptr;
  std::atomic<bool> capture_audio_{false};
  std::thread demux_thread_;
  std::thread adec_thread_;
  DemuxInterrupt interrupt_;
  bool demux_eof_ = false;
  int stream_ = -1;
  int audio_stream_ = -1;
  bool eof_ = false;
  PlayerInfo info_;
};
