/**
 * @file src/webrtc/media.cpp
 * @brief Definitions for delivering encoded media to a browser over WebRTC.
 */
// standard includes
#include <utility>

// local includes
#include "media.h"
#include "src/logging.h"

using namespace std::literals;

namespace webrtc {
  namespace {
    constexpr auto keyframe_retry_interval = 500ms;  ///< Minimum gap between keyframe requests while waiting to start.
  }  // namespace

  std::shared_ptr<rtc_media_sink_t> rtc_media_sink_t::create(std::shared_ptr<rtc::Track> track, video_codec_e codec, std::uint8_t payload_type, std::uint32_t ssrc) {
    // The constructor is private, so make_shared cannot be used.
    std::shared_ptr<rtc_media_sink_t> sink {new rtc_media_sink_t {std::move(track)}};
    sink->attach(codec, payload_type, ssrc);
    return sink;
  }

  rtc_media_sink_t::rtc_media_sink_t(std::shared_ptr<rtc::Track> track):
      track_ {std::move(track)} {
  }

  void rtc_media_sink_t::attach(video_codec_e codec, std::uint8_t payload_type, std::uint32_t ssrc) {
    // Both codecs use the same 90 kHz RTP clock; only the packetizer that turns a frame into RTP
    // packets depends on which one was negotiated.
    auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "sunshine", payload_type, rtc::H264RtpPacketizer::ClockRate);

    std::shared_ptr<rtc::RtpPacketizer> packetizer;
    if (codec == video_codec_e::av1) {
      // The encoder's output is a low-overhead OBU stream: one whole temporal unit (frame) per call,
      // which the packetizer itself splits into individual, correctly length-prefixed OBUs. This is
      // not Annex-B, and has nothing in common with the H.264 path below beyond both ending up as RTP.
      packetizer = std::make_shared<rtc::AV1RtpPacketizer>(rtc::AV1RtpPacketizer::Packetization::TemporalUnit, rtp_config);
    } else {
      // The encoder's output uses Annex-B start codes between NAL units.
      packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtp_config);
    }

    // Sender reports let the browser line up video with audio.
    packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp_config));

    // Answer retransmission requests for recently sent packets.
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());

    // When the browser cannot decode what it has, it asks for a fresh keyframe. The handler holds
    // the sink weakly so a late callback cannot reach a destroyed sink.
    std::weak_ptr<rtc_media_sink_t> weak = weak_from_this();
    packetizer->addToChain(std::make_shared<rtc::PliHandler>([weak]() {
      if (auto sink = weak.lock()) {
        sink->request_keyframe();
      }
    }));

    track_->setMediaHandler(packetizer);
    track_->onOpen([weak]() {
      if (auto sink = weak.lock()) {
        sink->request_keyframe();
      }
    });
  }

  void rtc_media_sink_t::add_audio(std::shared_ptr<rtc::Track> track, std::uint8_t payload_type, std::uint32_t ssrc) {
    auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "sunshine", payload_type, rtc::OpusRtpPacketizer::DefaultClockRate);

    // Opus packets are already self-contained, so they go out as they are, one per RTP packet.
    auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtp_config);

    // Sender reports let the browser line audio up with video.
    packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp_config));

    track->setMediaHandler(packetizer);
    audio_track_ = std::move(track);
  }

  void rtc_media_sink_t::set_keyframe_requester(std::function<void()> requester) {
    std::lock_guard lg {requester_mutex_};
    requester_ = std::move(requester);
  }

  void rtc_media_sink_t::request_keyframe() {
    std::function<void()> requester;
    {
      std::lock_guard lg {requester_mutex_};
      requester = requester_;
    }
    if (requester) {
      requester();
    }
  }

  void rtc_media_sink_t::video_frame(std::string_view payload, bool idr, [[maybe_unused]] std::int64_t frame_index) {
    if (!track_->isOpen()) {
      return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (!started_) {
      // Wait for a keyframe so the browser starts on a decodable picture. The encoder emitted its
      // first one before the browser finished connecting, so ask for another.
      if (!idr) {
        if (now - last_keyframe_request_ >= keyframe_retry_interval) {
          last_keyframe_request_ = now;
          request_keyframe();
        }
        return;
      }
      started_ = true;
      first_frame_ = now;
    }

    try {
      const std::chrono::duration<double> elapsed = now - first_frame_;
      track_->sendFrame(reinterpret_cast<const rtc::byte *>(payload.data()), payload.size(), rtc::FrameInfo {elapsed});
    } catch (const std::exception &e) {
      BOOST_LOG(debug) << "WebRTC: dropping video frame: "sv << e.what();
    }
  }

  void rtc_media_sink_t::audio_frame(std::string_view payload) {
    if (!audio_track_ || !audio_track_->isOpen()) {
      return;
    }

    try {
      const std::chrono::duration<double> elapsed = audio_packets_sent_ * std::chrono::milliseconds {audio_packet_duration_ms};
      audio_track_->sendFrame(reinterpret_cast<const rtc::byte *>(payload.data()), payload.size(), rtc::FrameInfo {elapsed});
      ++audio_packets_sent_;
    } catch (const std::exception &e) {
      BOOST_LOG(debug) << "WebRTC: dropping audio packet: "sv << e.what();
    }
  }
}  // namespace webrtc
