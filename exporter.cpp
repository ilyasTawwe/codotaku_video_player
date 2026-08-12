#include "exporter.h"

#include <algorithm>
#include <cstdio>
#include <format>
#include <print>
#include <source_location>
#include <stdexcept>
#include <string_view>

#include <libavutil/dict.h>
#include <libavutil/error.h>

// Unlike the other FFmpeg headers, libswscale/swscale.h lacks an `extern "C"`
// guard, so it must be wrapped manually to keep the C symbol names.
extern "C" {
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace {

auto av_err_str(int err) -> std::string {
  char buf[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(err, buf, sizeof(buf));
  return buf;
}

[[noreturn]] auto throw_av(int err,
                           std::source_location loc =
                               std::source_location::current()) -> void {
  throw std::runtime_error(std::format(
      "{}:{}:{} ({}) FFmpeg Error: {}", loc.file_name(), loc.line(),
      loc.column(), loc.function_name(), av_err_str(err)));
}

} // namespace

Exporter::~Exporter() { close(false); }

bool Exporter::try_open(const AVRational &audio_time_base,
                        const AVCodecParameters *audio_par) {
  int ret = avformat_alloc_output_context2(&ofmt_, nullptr, "mp4",
                                           path_.c_str());
  if (ret < 0 || ofmt_ == nullptr) {
    ofmt_ = nullptr;
    return false;
  }

  vstream_ = avformat_new_stream(ofmt_, nullptr);
  if (vstream_ == nullptr) {
    close(false);
    return false;
  }
  vstream_->id = 0;
  vstream_->time_base = venc_->time_base;
  vstream_->sample_aspect_ratio = venc_->sample_aspect_ratio;
  ret = avcodec_parameters_from_context(vstream_->codecpar, venc_);
  if (ret < 0) {
    close(false);
    return false;
  }

  if (audio_active_ && aenc_ != nullptr) {
    astream_ = avformat_new_stream(ofmt_, nullptr);
    if (astream_ == nullptr) {
      close(false);
      return false;
    }
    astream_->id = 1;
    astream_->time_base = aenc_->time_base;
    ret = avcodec_parameters_from_context(astream_->codecpar, aenc_);
    if (ret < 0) {
      close(false);
      return false;
    }
  }

  if (!(ofmt_->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&ofmt_->pb, path_.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
      close(false);
      return false;
    }
  }

  ret = avformat_write_header(ofmt_, nullptr);
  if (ret < 0) {
    close(false);
    return false;
  }
  return true;
}

void Exporter::open(const char *path, int width, int height, double fps,
                    const AVRational &sar,
                    const AVRational &audio_time_base,
                    const AVCodecParameters *audio_par) {
  path_ = path;
  w_ = width;
  h_ = height;
  fps_ = fps > 0.0 ? fps : 30.0;

  const AVCodec *codec = nullptr;
  const char *codec_name = nullptr;
  for (const char *name : {"libx264", "h264_nvenc", "libopenh264", "mpeg4"}) {
    codec = avcodec_find_encoder_by_name(name);
    if (codec != nullptr) {
      codec_name = name;
      break;
    }
  }
  if (codec == nullptr)
    throw std::runtime_error("export: no H.264 encoder available");

  venc_ = avcodec_alloc_context3(codec);
  if (venc_ == nullptr)
    throw std::bad_alloc();
  venc_->width = w_;
  venc_->height = h_;
  venc_->pix_fmt = AV_PIX_FMT_YUV420P;
  venc_->sample_aspect_ratio = sar;
  AVRational fr = av_d2q(fps_, 1000000);
  venc_->time_base = AVRational{fr.den, fr.num};
  venc_->framerate = fr;
  venc_->gop_size = std::max(1, static_cast<int>(fps_ * 2.0 + 0.5));
  // Standard 8-bit SDR metadata; the actual pixel math lives in sws_ below.
  venc_->color_primaries = AVCOL_PRI_BT709;
  venc_->color_trc = AVCOL_TRC_BT709;
  venc_->colorspace = AVCOL_SPC_BT709;
  venc_->color_range = AVCOL_RANGE_MPEG;

  AVDictionary *opts = nullptr;
  if (codec_name == std::string_view("libx264")) {
    av_dict_set(&opts, "preset", "medium", 0);
    av_dict_set(&opts, "crf", "18", 0);
  } else {
    // Constant-quality isn't available on the fallbacks; pick a sane bitrate.
    venc_->bit_rate = std::max(1'000'000, w_ * h_ * static_cast<int>(fps_) / 10);
  }
  if (avcodec_open2(venc_, codec, &opts) < 0) {
    av_dict_free(&opts);
    throw std::runtime_error(
        std::format("export: failed to open {} encoder", codec_name));
  }
  av_dict_free(&opts);

  sws_ = sws_getContext(w_, h_, AV_PIX_FMT_RGBA, w_, h_, AV_PIX_FMT_YUV420P,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (sws_ == nullptr) {
    close(false);
    throw std::runtime_error("export: sws_getContext failed");
  }
  // Full-range BT.709 RGB in -> limited-range BT.709 YUV out, matching the
  // 8-bit SDR pipeline rendered by libplacebo into the export target.
  sws_setColorspaceDetails(sws_, sws_getCoefficients(SWS_CS_ITU709),
                           /*srcRange=*/1, sws_getCoefficients(SWS_CS_ITU709),
                           /*dstRange=*/0, /*brightness=*/0,
                           /*contrast=*/1 << 16, /*saturation=*/1 << 16);

  yuv_ = av_frame_alloc();
  if (yuv_ == nullptr) {
    close(false);
    throw std::bad_alloc();
  }
  yuv_->format = AV_PIX_FMT_YUV420P;
  yuv_->width = w_;
  yuv_->height = h_;
  yuv_->sample_aspect_ratio = sar;
  yuv_->color_primaries = AVCOL_PRI_BT709;
  yuv_->color_trc = AVCOL_TRC_BT709;
  yuv_->colorspace = AVCOL_SPC_BT709;
  yuv_->color_range = AVCOL_RANGE_MPEG;
  if (av_frame_get_buffer(yuv_, 32) < 0) {
    close(false);
    throw_av(AVERROR(ENOMEM));
  }

  vpkt_ = av_packet_alloc();
  if (vpkt_ == nullptr) {
    close(false);
    throw std::bad_alloc();
  }

  setup_audio(audio_time_base, audio_par);

  bool ok = try_open(audio_time_base, audio_par);
  if (!ok && audio_active_) {
    std::println(stderr, "export: audio muxing failed, retrying video-only");
    audio_active_ = false;
    astream_ = nullptr;
    ok = try_open(audio_time_base, nullptr);
  }
  if (!ok) {
    close(false);
    throw std::runtime_error("export: failed to create output file");
  }
  if (!audio_active_ && audio_par != nullptr)
    std::println(stderr, "export: continuing video-only (audio omitted)");
}

bool Exporter::setup_audio(const AVRational &audio_time_base,
                           const AVCodecParameters *audio_par) {
  if (audio_par == nullptr || audio_par->codec_id == AV_CODEC_ID_NONE)
    return false;
  if (audio_par->sample_rate <= 0) {
    std::println(stderr, "export: audio stream has no sample rate; omitting");
    return false;
  }

  const AVCodec *adec = avcodec_find_decoder(audio_par->codec_id);
  if (adec == nullptr) {
    std::println(stderr, "export: no decoder for {}; omitting audio",
                 avcodec_get_name(audio_par->codec_id));
    return false;
  }
  adec_ = avcodec_alloc_context3(adec);
  if (adec_ == nullptr)
    return false;
  if (avcodec_parameters_to_context(adec_, audio_par) < 0 ||
      avcodec_open2(adec_, adec, nullptr) < 0) {
    std::println(stderr, "export: failed to open audio decoder; omitting");
    return false;
  }

  const AVCodec *aenc = avcodec_find_encoder_by_name("aac");
  if (aenc == nullptr) {
    std::println(stderr, "export: no AAC encoder available; omitting audio");
    return false;
  }
  aenc_ = avcodec_alloc_context3(aenc);
  if (aenc_ == nullptr)
    return false;
  aenc_->sample_rate = adec_->sample_rate;
  aenc_->sample_fmt = AV_SAMPLE_FMT_FLTP;
  aenc_->time_base = AVRational{1, aenc_->sample_rate};
  // Preserve the source channel layout; fall back to stereo on failure.
  if (av_channel_layout_copy(&aenc_->ch_layout, &adec_->ch_layout) < 0 ||
      aenc_->ch_layout.nb_channels <= 0) {
    av_channel_layout_uninit(&aenc_->ch_layout);
    aenc_->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
  }
  aenc_->bit_rate = aenc_->ch_layout.nb_channels <= 2 ? 128'000 : 192'000;
  if (avcodec_open2(aenc_, aenc, nullptr) < 0) {
    std::println(stderr, "export: failed to open AAC encoder; omitting");
    return false;
  }

  AVChannelLayout out_layout = aenc_->ch_layout;
  int out_rate = aenc_->sample_rate;
  if (swr_alloc_set_opts2(&aswr_, &out_layout, AV_SAMPLE_FMT_FLTP, out_rate,
                          &adec_->ch_layout, adec_->sample_fmt,
                          adec_->sample_rate, 0, nullptr) < 0 ||
      swr_init(aswr_) < 0) {
    std::println(stderr, "export: failed to init audio resampler; omitting");
    return false;
  }

  aconv_ = av_frame_alloc();
  apkt_ = av_packet_alloc();
  if (aconv_ == nullptr || apkt_ == nullptr)
    return false;
  aconv_->format = AV_SAMPLE_FMT_FLTP;
  aconv_->sample_rate = out_rate;
  av_channel_layout_copy(&aconv_->ch_layout, &out_layout);

  asrc_tb_ = audio_time_base;
  audio_active_ = true;
  std::println("export: audio {} ch @ {} Hz -> AAC", aenc_->ch_layout.nb_channels,
               aenc_->sample_rate);
  return true;
}

void Exporter::encode_audio_frame(AVFrame *decoded) {
  if (aenc_ == nullptr || astream_ == nullptr)
    return;
  if (swr_convert_frame(aswr_, aconv_, decoded) < 0) {
    std::println(stderr, "export: audio resample failed");
    return;
  }
  int64_t pts = AV_NOPTS_VALUE;
  if (decoded->pts != AV_NOPTS_VALUE)
    pts = av_rescale_q(decoded->pts, asrc_tb_, aenc_->time_base);
  if (pts == AV_NOPTS_VALUE)
    pts = aout_pts_;
  aout_pts_ = pts + std::max(0, aconv_->nb_samples);
  aconv_->pts = pts;
  if (avcodec_send_frame(aenc_, aconv_) < 0) {
    std::println(stderr, "export: avcodec_send_frame (audio) failed");
    return;
  }
  drain_audio_encoder();
}

void Exporter::drain_audio_encoder() {
  if (astream_ == nullptr)
    return;
  while (avcodec_receive_packet(aenc_, apkt_) == 0) {
    av_packet_rescale_ts(apkt_, aenc_->time_base, astream_->time_base);
    apkt_->stream_index = astream_->index;
    if (av_interleaved_write_frame(ofmt_, apkt_) < 0)
      std::println(stderr, "export: av_interleaved_write_frame (audio) failed");
    av_packet_unref(apkt_);
  }
}

void Exporter::push_video(const uint8_t *rgba, size_t row_stride, int64_t pts,
                          const AVRational &pts_tb) {
  if (ofmt_ == nullptr)
    return;

  const uint8_t *src[4] = {rgba, nullptr, nullptr, nullptr};
  int src_stride[4] = {static_cast<int>(row_stride), 0, 0, 0};
  sws_scale(sws_, src, src_stride, 0, h_, yuv_->data, yuv_->linesize);

  int64_t out_pts = AV_NOPTS_VALUE;
  if (pts != AV_NOPTS_VALUE)
    // Frames are always fed to the encoder in its own time base; the muxer's
    // (possibly modified) stream time base is applied to packets later.
    out_pts = av_rescale_q(pts, pts_tb, venc_->time_base);
  if (out_pts == AV_NOPTS_VALUE) {
    AVRational fptb = av_d2q(fps_, 1000000);
    fptb = AVRational{fptb.den, fptb.num};  // 1 frame = 1/fps seconds
    out_pts = av_rescale_q(frame_count_, fptb, venc_->time_base);
  }
  if (last_pts_ != AV_NOPTS_VALUE && out_pts <= last_pts_)
    out_pts = last_pts_ + 1;
  yuv_->pts = out_pts;
  last_pts_ = out_pts;
  frame_count_++;

  int ret = avcodec_send_frame(venc_, yuv_);
  if (ret < 0) {
    std::println(stderr, "export: avcodec_send_frame: {}", av_err_str(ret));
    return;
  }
  while (true) {
    ret = avcodec_receive_packet(venc_, vpkt_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      break;
    if (ret < 0) {
      std::println(stderr, "export: avcodec_receive_packet: {}",
                   av_err_str(ret));
      break;
    }
    av_packet_rescale_ts(vpkt_, venc_->time_base, vstream_->time_base);
    vpkt_->stream_index = vstream_->index;
    if (av_interleaved_write_frame(ofmt_, vpkt_) < 0)
      std::println(stderr, "export: av_interleaved_write_frame (video) failed");
    av_packet_unref(vpkt_);
  }
}

void Exporter::push_audio(AVPacket *pkt) {
  if (pkt == nullptr)
    return;
  if (ofmt_ == nullptr || !audio_active_ || adec_ == nullptr) {
    av_packet_free(&pkt);
    return;
  }
  if (avcodec_send_packet(adec_, pkt) < 0)
    std::println(stderr, "export: audio avcodec_send_packet failed");
  av_packet_free(&pkt);
  AVFrame *decoded = av_frame_alloc();
  if (decoded == nullptr)
    return;
  while (avcodec_receive_frame(adec_, decoded) == 0) {
    encode_audio_frame(decoded);
    av_frame_unref(decoded);
  }
  av_frame_free(&decoded);
}

void Exporter::finish() {
  // Flush the audio decoder and re-encoder so trailing samples land in the
  // output, then do the same for the video encoder.
  if (adec_ != nullptr) {
    avcodec_send_packet(adec_, nullptr);  // flush buffered audio frames
    AVFrame *decoded = av_frame_alloc();
    while (decoded != nullptr &&
           avcodec_receive_frame(adec_, decoded) == 0) {
      encode_audio_frame(decoded);
      av_frame_unref(decoded);
    }
    av_frame_free(&decoded);
  }
  if (aenc_ != nullptr) {
    avcodec_send_frame(aenc_, nullptr);  // flush buffered audio
    drain_audio_encoder();
  }

  if (ofmt_ != nullptr && venc_ != nullptr) {
    avcodec_send_frame(venc_, nullptr);  // flush buffered video frames
    while (true) {
      int ret = avcodec_receive_packet(venc_, vpkt_);
      if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
        break;
      if (ret < 0)
        break;
      av_packet_rescale_ts(vpkt_, venc_->time_base, vstream_->time_base);
      vpkt_->stream_index = vstream_->index;
      if (av_interleaved_write_frame(ofmt_, vpkt_) < 0)
        break;
      av_packet_unref(vpkt_);
    }
  }
  if (ofmt_ != nullptr)
    av_write_trailer(ofmt_);
  close(false);
}

void Exporter::cancel() { close(true); }

void Exporter::close(bool delete_file) {
  if (ofmt_ != nullptr && !(ofmt_->oformat->flags & AVFMT_NOFILE) &&
      ofmt_->pb != nullptr)
    avio_closep(&ofmt_->pb);
  avformat_free_context(ofmt_);
  ofmt_ = nullptr;
  vstream_ = nullptr;
  astream_ = nullptr;
  avcodec_free_context(&venc_);
  av_frame_free(&yuv_);
  av_packet_free(&vpkt_);
  if (sws_ != nullptr) {
    sws_freeContext(sws_);
    sws_ = nullptr;
  }
  avcodec_free_context(&adec_);
  avcodec_free_context(&aenc_);
  if (aswr_ != nullptr) {
    swr_free(&aswr_);
    aswr_ = nullptr;
  }
  av_frame_free(&aconv_);
  av_packet_free(&apkt_);
  audio_active_ = false;
  if (delete_file && !path_.empty())
    std::remove(path_.c_str());
}
