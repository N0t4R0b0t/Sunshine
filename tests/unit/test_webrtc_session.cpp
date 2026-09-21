/**
 * @file tests/unit/test_webrtc_session.cpp
 * @brief Tests for WebRTC browser session signaling and lifecycle.
 */
#include "../tests_common.h"

#ifdef SUNSHINE_BUILD_WEBRTC

  // standard includes
  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <cstdint>
  #include <memory>
  #include <mutex>
  #include <string>
  #include <thread>
  #include <vector>

  // lib includes
  #include <rtc/rtc.hpp>

  // local includes
  #include "src/webrtc/media.h"
  #include "src/webrtc/session.h"

using namespace std::literals;

namespace {
  /**
   * @brief H.264 format parameters as a browser would offer them.
   */
  constexpr auto high_profile = "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=64001f"sv;
  constexpr auto baseline_profile = "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f"sv;

  /**
   * @brief What a fake browser offers to receive.
   */
  enum class video_e {
    none,  ///< Data channel only.
    h264_high_and_baseline,  ///< H.264 in both High and Baseline profiles.
    h264_baseline,  ///< H.264 in Baseline profile only.
    vp8_only,  ///< A codec Sunshine cannot send.
  };

  /**
   * @brief An in-process stand-in for a browser: a peer connection that offers a data channel and,
   *        optionally, video it wants to receive.
   */
  struct fake_browser_t {
    std::shared_ptr<rtc::PeerConnection> connection = std::make_shared<rtc::PeerConnection>(rtc::Configuration {});
    std::shared_ptr<rtc::DataChannel> channel;  ///< Data channel created for the offer.
    std::shared_ptr<rtc::Track> video_track;  ///< Video track, when video is offered.

    std::mutex mutex;  ///< Guards the fields below.
    std::condition_variable cv;  ///< Signaled when any field below changes.
    bool gathered = false;  ///< Whether ICE gathering finished.
    bool open = false;  ///< Whether the data channel opened.
    std::vector<std::string> received;  ///< Text messages received on the channel.
    std::vector<rtc::binary> rtp_packets;  ///< RTP packets received on the video track, sender reports excluded.

    /**
     * @brief Create the data channel, the video track if asked, and wire up callbacks.
     *
     * @param video What video to offer to receive.
     */
    explicit fake_browser_t(video_e video = video_e::none) {
      connection->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
          std::lock_guard lg {mutex};
          gathered = true;
          cv.notify_all();
        }
      });

      if (video != video_e::none) {
        rtc::Description::Video description {"video", rtc::Description::Direction::RecvOnly};
        switch (video) {
          case video_e::h264_high_and_baseline:
            description.addH264Codec(96, std::string {baseline_profile});
            description.addH264Codec(98, std::string {high_profile});
            break;
          case video_e::h264_baseline:
            description.addH264Codec(96, std::string {baseline_profile});
            break;
          default:
            description.addVideoCodec(100, "VP8");
            break;
        }
        video_track = connection->addTrack(description);
        video_track->onMessage([this](rtc::binary packet) {
          // RTCP shares the track. Its packet types (200 to 204) are what an RTP marker bit plus a
          // payload type of 72 to 76 would look like, so RTP has to be told apart by that range.
          if (packet.size() < 12 || (static_cast<std::uint8_t>(packet[1]) >= 192 && static_cast<std::uint8_t>(packet[1]) <= 223)) {
            return;
          }
          std::lock_guard lg {mutex};
          rtp_packets.push_back(std::move(packet));
          cv.notify_all();
        },
                               nullptr);
      }

      channel = connection->createDataChannel("input");
      channel->onOpen([this]() {
        std::lock_guard lg {mutex};
        open = true;
        cv.notify_all();
      });
      channel->onMessage([this](rtc::message_variant message) {
        if (const auto *text = std::get_if<std::string>(&message)) {
          std::lock_guard lg {mutex};
          received.push_back(*text);
          cv.notify_all();
        }
      });
    }

    /**
     * @brief Wait for a condition to become true.
     *
     * @param timeout Longest time to wait.
     * @param predicate Condition, evaluated with the mutex held.
     * @return Whether the condition became true in time.
     */
    template<typename Predicate>
    bool wait(std::chrono::milliseconds timeout, Predicate predicate) {
      std::unique_lock lock {mutex};
      return cv.wait_for(lock, timeout, predicate);
    }

    /**
     * @brief Produce a complete SDP offer.
     *
     * @return The offer, or an empty string if gathering did not finish.
     */
    std::string make_offer() {
      if (!wait(5s, [this] {
            return gathered;
          })) {
        return {};
      }
      return std::string {*connection->localDescription()};
    }
  };

  /**
   * @brief What a fake host stream records, shared so tests can look after it is handed away.
   */
  struct fake_stream_state_t {
    std::atomic_bool running {true};  ///< Whether the fake stream is still running.
    std::atomic_int idr_requests {0};  ///< Times a keyframe was requested.
    std::atomic_int stop_calls {0};  ///< Times the stream was stopped.
  };

  /**
   * @brief A host stream that does nothing but record how it is driven.
   */
  class fake_stream_t: public webrtc::stream_handle_t {
  public:
    /**
     * @brief Create a fake stream.
     *
     * @param state Where to record activity.
     */
    explicit fake_stream_t(std::shared_ptr<fake_stream_state_t> state):
        state_ {std::move(state)} {
    }

    bool running() override {
      return state_->running;
    }

    void stop() override {
      ++state_->stop_calls;
      state_->running = false;
    }

    void request_idr() override {
      ++state_->idr_requests;
    }

  private:
    std::shared_ptr<fake_stream_state_t> state_;  ///< Where activity is recorded.
  };

  /**
   * @brief Records how the session layer started a stream.
   */
  struct fake_starter_t {
    std::shared_ptr<fake_stream_state_t> stream = std::make_shared<fake_stream_state_t>();  ///< State of the stream handed out.
    std::shared_ptr<webrtc::media_sink_t> sink;  ///< The sink the session layer supplied.
    webrtc::stream_request_t request;  ///< The request the session layer passed.
    std::atomic_int calls {0};  ///< Times a stream was started.
    bool fail = false;  ///< Whether to fail instead of starting.

    /**
     * @brief Install this as the stream starter.
     */
    void install() {
      webrtc::set_stream_starter([this](const webrtc::stream_request_t &requested, std::shared_ptr<webrtc::media_sink_t> supplied_sink, std::string &error) -> std::unique_ptr<webrtc::stream_handle_t> {
        ++calls;
        request = requested;
        sink = std::move(supplied_sink);
        if (fail) {
          error = "boom";
          return nullptr;
        }
        return std::make_unique<fake_stream_t>(stream);
      });
    }
  };

  /**
   * @brief A minimal Annex-B keyframe: SPS, PPS, then an IDR slice.
   */
  const std::string annexb_idr =
    "\x00\x00\x00\x01\x67\x64\x00\x1f\xac\xd9\x40"s +
    "\x00\x00\x00\x01\x68\xeb\xe3\xcb\x22\xc0"s +
    "\x00\x00\x00\x01\x65\x88\x84\x00\x10\x20\x30\x40\x50\x60\x70\x80"s;

  /**
   * @brief A minimal Annex-B delta frame: a single non-IDR slice.
   */
  const std::string annexb_delta = "\x00\x00\x00\x01\x41\x9a\x24\x6c\x42\x10\x20\x30"s;

  class WebrtcSessionTest: public BaseTest {
  protected:
    void SetUp() override {
      BaseTest::SetUp();
      webrtc::close_all();
    }

    void TearDown() override {
      webrtc::close_all();
      webrtc::set_stream_starter({});
      BaseTest::TearDown();
    }
  };
}  // namespace

TEST_F(WebrtcSessionTest, RejectsEmptyOffer) {
  std::string error;
  EXPECT_FALSE(webrtc::accept_offer("", "", {}, error));
  EXPECT_EQ(error, "Empty SDP offer");
  EXPECT_EQ(webrtc::session_count(), 0u);
}

TEST_F(WebrtcSessionTest, RejectsOversizedOffer) {
  std::string error;
  const std::string huge(webrtc::max_offer_size + 1, 'a');
  EXPECT_FALSE(webrtc::accept_offer(huge, "", {}, error));
  EXPECT_EQ(error, "SDP offer too large");
}

TEST_F(WebrtcSessionTest, RejectsGarbageOffer) {
  std::string error;
  EXPECT_FALSE(webrtc::accept_offer("this is not sdp", "", {}, error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(webrtc::session_count(), 0u);
}

TEST_F(WebrtcSessionTest, RejectsMalformedStunServer) {
  fake_browser_t browser;
  const auto offer = browser.make_offer();
  ASSERT_FALSE(offer.empty());

  // An unknown URL scheme is one libdatachannel rejects, unlike a bare hostname, which it accepts.
  std::string error;
  EXPECT_FALSE(webrtc::accept_offer(offer, "http://stun.example.com", {}, error));
  EXPECT_EQ(error, "The host's STUN server setting is invalid");
  EXPECT_EQ(webrtc::session_count(), 0u);
}

TEST_F(WebrtcSessionTest, CloseUnknownSessionReturnsFalse) {
  EXPECT_FALSE(webrtc::close_session("doesnotexist0000"));
}

TEST_F(WebrtcSessionTest, AcceptsOfferAndTracksSession) {
  fake_browser_t browser;
  const auto offer = browser.make_offer();
  ASSERT_FALSE(offer.empty());

  std::string error;
  const auto answer = webrtc::accept_offer(offer, "", {}, error);
  ASSERT_TRUE(answer.has_value()) << error;
  EXPECT_EQ(answer->id.size(), 16u);
  EXPECT_NE(answer->sdp.find("v=0"), std::string::npos);
  EXPECT_EQ(webrtc::session_count(), 1u);

  EXPECT_TRUE(webrtc::close_session(answer->id));
  EXPECT_EQ(webrtc::session_count(), 0u);
  EXPECT_FALSE(webrtc::close_session(answer->id));
}

TEST_F(WebrtcSessionTest, SessionIdsAreUnique) {
  fake_browser_t first;
  fake_browser_t second;
  const auto first_offer = first.make_offer();
  const auto second_offer = second.make_offer();
  ASSERT_FALSE(first_offer.empty());
  ASSERT_FALSE(second_offer.empty());

  std::string error;
  const auto a = webrtc::accept_offer(first_offer, "", {}, error);
  const auto b = webrtc::accept_offer(second_offer, "", {}, error);
  ASSERT_TRUE(a && b) << error;
  EXPECT_NE(a->id, b->id);
  EXPECT_EQ(webrtc::session_count(), 2u);
}

TEST_F(WebrtcSessionTest, EnforcesSessionLimit) {
  std::vector<std::unique_ptr<fake_browser_t>> browsers;
  std::string error;

  for (std::size_t i = 0; i < webrtc::max_sessions; ++i) {
    browsers.push_back(std::make_unique<fake_browser_t>());
    const auto offer = browsers.back()->make_offer();
    ASSERT_FALSE(offer.empty());
    ASSERT_TRUE(webrtc::accept_offer(offer, "", {}, error)) << error;
  }
  EXPECT_EQ(webrtc::session_count(), webrtc::max_sessions);

  browsers.push_back(std::make_unique<fake_browser_t>());
  const auto extra_offer = browsers.back()->make_offer();
  ASSERT_FALSE(extra_offer.empty());
  EXPECT_FALSE(webrtc::accept_offer(extra_offer, "", {}, error));
  EXPECT_EQ(error, "Too many browser sessions");
  EXPECT_EQ(webrtc::session_count(), webrtc::max_sessions);
}

TEST_F(WebrtcSessionTest, CloseAllClearsSessions) {
  fake_browser_t browser;
  const auto offer = browser.make_offer();
  ASSERT_FALSE(offer.empty());

  std::string error;
  ASSERT_TRUE(webrtc::accept_offer(offer, "", {}, error)) << error;
  webrtc::close_all();
  EXPECT_EQ(webrtc::session_count(), 0u);
}

TEST_F(WebrtcSessionTest, EchoesDataChannelMessages) {
  fake_browser_t browser;
  const auto offer = browser.make_offer();
  ASSERT_FALSE(offer.empty());

  std::string error;
  const auto answer = webrtc::accept_offer(offer, "", {}, error);
  ASSERT_TRUE(answer.has_value()) << error;

  browser.connection->setRemoteDescription(rtc::Description {answer->sdp, "answer"});

  // Loopback ICE needs a usable non-loopback interface. Sandboxes without one cannot connect, which
  // says nothing about this code, so skip rather than fail.
  if (!browser.wait(10s, [&] {
        return browser.open;
      })) {
    GTEST_SKIP() << "Loopback WebRTC connection could not be established in this environment";
  }

  browser.channel->send("hello"s);
  ASSERT_TRUE(browser.wait(5s, [&] {
    return !browser.received.empty();
  }));

  std::lock_guard lg {browser.mutex};
  EXPECT_EQ(browser.received.front(), "hello");
}

TEST_F(WebrtcSessionTest, DataChannelOnlyOfferStartsNoStream) {
  fake_starter_t starter;
  starter.install();

  fake_browser_t browser;
  const auto offer = browser.make_offer();
  ASSERT_FALSE(offer.empty());

  std::string error;
  ASSERT_TRUE(webrtc::accept_offer(offer, "", {}, error)) << error;
  EXPECT_EQ(starter.calls, 0);
}

TEST_F(WebrtcSessionTest, RejectsVideoOfferWithoutH264) {
  fake_starter_t starter;
  starter.install();

  fake_browser_t browser {video_e::vp8_only};
  const auto offer = browser.make_offer();
  ASSERT_FALSE(offer.empty());

  std::string error;
  EXPECT_FALSE(webrtc::accept_offer(offer, "", {}, error));
  EXPECT_EQ(error, "Browser does not support H.264 video");
  EXPECT_EQ(starter.calls, 0);
  EXPECT_EQ(webrtc::session_count(), 0u);
}

TEST_F(WebrtcSessionTest, VideoOfferStartsStreamWithTheRequest) {
  fake_starter_t starter;
  starter.install();

  fake_browser_t browser {video_e::h264_baseline};
  const auto offer = browser.make_offer();
  ASSERT_FALSE(offer.empty());

  webrtc::stream_request_t request;
  request.width = 1280;
  request.height = 720;
  request.fps = 30;
  request.bitrate_kbps = 8000;
  request.appid = 7;

  std::string error;
  const auto answer = webrtc::accept_offer(offer, "", request, error);
  ASSERT_TRUE(answer.has_value()) << error;

  EXPECT_EQ(starter.calls, 1);
  EXPECT_EQ(starter.request.width, 1280);
  EXPECT_EQ(starter.request.height, 720);
  EXPECT_EQ(starter.request.fps, 30);
  EXPECT_EQ(starter.request.bitrate_kbps, 8000);
  EXPECT_EQ(starter.request.appid, 7);
  EXPECT_NE(starter.sink, nullptr);

  EXPECT_NE(answer->sdp.find("m=video"), std::string::npos);
  EXPECT_NE(answer->sdp.find("H264"), std::string::npos);
  EXPECT_EQ(webrtc::session_count(), 1u);
}

TEST_F(WebrtcSessionTest, AnswersWithTheHighestOfferedProfile) {
  fake_starter_t starter;
  starter.install();

  fake_browser_t browser {video_e::h264_high_and_baseline};
  const auto offer = browser.make_offer();
  ASSERT_FALSE(offer.empty());

  std::string error;
  const auto answer = webrtc::accept_offer(offer, "", {}, error);
  ASSERT_TRUE(answer.has_value()) << error;

  // Sunshine encodes High profile, so that is the one to agree on. It was offered as payload type 98.
  EXPECT_NE(answer->sdp.find("profile-level-id=64001f"), std::string::npos);
  EXPECT_NE(answer->sdp.find("a=rtpmap:98 H264"), std::string::npos);
  EXPECT_EQ(answer->sdp.find("a=rtpmap:96 H264"), std::string::npos);
}

TEST_F(WebrtcSessionTest, StreamStartFailureFailsTheOffer) {
  fake_starter_t starter;
  starter.fail = true;
  starter.install();

  fake_browser_t browser {video_e::h264_baseline};
  const auto offer = browser.make_offer();
  ASSERT_FALSE(offer.empty());

  std::string error;
  EXPECT_FALSE(webrtc::accept_offer(offer, "", {}, error));
  EXPECT_EQ(error, "boom");
  EXPECT_EQ(starter.calls, 1);
  EXPECT_EQ(webrtc::session_count(), 0u);
}

TEST_F(WebrtcSessionTest, ClosingASessionStopsItsStream) {
  fake_starter_t starter;
  starter.install();

  fake_browser_t browser {video_e::h264_baseline};
  const auto offer = browser.make_offer();
  ASSERT_FALSE(offer.empty());

  std::string error;
  const auto answer = webrtc::accept_offer(offer, "", {}, error);
  ASSERT_TRUE(answer.has_value()) << error;
  EXPECT_EQ(starter.stream->stop_calls, 0);

  EXPECT_TRUE(webrtc::close_session(answer->id));
  EXPECT_GE(starter.stream->stop_calls, 1);
}

TEST_F(WebrtcSessionTest, StreamThatEndsOnItsOwnIsReapedInTheBackground) {
  fake_starter_t starter;
  starter.install();

  fake_browser_t browser {video_e::h264_baseline};
  const auto offer = browser.make_offer();
  ASSERT_FALSE(offer.empty());

  std::string error;
  ASSERT_TRUE(webrtc::accept_offer(offer, "", {}, error)) << error;

  // The host stream ends, for example because the encoder failed. Nothing calls into the session
  // layer after this, so only the background reaper can notice.
  starter.stream->running = false;

  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (starter.stream->stop_calls == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(50ms);
  }
  EXPECT_GE(starter.stream->stop_calls, 1);
  EXPECT_EQ(webrtc::session_count(), 0u);
}

TEST_F(WebrtcSessionTest, DeliversVideoToTheBrowserAsRtp) {
  fake_starter_t starter;
  starter.install();

  fake_browser_t browser {video_e::h264_baseline};
  const auto offer = browser.make_offer();
  ASSERT_FALSE(offer.empty());

  std::string error;
  const auto answer = webrtc::accept_offer(offer, "", {}, error);
  ASSERT_TRUE(answer.has_value()) << error;
  browser.connection->setRemoteDescription(rtc::Description {answer->sdp, "answer"});

  if (!browser.wait(10s, [&] {
        return browser.open;
      })) {
    GTEST_SKIP() << "Loopback WebRTC connection could not be established in this environment";
  }

  // A delta frame before any keyframe must be held back, and should prompt a keyframe request.
  for (int i = 0; i < 10 && starter.stream->idr_requests == 0; ++i) {
    starter.sink->video_frame(annexb_delta, false, i);
    std::this_thread::sleep_for(100ms);
  }
  EXPECT_GE(starter.stream->idr_requests, 1);
  {
    std::lock_guard lg {browser.mutex};
    EXPECT_TRUE(browser.rtp_packets.empty()) << "video was sent before a keyframe";
  }

  // Once a keyframe arrives, RTP should reach the browser.
  ASSERT_TRUE(browser.video_track->isOpen());
  for (int i = 0; i < 50; ++i) {
    starter.sink->video_frame(annexb_idr, true, 100 + i);
    {
      std::lock_guard lg {browser.mutex};
      if (!browser.rtp_packets.empty()) {
        break;
      }
    }
    std::this_thread::sleep_for(100ms);
  }

  std::lock_guard lg {browser.mutex};
  ASSERT_FALSE(browser.rtp_packets.empty());

  // RTP version 2, with the payload type the browser offered for H.264.
  const auto &packet = browser.rtp_packets.front();
  ASSERT_GE(packet.size(), 12u);
  EXPECT_EQ(static_cast<std::uint8_t>(packet[0]) >> 6, 2);
  EXPECT_EQ(static_cast<std::uint8_t>(packet[1]) & 0x7f, 96);
}

#endif  // SUNSHINE_BUILD_WEBRTC
