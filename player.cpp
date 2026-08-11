#include "player.h"

#include <cstdlib>
#include <format>
#include <print>
#include <source_location>
#include <stdexcept>
#include <string>

#include <libavutil/error.h>
#include <libavutil/threadmessage.h>

namespace {

auto av_err_str(int err) -> std::string {
  char buf[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(err, buf, sizeof(buf));
  return buf;
}

// Demuxed packets are pushed onto these queues, which gives the demux thread
// natural backpressure so it can't run arbitrarily far ahead of playback.
// The video queue is deeper than the audio queue: audio is delivered on a
// first-priority blocking send, while a deep video queue absorbs transient
// decode/display stalls without the demux thread blocking on it.
constexpr unsigned kVideoQueueDepth = 512;
constexpr unsigned kAudioQueueDepth = 128;
// Decoded audio frames are handed to the main thread through this queue.
constexpr unsigned kAudioFrameQueueDepth = 32;

[[noreturn]] auto throw_av(int err,
                           std::source_location loc =
                               std::source_location::current()) -> void {
  throw std::runtime_error(std::format(
      "{}:{}:{} ({}) FFmpeg Error: {}", loc.file_name(), loc.line(),
      loc.column(), loc.function_name(), av_err_str(err)));
}

} // namespace

Player::~Player() { close(); }

int Player::demux_interrupt_cb(void *opaque) {
  return static_cast<DemuxInterrupt *>(opaque)->abort.load();
}

void Player::close() {
  // Abort the demux thread and wake any blocked send/recv on the queues. The
  // err_send on audio_frame_q_ also unblocks a pending send in the audio
  // thread that would otherwise wait for the main thread to drain it.
  interrupt_.abort = 1;
  for (AVThreadMessageQueue *q : {video_q_, audio_q_, audio_frame_q_}) {
    if (q == nullptr)
      continue;
    av_thread_message_queue_set_err_send(q, AVERROR_EXIT);
    av_thread_message_queue_set_err_recv(q, AVERROR_EXIT);
  }
  stop_demux();
  stop_adec();

  for (AVThreadMessageQueue **q : {&video_q_, &audio_q_, &audio_frame_q_}) {
    if (*q == nullptr)
      continue;
    av_thread_message_flush(*q);
    av_thread_message_queue_free(q);
  }

  avcodec_free_context(&adec_);
  av_packet_free(&pending_pkt_);
  avcodec_free_context(&dec_);
  avformat_close_input(&fmt_);
  stream_ = -1;
  audio_stream_ = -1;
  eof_ = false;
  demux_eof_ = false;
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

void Player::open_audio() {
  int ret = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (ret < 0)
    return;
  audio_stream_ = ret;

  AVStream *st = fmt_->streams[audio_stream_];
  const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
  if (codec == nullptr) {
    std::println(stderr, "unsupported audio codec: {}",
                 avcodec_get_name(st->codecpar->codec_id));
    audio_stream_ = -1;
    return;
  }

  adec_ = avcodec_alloc_context3(codec);
  if (adec_ == nullptr)
    throw std::bad_alloc();

  ret = avcodec_parameters_to_context(adec_, st->codecpar);
  if (ret < 0) {
    avcodec_free_context(&adec_);
    throw_av(ret);
  }

  if (avcodec_open2(adec_, codec, nullptr) < 0) {
    std::println(stderr, "failed to open audio decoder, disabling audio");
    avcodec_free_context(&adec_);
    audio_stream_ = -1;
    return;
  }

  info_.has_audio = true;
  info_.audio_sample_rate = adec_->sample_rate;
  info_.audio_channels = adec_->ch_layout.nb_channels;
  info_.audio_time_base = st->time_base;
}

void Player::open(const char *path, AVBufferRef *hwdev) {
  close();

  fmt_ = avformat_alloc_context();
  if (fmt_ == nullptr)
    throw std::bad_alloc();
  interrupt_.abort = 0;
  fmt_->interrupt_callback = {&Player::demux_interrupt_cb, &interrupt_};

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

  pending_pkt_ = av_packet_alloc();
  if (pending_pkt_ == nullptr)
    throw std::bad_alloc();

  int qr = av_thread_message_queue_alloc(&video_q_, kVideoQueueDepth,
                                         sizeof(AVPacket *));
  if (qr < 0)
    throw_av(qr);
  av_thread_message_queue_set_free_func(video_q_, [](void *msg) {
    av_packet_free(reinterpret_cast<AVPacket **>(msg));
  });

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

  open_audio();

  if (info_.has_audio) {
    int aq = av_thread_message_queue_alloc(&audio_q_, kAudioQueueDepth,
                                           sizeof(AVPacket *));
    if (aq < 0)
      throw_av(aq);
    av_thread_message_queue_set_free_func(audio_q_, [](void *msg) {
      av_packet_free(reinterpret_cast<AVPacket **>(msg));
    });

    int fq = av_thread_message_queue_alloc(&audio_frame_q_, kAudioQueueDepth,
                                           sizeof(AVFrame *));
    if (fq < 0)
      throw_av(fq);
    av_thread_message_queue_set_free_func(audio_frame_q_, [](void *msg) {
      av_frame_free(reinterpret_cast<AVFrame **>(msg));
    });
  }

  info_.width = st->codecpar->width;
  info_.height = st->codecpar->height;
  info_.time_base = st->time_base;
  info_.codec_name = codec->name;
  if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
    info_.fps = av_q2d(st->avg_frame_rate);
  else if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0)
    info_.fps = av_q2d(st->r_frame_rate);
  if (st->duration != AV_NOPTS_VALUE)
    info_.duration_us =
        av_rescale_q(st->duration, st->time_base, AV_TIME_BASE_Q);

  start_demux();
  if (info_.has_audio)
    start_adec();
}

void Player::start_demux() {
  interrupt_.abort = 0;
  demux_eof_ = false;
  if (video_q_ != nullptr) {
    av_thread_message_queue_set_err_send(video_q_, 0);
    av_thread_message_queue_set_err_recv(video_q_, 0);
  }
  if (demux_thread_.joinable())
    stop_demux();
  demux_thread_ = std::thread(&Player::demux_loop, this);
}

void Player::stop_demux() {
  if (demux_thread_.joinable())
    demux_thread_.join();
}

void Player::start_adec() {
  if (audio_q_ == nullptr || audio_frame_q_ == nullptr)
    return;
  av_thread_message_queue_set_err_send(audio_q_, 0);
  av_thread_message_queue_set_err_recv(audio_q_, 0);
  av_thread_message_queue_set_err_send(audio_frame_q_, 0);
  av_thread_message_queue_set_err_recv(audio_frame_q_, 0);
  if (adec_thread_.joinable())
    stop_adec();
  adec_thread_ = std::thread(&Player::audio_decode_loop, this);
}

void Player::stop_adec() {
  if (adec_thread_.joinable())
    adec_thread_.join();
}

void Player::demux_loop() {
  for (;;) {
    AVPacket *raw = av_packet_alloc();
    if (raw == nullptr)
      break;
    int pr = av_read_frame(fmt_, raw);
    if (pr < 0) {
      if (pr != AVERROR_EOF && pr != AVERROR_EXIT)
        std::println(stderr, "av_read_frame: {}", av_err_str(pr));
      av_packet_free(&raw);
      break;
    }
    // Blocking sends apply backpressure; the pointer ownership transfers.
    // Video uses a deep queue so the demux thread keeps reading audio even
    // while the display stalls, and audio always gets its packet.
    int sr = 0;
    if (raw->stream_index == stream_) {
      sr = av_thread_message_queue_send(video_q_, &raw, 0);
    } else if (raw->stream_index == audio_stream_) {
      sr = av_thread_message_queue_send(audio_q_, &raw, 0);
    } else {
      av_packet_free(&raw);
      continue;
    }
    if (sr < 0) {
      av_packet_free(&raw);
      break;
    }
  }
  // Receivers drain the queues, then see AVERROR_EOF on recv. recv() only
  // wakes on err_recv/cond_recv, so set the receiver error, not just err_send.
  av_thread_message_queue_set_err_recv(video_q_, AVERROR_EOF);
  if (audio_q_ != nullptr)
    av_thread_message_queue_set_err_recv(audio_q_, AVERROR_EOF);
}

void Player::audio_decode_loop() {
  for (;;) {
    AVPacket *raw = nullptr;
    int pr = av_thread_message_queue_recv(audio_q_, &raw, 0);
    if (pr == 0) {
      if (avcodec_send_packet(adec_, raw) < 0)
        std::println(stderr, "audio avcodec_send_packet failed");
      av_packet_free(&raw);
      drain_audio();
    } else if (pr == AVERROR_EOF) {
      avcodec_send_packet(adec_, nullptr); // flush buffered audio frames
      drain_audio();
      av_thread_message_queue_set_err_send(audio_frame_q_, AVERROR_EOF);
      return;
    } else {
      return;
    }
  }
}

AVFrame *Player::next_frame() {
  if (eof_)
    return nullptr;

  AVFrame *frame = av_frame_alloc();
  if (frame == nullptr)
    throw std::bad_alloc();

  for (;;) {
    // Push a video packet that was held back while the decoder input was
    // full. pending_pkt_ is a pre-allocated packet, so only send it when it
    // actually carries data; an empty packet would flush the decoder.
    if (pending_pkt_->buf != nullptr) {
      int sr = avcodec_send_packet(dec_, pending_pkt_);
      if (sr == AVERROR(EAGAIN)) {
        // Input still full; don't demux more, just try to receive below.
      } else {
        if (sr < 0)
          std::println(stderr, "avcodec_send_packet: {}", av_err_str(sr));
        av_packet_unref(pending_pkt_);
      }
    }

    // Pull the next demuxed packet (blocking: the demux thread feeds this).
    if (pending_pkt_->buf == nullptr && !demux_eof_) {
      AVPacket *raw = nullptr;
      int pr = av_thread_message_queue_recv(video_q_, &raw, 0);
      if (pr == 0) {
        int sr = avcodec_send_packet(dec_, raw);
        if (sr == AVERROR(EAGAIN))
          av_packet_move_ref(pending_pkt_, raw);
        else if (sr < 0)
          std::println(stderr, "avcodec_send_packet: {}", av_err_str(sr));
        av_packet_free(&raw);
      } else if (pr == AVERROR_EOF) {
        demux_eof_ = true;
        avcodec_send_packet(dec_, nullptr); // flush buffered video frames
      } else {
        std::println(stderr, "demux queue: {}", av_err_str(pr));
        demux_eof_ = true;
      }
    }

    // Return a decoded video frame whenever one is ready.
    int ret = avcodec_receive_frame(dec_, frame);
    if (ret == 0)
      return frame;
    if (ret == AVERROR_EOF)
      break;
    if (ret != AVERROR(EAGAIN))
      std::println(stderr, "avcodec_receive_frame: {}", av_err_str(ret));
    if (demux_eof_ && pending_pkt_->buf == nullptr)
      break; // decoder flushed; no more output
    // EAGAIN: keep pulling packets until a frame materializes.
  }

  eof_ = true;
  av_frame_free(&frame);
  return nullptr;
}

void Player::drain_audio() {
  if (adec_ == nullptr)
    return;
  for (;;) {
    AVFrame *af = av_frame_alloc();
    if (af == nullptr)
      break;
    int ret = avcodec_receive_frame(adec_, af);
    if (ret == 0) {
      // Blocking send applies backpressure: the audio thread pauses when the
      // main thread stops draining (e.g. during video pacing).
      int sr = av_thread_message_queue_send(audio_frame_q_, &af, 0);
      if (sr < 0)
        av_frame_free(&af);
      continue;
    }
    av_frame_free(&af);
    break;
  }
}

AVFrame *Player::take_audio_frame() {
  if (audio_frame_q_ == nullptr)
    return nullptr;
  AVFrame *af = nullptr;
  int ret = av_thread_message_queue_recv(audio_frame_q_, &af,
                                         AV_THREAD_MESSAGE_NONBLOCK);
  if (ret < 0)
    return nullptr;
  return af;
}

void Player::rewind() {
  if (fmt_ == nullptr)
    return;

  // Both worker threads have normally exited at EOF; stop_demux/stop_adec
  // are safe no-ops then. If an audio frame send is still blocked on a full
  // queue, err_send unblocks it so the join can't deadlock.
  stop_demux();
  if (audio_frame_q_ != nullptr)
    av_thread_message_queue_set_err_send(audio_frame_q_, AVERROR_EXIT);
  stop_adec();

  for (AVThreadMessageQueue **q : {&video_q_, &audio_q_, &audio_frame_q_}) {
    if (*q == nullptr)
      continue;
    av_thread_message_flush(*q);
    av_thread_message_queue_set_err_send(*q, 0);
    av_thread_message_queue_set_err_recv(*q, 0);
  }

  int ret = av_seek_frame(fmt_, stream_, 0, AVSEEK_FLAG_BACKWARD);
  if (ret < 0)
    throw_av(ret);
  avcodec_flush_buffers(dec_);
  if (adec_ != nullptr)
    avcodec_flush_buffers(adec_);
  av_packet_unref(pending_pkt_);
  eof_ = false;
  demux_eof_ = false;

  start_demux();
  if (info_.has_audio)
    start_adec();
}
