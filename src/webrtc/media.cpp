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

  std::shared_ptr<rtc_video_sink_t> rtc_video_sink_t::create(std::shared_ptr<rtc::Track> track, std::uint8_t payload_type, std::uint32_t ssrc) {
    // The constructor is private, so make_shared cannot be used.
    std::shared_ptr<rtc_video_sink_t> sink {new rtc_video_sink_t {std::move(track)}};
    sink->attach(payload_type, ssrc);
    return sink;
  }

  rtc_video_sink_t::rtc_video_sink_t(std::shared_ptr<rtc::Track> track):
      track_ {std::move(track)} {
  }

  void rtc_video_sink_t::attach(std::uint8_t payload_type, std::uint32_t ssrc) {
    auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "sunshine", payload_type, rtc::H264RtpPacketizer::ClockRate);

    // The encoder's output uses Annex-B start codes between NAL units.
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtp_config);

    // Sender reports let the browser line up video with audio.
    packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp_config));

    // Answer retransmission requests for recently sent packets.
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());

    // When the browser cannot decode what it has, it asks for a fresh keyframe. The handler holds
    // the sink weakly so a late callback cannot reach a destroyed sink.
    std::weak_ptr<rtc_video_sink_t> weak = weak_from_this();
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

  void rtc_video_sink_t::set_keyframe_requester(std::function<void()> requester) {
    std::lock_guard lg {requester_mutex_};
    requester_ = std::move(requester);
  }

  void rtc_video_sink_t::request_keyframe() {
    std::function<void()> requester;
    {
      std::lock_guard lg {requester_mutex_};
      requester = requester_;
    }
    if (requester) {
      requester();
    }
  }

  void rtc_video_sink_t::video_frame(std::string_view payload, bool idr, [[maybe_unused]] std::int64_t frame_index) {
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

  void rtc_video_sink_t::audio_frame([[maybe_unused]] std::string_view payload) {
  }
}  // namespace webrtc
