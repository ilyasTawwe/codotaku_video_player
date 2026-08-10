#include "player.h"

#include <cstdlib>
#include <format>
#include <print>
#include <source_location>
#include <stdexcept>
#include <string>

#include <libavutil/error.h>

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

Player::~Player() { close(); }

void Player::close() {
  av_packet_free(&pkt_);
  avcodec_free_context(&dec_);
  avformat_close_input(&fmt_);
  stream_ = -1;
  eof_ = false;
  info_ = {};
}

bool Player::vulkan_hw_config(const AVCodec *codec) const {
  for (int i = 0;; i++) {
    const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
    if (config == nullptr)
      break;
    if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
        config->device_type == AV_HWDEVICE_TYPE_VULKAN)
      return true;
  }
  return false;
}

void Player::open_codec(const AVCodec *codec, AVBufferRef *hwdev) {
  dec_ = avcodec_alloc_context3(codec);
  if (dec_ == nullptr)
    throw std::bad_alloc();

  AVStream *st = fmt_->streams[stream_];
  int ret = avcodec_parameters_to_context(dec_, st->codecpar);
  if (ret < 0) {
    avcodec_free_context(&dec_);
    throw_av(ret);
  }

  if (hwdev != nullptr) {
    dec_->hw_device_ctx = av_buffer_ref(hwdev);
    if (dec_->hw_device_ctx == nullptr) {
      avcodec_free_context(&dec_);
      throw std::bad_alloc();
    }
    dec_->hwaccel_flags = AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH |
                          AV_HWACCEL_FLAG_IGNORE_LEVEL;
  } else {
    // Let FFmpeg pick a sensible number of frame threads for software decode.
    dec_->thread_count = 0;
  }
}

void Player::open(const char *path, AVBufferRef *hwdev) {
  close();

  int ret = avformat_open_input(&fmt_, path, nullptr, nullptr);
  if (ret < 0)
    throw_av(ret);

  ret = avformat_find_stream_info(fmt_, nullptr);
  if (ret < 0)
    throw_av(ret);

  ret = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (ret < 0)
    throw std::runtime_error("no video stream found");
  stream_ = ret;

  AVStream *st = fmt_->streams[stream_];
  const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
  if (codec == nullptr)
    throw std::runtime_error(
        std::format("unsupported codec: {}",
                    avcodec_get_name(st->codecpar->codec_id)));

  pkt_ = av_packet_alloc();
  if (pkt_ == nullptr)
    throw std::bad_alloc();

  if (hwdev != nullptr && vulkan_hw_config(codec)) {
    open_codec(codec, hwdev);
    if (avcodec_open2(dec_, codec, nullptr) >= 0) {
      info_.hwaccel = true;
    } else {
      std::println(stderr,
                   "Vulkan hardware decode unavailable, falling back to "
                   "software decoding");
      avcodec_free_context(&dec_);
      open_codec(codec, nullptr);
      if (avcodec_open2(dec_, codec, nullptr) < 0)
        throw std::runtime_error("failed to open decoder in software");
    }
  } else {
    open_codec(codec, nullptr);
    if (avcodec_open2(dec_, codec, nullptr) < 0)
      throw std::runtime_error("failed to open decoder");
  }

  info_.width = st->codecpar->width;
  info_.height = st->codecpar->height;
  info_.time_base = st->time_base;
  info_.codec_name = codec->name;
  if (st->duration != AV_NOPTS_VALUE)
    info_.duration_us =
        av_rescale_q(st->duration, st->time_base, AV_TIME_BASE_Q);
}

AVFrame *Player::next_frame() {
  if (eof_)
    return nullptr;

  AVFrame *frame = av_frame_alloc();
  if (frame == nullptr)
    throw std::bad_alloc();

  for (;;) {
    int ret = avcodec_receive_frame(dec_, frame);
    if (ret == 0)
      return frame;

    if (ret == AVERROR(EAGAIN)) {
      int pr = av_read_frame(fmt_, pkt_);
      if (pr == AVERROR_EOF) {
        avcodec_send_packet(dec_, nullptr); // flush buffered frames
        continue;
      }
      if (pr < 0) {
        std::println(stderr, "av_read_frame: {}", av_err_str(pr));
        break;
      }
      if (pkt_->stream_index != stream_) {
        av_packet_unref(pkt_);
        continue;
      }
      avcodec_send_packet(dec_, pkt_);
      av_packet_unref(pkt_);
      continue;
    }

    if (ret != AVERROR_EOF)
      std::println(stderr, "avcodec_receive_frame: {}", av_err_str(ret));
    break;
  }

  eof_ = true;
  av_frame_free(&frame);
  return nullptr;
}

void Player::rewind() {
  if (fmt_ == nullptr)
    return;
  int ret = av_seek_frame(fmt_, stream_, 0, AVSEEK_FLAG_BACKWARD);
  if (ret < 0)
    throw_av(ret);
  avcodec_flush_buffers(dec_);
  eof_ = false;
}
