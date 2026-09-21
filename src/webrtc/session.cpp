/**
 * @file src/webrtc/session.cpp
 * @brief Definitions for WebRTC peer sessions used by the browser streaming client.
 */
// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

// lib includes
#include <openssl/rand.h>
#include <rtc/rtc.hpp>

// local includes
#include "input_translate.h"
#include "media.h"
#include "session.h"
#include "src/crypto.h"
#include "src/logging.h"

using namespace std::literals;

namespace webrtc {
  namespace {
    constexpr auto gathering_timeout = 5s;  ///< How long to wait for ICE candidate gathering.
    constexpr auto connect_timeout = 30s;  ///< How long a session may sit unconnected before it is reaped.
    constexpr auto reap_interval = 1s;  ///< How often finished sessions are cleaned up.
    constexpr auto ping_channel_label = "ping"sv;  ///< Label of the data channel that echoes, for measuring latency.
    constexpr auto input_channel_label = "input"sv;  ///< Label of the data channel that carries keyboard, mouse and controller input.
    const std::string media_stream_id {"sunshine"};  ///< Stream that the video and audio tracks both belong to, so a browser keeps them in sync.

    /**
     * @brief State shared between a peer connection's callbacks and the code that owns it.
     *
     * Callbacks run on libdatachannel's threads and can outlive the call that created them, so
     * they hold this by shared pointer rather than referencing the owner directly.
     */
    class peer_state_t {
    public:
      std::atomic_bool connected {false};  ///< Whether the connection has reached the connected state.
      std::atomic_bool dead {false};  ///< Whether the connection has failed or closed.

      /**
       * @brief Record that the connection is gone, and stop its host stream.
       *
       * Stopping here, on the callback thread, is what keeps a closed browser tab from leaving the
       * encoder running until something else notices.
       */
      void mark_dead() {
        dead = true;

        std::shared_ptr<stream_handle_t> stream;
        {
          std::lock_guard lg {mutex_};
          stream = stream_.lock();
        }
        if (stream) {
          stream->stop();
        }
      }

      /**
       * @brief Pass a browser's input packet to its host stream.
       *
       * Input that arrives before the stream exists, or after the connection died, is dropped.
       *
       * @param packet A complete Moonlight input packet.
       */
      void send_input(std::vector<std::uint8_t> &&packet) {
        if (dead) {
          return;
        }

        std::shared_ptr<stream_handle_t> stream;
        {
          std::lock_guard lg {mutex_};
          stream = stream_.lock();
        }
        if (stream) {
          stream->send_input(std::move(packet));
        }
      }

      /**
       * @brief Note a rejected input message, and say whether it is worth logging.
       *
       * A misbehaving page could send thousands, so only the first few are reported.
       *
       * @return `true` for the first few rejections on this connection.
       */
      bool should_log_rejected_input() {
        return ++rejected_input_ <= max_logged_rejections;
      }

      /**
       * @brief Keep a data channel alive for as long as the connection.
       *
       * libdatachannel only holds channels weakly, so a channel nobody owns is destroyed as soon as
       * its creation callback returns, silently discarding everything sent on it.
       *
       * @param channel The channel the browser opened.
       * @return `false` if the browser has opened too many channels and this one was refused.
       */
      bool keep_channel(std::shared_ptr<rtc::DataChannel> channel) {
        std::lock_guard lg {mutex_};
        if (channels_.size() >= max_channels) {
          return false;
        }
        channels_.push_back(std::move(channel));
        return true;
      }

      /**
       * @brief Tie a host stream to this connection.
       *
       * @param stream The stream to stop if the connection dies.
       */
      void attach_stream(const std::shared_ptr<stream_handle_t> &stream) {
        {
          std::lock_guard lg {mutex_};
          stream_ = stream;
        }
        // The connection may have died before the stream existed.
        if (dead) {
          stream->stop();
        }
      }

    private:
      static constexpr std::size_t max_channels = 8;  ///< Most data channels a browser may open.
      static constexpr int max_logged_rejections = 5;  ///< How many rejected input messages are logged per connection.

      std::atomic_int rejected_input_ {0};  ///< Input messages refused on this connection.

      std::mutex mutex_;  ///< Guards `stream_` and `channels_`.
      std::weak_ptr<stream_handle_t> stream_;  ///< The host stream to stop when the connection dies.
      std::vector<std::shared_ptr<rtc::DataChannel>> channels_;  ///< Channels kept alive for the life of the connection.
    };

    /**
     * @brief An open browser session.
     */
    struct peer_t {
      std::shared_ptr<rtc::PeerConnection> connection;  ///< The underlying peer connection.
      std::shared_ptr<peer_state_t> state;  ///< State updated by the connection's callbacks.
      std::shared_ptr<stream_handle_t> stream;  ///< The host stream feeding this browser, if any.
      std::chrono::steady_clock::time_point created;  ///< When the session was accepted.
    };

    /**
     * @brief Synchronization for waiting on ICE candidate gathering.
     */
    struct gathering_t {
      std::mutex mutex;  ///< Guards `complete`.
      std::condition_variable cv;  ///< Signaled when gathering completes.
      bool complete = false;  ///< Whether gathering has finished.
    };

    /**
     * @brief The H.264 codec chosen from a browser's offer.
     */
    struct h264_choice_t {
      std::string mid;  ///< Media section identifier the answer must reuse.
      int payload_type;  ///< Payload type the browser assigned to H.264.
      std::string fmtp;  ///< The browser's own format parameters, echoed back so the profile matches.
    };

    /**
     * @brief The Opus codec chosen from a browser's offer.
     */
    struct opus_choice_t {
      std::string mid;  ///< Media section identifier the answer must reuse.
      int payload_type;  ///< Payload type the browser assigned to Opus.
    };

    /**
     * @brief What the browser's offer asks for in the way of video and audio.
     */
    struct media_offer_t {
      bool video_present = false;  ///< Whether the offer has a video section that wants to receive.
      std::optional<h264_choice_t> h264;  ///< The H.264 codec to use, if the browser offered a usable one.
      bool audio_present = false;  ///< Whether the offer has an audio section that wants to receive.
      std::optional<opus_choice_t> opus;  ///< The Opus codec to use, if the browser offered it.
    };

    std::mutex sessions_mutex;  ///< Guards `sessions`.
    std::map<std::string, peer_t, std::less<>> sessions;  ///< Open sessions keyed by identifier.

    std::mutex starter_mutex;  ///< Guards `starter_override`.
    stream_starter_t starter_override;  ///< Replacement for the real stream starter, set by tests.

    /**
     * @brief Get the function that starts host streams.
     *
     * @return The override if one is set, otherwise the real starter.
     */
    stream_starter_t current_starter() {
      {
        std::lock_guard lg {starter_mutex};
        if (starter_override) {
          return starter_override;
        }
      }
      return start_host_stream;
    }

    /**
     * @brief How much a browser's H.264 profile is preferred.
     *
     * Sunshine's encoders produce High profile, so a matching profile is the safest to answer with.
     *
     * @param fmtp Format parameters from the offer.
     * @return Higher is better; zero means the parameters are unusable.
     */
    int h264_preference(const std::string &fmtp) {
      // Only packetization mode 1 (non-interleaved) is supported by the packetizer.
      if (fmtp.find("packetization-mode=1") == std::string::npos) {
        return 0;
      }

      constexpr auto key = "profile-level-id="sv;
      const auto at = fmtp.find(key);
      if (at == std::string::npos || fmtp.size() < at + key.size() + 2) {
        return 1;
      }

      // The first byte of profile-level-id is profile_idc: 0x64 High, 0x4d Main, 0x42 Baseline.
      const auto profile = fmtp.substr(at + key.size(), 2);
      if (profile == "64") {
        return 4;
      }
      if (profile == "4d") {
        return 3;
      }
      return 2;
    }

    /**
     * @brief Whether a media section is one that wants to receive from us.
     *
     * @param media The media section.
     * @return `true` if the browser offered to receive.
     */
    bool wants_to_receive(const rtc::Description::Media &media) {
      using direction_e = rtc::Description::Direction;
      return media.direction() == direction_e::RecvOnly || media.direction() == direction_e::SendRecv;
    }

    /**
     * @brief Find what the offer wants for video and audio, and the codecs to answer with.
     *
     * Only the first section of each kind is answered.
     *
     * @param offer The browser's offer.
     * @return Whether video and audio are wanted, and which codecs to answer with.
     */
    media_offer_t inspect_media(rtc::Description &offer) {
      media_offer_t result;
      int best_h264 = 0;

      for (int i = 0; i < offer.mediaCount(); ++i) {
        auto entry = offer.media(i);
        const auto *media_pp = std::get_if<rtc::Description::Media *>(&entry);
        if (!media_pp || !wants_to_receive(**media_pp)) {
          continue;
        }

        auto *media = *media_pp;

        if (media->type() == "video" && !result.video_present) {
          result.video_present = true;

          for (const int payload_type : media->payloadTypes()) {
            const auto *map = media->rtpMap(payload_type);
            if (!map || map->format != "H264" || map->fmtps.empty()) {
              continue;
            }

            const int preference = h264_preference(map->fmtps.front());
            if (preference > best_h264) {
              best_h264 = preference;
              result.h264 = h264_choice_t {media->mid(), payload_type, map->fmtps.front()};
            }
          }
        } else if (media->type() == "audio" && !result.audio_present) {
          result.audio_present = true;

          for (const int payload_type : media->payloadTypes()) {
            const auto *map = media->rtpMap(payload_type);
            // Codec names are case-insensitive in SDP, and browsers write it as "opus".
            if (map && (map->format == "opus" || map->format == "OPUS" || map->format == "Opus") && map->clockRate == 48000) {
              result.opus = opus_choice_t {media->mid(), payload_type};
              break;
            }
          }
        }
      }

      return result;
    }

    /**
     * @brief Whether a session should be dropped.
     *
     * @param peer Session to check.
     * @param now Current time.
     * @return `true` if the connection died, its host stream ended, or it never connected in time.
     */
    bool is_stale(const peer_t &peer, std::chrono::steady_clock::time_point now) {
      if (peer.state->dead) {
        return true;
      }
      if (peer.stream && !peer.stream->running()) {
        return true;
      }
      return !peer.state->connected && now - peer.created > connect_timeout;
    }

    /**
     * @brief Remove stale sessions, moving them out so they are destroyed outside the lock.
     *
     * A peer connection must not be destroyed from one of its own callbacks, so reaping happens
     * here, on the caller's thread, rather than in the state-change callback.
     *
     * @param graveyard Receives the removed sessions.
     */
    void reap_locked(std::vector<peer_t> &graveyard) {
      const auto now = std::chrono::steady_clock::now();
      for (auto it = sessions.begin(); it != sessions.end();) {
        if (is_stale(it->second, now)) {
          graveyard.push_back(std::move(it->second));
          it = sessions.erase(it);
        } else {
          ++it;
        }
      }
    }

    /**
     * @brief Stop and close sessions that have been removed from the session table.
     *
     * @param graveyard Sessions to close.
     */
    void close_peers(std::vector<peer_t> &graveyard) {
      for (auto &peer : graveyard) {
        try {
          if (peer.stream) {
            peer.stream->stop();
          }
          peer.connection->close();
        } catch (const std::exception &e) {
          BOOST_LOG(debug) << "WebRTC: error closing session: "sv << e.what();
        }
      }
      graveyard.clear();
    }

    /**
     * @brief Drop every stale session.
     */
    void reap_now() {
      std::vector<peer_t> graveyard;
      {
        std::lock_guard lg {sessions_mutex};
        reap_locked(graveyard);
      }
      close_peers(graveyard);
    }

    /**
     * @brief Background thread that reaps finished sessions.
     *
     * Without it, a browser that simply closes its tab would leave the encoder running and the
     * display reconfigured until some unrelated request happened to trigger a cleanup.
     */
    class reaper_t {
    public:
      reaper_t():
          thread_ {[](std::stop_token stop) {
            std::mutex mutex;
            std::condition_variable_any cv;
            while (!stop.stop_requested()) {
              {
                std::unique_lock lock {mutex};
                cv.wait_for(lock, stop, reap_interval, [] {
                  return false;
                });
              }
              reap_now();
            }
          }} {
      }

    private:
      std::jthread thread_;  ///< Requests stop and joins when the reaper is destroyed.
    };

    /**
     * @brief Start the reaper thread if it is not already running.
     */
    void ensure_reaper() {
      // Constructed after `sessions`, so it is destroyed, and its thread joined, before `sessions` is.
      static reaper_t reaper;
    }
  }  // namespace

  void set_stream_starter(stream_starter_t starter) {
    std::lock_guard lg {starter_mutex};
    starter_override = std::move(starter);
  }

  std::optional<answer_t> accept_offer(std::string_view offer_sdp, const std::string &stun_server, const stream_request_t &request, std::string &error) {
    if (offer_sdp.empty()) {
      error = "Empty SDP offer";
      return std::nullopt;
    }
    if (offer_sdp.size() > max_offer_size) {
      error = "SDP offer too large";
      return std::nullopt;
    }

    ensure_reaper();

    // Drop finished sessions first, so they do not count against the limit.
    reap_now();
    {
      std::lock_guard lg {sessions_mutex};
      if (sessions.size() >= max_sessions) {
        error = "Too many browser sessions";
        return std::nullopt;
      }
    }

    std::shared_ptr<rtc::PeerConnection> connection;
    try {
      rtc::Description offer {std::string {offer_sdp}, "offer"};
      const auto media = inspect_media(offer);

      if (media.video_present && !media.h264) {
        error = "Browser does not support H.264 video";
        return std::nullopt;
      }
      if (media.audio_present && !media.opus) {
        error = "Browser does not support Opus audio";
        return std::nullopt;
      }

      rtc::Configuration config;
      if (!stun_server.empty()) {
        // This comes from the host's own settings, so a bad value is not the browser's fault.
        try {
          config.iceServers.emplace_back(stun_server);
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "WebRTC: invalid webrtc_stun_server setting: "sv << e.what();
          error = "The host's STUN server setting is invalid";
          return std::nullopt;
        }
      }

      connection = std::make_shared<rtc::PeerConnection>(config);
      auto state = std::make_shared<peer_state_t>();
      auto gathering = std::make_shared<gathering_t>();

      connection->onStateChange([state](rtc::PeerConnection::State new_state) {
        using state_e = rtc::PeerConnection::State;
        if (new_state == state_e::Connected) {
          state->connected = true;
        } else if (new_state == state_e::Failed || new_state == state_e::Closed) {
          state->mark_dead();
        }
      });

      connection->onGatheringStateChange([gathering](rtc::PeerConnection::GatheringState new_state) {
        if (new_state == rtc::PeerConnection::GatheringState::Complete) {
          {
            std::lock_guard lg {gathering->mutex};
            gathering->complete = true;
          }
          gathering->cv.notify_all();
        }
      });

      // The browser opens a channel for each purpose, and the label says which. Anything else is
      // refused, so a page cannot use the connection for something it was not meant for.
      connection->onDataChannel([state](std::shared_ptr<rtc::DataChannel> channel) {
        const auto label = channel->label();
        std::weak_ptr<rtc::DataChannel> weak {channel};

        if (label == ping_channel_label) {
          // Echoes everything back, so the browser can measure the round trip.
          channel->onMessage([weak](rtc::message_variant message) {
            if (auto locked = weak.lock()) {
              locked->send(std::move(message));
            }
          });
        } else if (label == input_channel_label) {
          channel->onMessage([state](rtc::message_variant message) {
            std::string error;
            const auto *text = std::get_if<std::string>(&message);
            const auto packet = text ? translate_input(*text, error) : std::nullopt;
            if (!packet) {
              if (state->should_log_rejected_input()) {
                BOOST_LOG(warning) << "WebRTC: ignoring browser input: "sv << (text ? error : "input must be sent as text"s);
              }
              return;
            }
            state->send_input(std::vector<std::uint8_t> {*packet});
          });
        } else {
          BOOST_LOG(warning) << "WebRTC: refusing a data channel with an unknown label"sv;
          channel->close();
          return;
        }

        if (!state->keep_channel(channel)) {
          BOOST_LOG(warning) << "WebRTC: refusing a data channel, the browser opened too many"sv;
          channel->close();
        }
      });

      // The video track has to exist before the offer is applied, so the answer can include it.
      std::shared_ptr<rtc_media_sink_t> sink;
      if (media.h264) {
        std::uint32_t ssrc = 0;
        RAND_bytes(reinterpret_cast<unsigned char *>(&ssrc), sizeof(ssrc));

        // Reuse the browser's own format parameters so the answered profile matches what it offered.
        rtc::Description::Video track_description {media.h264->mid, rtc::Description::Direction::SendOnly};
        track_description.addH264Codec(media.h264->payload_type, media.h264->fmtp);
        // Both tracks name the same stream, which is what lets the browser play audio and video in sync.
        track_description.addSSRC(ssrc, "sunshine", media_stream_id, "video");

        auto track = connection->addTrack(track_description);
        sink = rtc_media_sink_t::create(std::move(track), static_cast<std::uint8_t>(media.h264->payload_type), ssrc);

        // Audio rides along with a stream, and is only added if the browser asked for it.
        if (media.opus) {
          std::uint32_t audio_ssrc = 0;
          RAND_bytes(reinterpret_cast<unsigned char *>(&audio_ssrc), sizeof(audio_ssrc));

          rtc::Description::Audio audio_description {media.opus->mid, rtc::Description::Direction::SendOnly};
          audio_description.addOpusCodec(media.opus->payload_type);
          audio_description.addSSRC(audio_ssrc, "sunshine", media_stream_id, "audio");

          sink->add_audio(connection->addTrack(audio_description), static_cast<std::uint8_t>(media.opus->payload_type), audio_ssrc);
        }
      }

      // Auto-negotiation turns this offer into a local answer and begins gathering.
      connection->setRemoteDescription(offer);

      {
        std::unique_lock lock {gathering->mutex};
        if (!gathering->cv.wait_for(lock, gathering_timeout, [&] {
              return gathering->complete;
            })) {
          error = "Timed out gathering ICE candidates";
          lock.unlock();
          connection->close();
          return std::nullopt;
        }
      }

      auto local = connection->localDescription();
      if (!local) {
        error = "No answer was produced";
        connection->close();
        return std::nullopt;
      }

      answer_t answer {crypto::rand_alphabet(16, "abcdefghijklmnopqrstuvwxyz0123456789"sv), std::string {*local}};

      // Record the session before starting the stream, so the limit is enforced and a stream that
      // starts is always owned by something that will stop it.
      {
        std::lock_guard lg {sessions_mutex};
        if (sessions.size() >= max_sessions) {
          error = "Too many browser sessions";
          connection->close();
          return std::nullopt;
        }
        sessions.emplace(answer.id, peer_t {connection, state, nullptr, std::chrono::steady_clock::now()});
      }

      if (sink) {
        std::string start_error;
        std::shared_ptr<stream_handle_t> stream = current_starter()(request, sink, start_error);
        if (!stream) {
          error = start_error.empty() ? "Failed to start the stream" : start_error;
          close_session(answer.id);
          return std::nullopt;
        }

        std::weak_ptr<stream_handle_t> weak_stream {stream};
        sink->set_keyframe_requester([weak_stream]() {
          if (auto locked = weak_stream.lock()) {
            locked->request_idr();
          }
        });

        state->attach_stream(stream);
        {
          std::lock_guard lg {sessions_mutex};
          if (auto it = sessions.find(answer.id); it != sessions.end()) {
            it->second.stream = stream;
          } else {
            // The session was closed while the stream was starting.
            stream->stop();
          }
        }
      }

      BOOST_LOG(info) << "WebRTC: accepted browser session "sv << answer.id << (sink ? (media.opus ? " with video and audio"sv : " with video"sv) : ""sv);
      return answer;
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "WebRTC: failed to accept offer: "sv << e.what();
      error = "Invalid or unsupported SDP offer";
      if (connection) {
        try {
          connection->close();
        } catch (...) {
        }
      }
      return std::nullopt;
    }
  }

  bool close_session(const std::string &id) {
    std::vector<peer_t> graveyard;
    {
      std::lock_guard lg {sessions_mutex};
      auto it = sessions.find(id);
      if (it == sessions.end()) {
        return false;
      }
      graveyard.push_back(std::move(it->second));
      sessions.erase(it);
    }
    close_peers(graveyard);
    BOOST_LOG(info) << "WebRTC: closed browser session "sv << id;
    return true;
  }

  void close_all() {
    std::vector<peer_t> graveyard;
    {
      std::lock_guard lg {sessions_mutex};
      for (auto &[id, peer] : sessions) {
        graveyard.push_back(std::move(peer));
      }
      sessions.clear();
    }
    close_peers(graveyard);
  }

  std::size_t session_count() {
    reap_now();

    std::lock_guard lg {sessions_mutex};
    return sessions.size();
  }
}  // namespace webrtc
