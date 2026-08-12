#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/rational.h>
}

// Encodes composited RGBA8 frames (y-down, straight alpha already baked by the
// caller) into an MP4/H.264 file, re-encoding the source audio stream to AAC so
// the output plays everywhere (Windows Media Player, browsers, phones). When
// the audio track cannot be decoded or re-encoded, the export falls back to
// video-only rather than failing.
class Exporter {
 public:
  Exporter() = default;
  ~Exporter();
  Exporter(const Exporter &) = delete;
  Exporter &operator=(const Exporter &) = delete;

  // Throws std::runtime_error on failure (including when no usable encoder is
  // available). `sar` is preserved on the output so non-square-pixel sources
  // keep their display aspect. `audio_par`/`audio_time_base` describe the
  // source audio stream; pass null to export video-only.
  void open(const char *path, int width, int height, double fps,
            const AVRational &sar, const AVRational &audio_time_base,
            const AVCodecParameters *audio_par);

  bool is_open() const { return ofmt_ != nullptr; }
  bool has_audio() const { return astream_ != nullptr; }
  const std::string &path() const { return path_; }

  // `rgba` is tightly packed RGBA8, y-down (row 0 = top), width*height*4
  // bytes, `row_stride` bytes between rows. `pts` is the presentation time in
  // `pts_tb`; pass AV_NOPTS_VALUE to fall back to a frame counter at `fps`.
  void push_video(const uint8_t *rgba, size_t row_stride, int64_t pts,
                  const AVRational &pts_tb);

  // Feeds a compressed source audio packet (in the codec/timing of the
  // `audio_par` stream passed to open) into the internal decoder, re-encoding
  // the decoded PCM to AAC. Takes ownership and frees the packet.
  void push_audio(AVPacket *pkt);

  // Flush the encoders, write the trailer, and close the output.
  void finish();

  // Close the output and delete the (partial) file.
  void cancel();

 private:
  bool try_open(const AVRational &audio_time_base,
                const AVCodecParameters *audio_par);
  void close(bool delete_file);
  bool setup_audio(const AVRational &audio_time_base,
                   const AVCodecParameters *audio_par);
  void encode_audio_frame(AVFrame *decoded);
  void drain_audio_resampler();
  bool ensure_aconv_buffer(int frame_size);
  void drain_audio_encoder();
  void abort_muxer();
  void fifo_append(const uint8_t *const *planes, int nb);
  bool fifo_pop_into(AVFrame *frame, int nb);
  bool emit_fifo_frame(int nb);
  int aenc_frame_size() const;

  std::string path_;
  AVFormatContext *ofmt_ = nullptr;
  AVCodecContext *venc_ = nullptr;
  AVStream *vstream_ = nullptr;
  AVStream *astream_ = nullptr;
  struct SwsContext *sws_ = nullptr;
  AVFrame *yuv_ = nullptr;
  AVPacket *vpkt_ = nullptr;
  int w_ = 0;
  int h_ = 0;
  double fps_ = 30.0;
  int64_t last_pts_ = AV_NOPTS_VALUE;  // last written video pts (output tb)
  int64_t frame_count_ = 0;

  // Audio re-encode (source -> AAC).
  bool audio_active_ = false;   // an audio track is present in the output
  AVRational asrc_tb_{1, 1};    // source audio stream time base
  AVCodecContext *adec_ = nullptr;  // source audio decoder
  AVCodecContext *aenc_ = nullptr;  // AAC encoder
  struct SwrContext *aswr_ = nullptr;  // source fmt -> fltp
  AVFrame *aconv_ = nullptr;     // resampled frame fed to the AAC encoder
  AVPacket *apkt_ = nullptr;     // encoded audio packet
  int64_t aout_pts_ = 0;         // running pts fallback for NOPTS input

  // Staging FIFO of resampled (fltp) samples: interleaved by
  // [sample][channel]. AAC only accepts exactly frame_size samples per frame
  // (one undersized last frame is tolerated), so decoded frames of any size
  // are accumulated here and emitted only in full chunks.
  std::vector<float> aout_fifo_;
  int aout_fifo_samples_ = 0;
  int64_t aout_fifo_pts_ = 0;    // pts of the first sample in the FIFO
};
