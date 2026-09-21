/**
 * @file src/webrtc/session.h
 * @brief Declarations for WebRTC peer sessions used by the browser streaming client.
 */
#pragma once

// standard includes
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// local includes
#include "sink.h"

namespace webrtc {
  /**
   * @brief Maximum number of browser sessions that may be open at once.
   */
  constexpr std::size_t max_sessions = 4;

  /**
   * @brief Maximum accepted size of a browser-supplied SDP offer, in bytes.
   */
  constexpr std::size_t max_offer_size = 64 * 1024;

  /**
   * @brief What the browser wants to stream.
   */
  struct stream_request_t {
    int width = 1920;  ///< Requested stream width in pixels.
    int height = 1080;  ///< Requested stream height in pixels.
    int fps = 60;  ///< Requested frame rate.
    int bitrate_kbps = 20000;  ///< Requested video bitrate in kilobits per second.
    int appid = 0;  ///< Application to launch; zero or less streams the desktop as it is.
  };

  /**
   * @brief Control over a running host streaming session.
   *
   * This is what lets the browser session layer drive a stream without knowing how it is
   * implemented, and lets tests substitute a fake.
   */
  class stream_handle_t {
  public:
    virtual ~stream_handle_t() = default;

    /**
     * @brief Whether the stream is still running.
     *
     * @return `true` until the stream has stopped, for any reason.
     */
    virtual bool running() = 0;

    /**
     * @brief Stop the stream. Must not block.
     */
    virtual void stop() = 0;

    /**
     * @brief Ask the encoder for a keyframe. Must not block.
     */
    virtual void request_idr() = 0;
  };

  /**
   * @brief Function that starts a host stream that delivers its media to a sink.
   *
   * @param request What to stream.
   * @param sink Receiver of the encoded media.
   * @param error Receives a human readable reason on failure.
   * @return A handle to the running stream, or `nullptr` on failure with `error` set.
   */
  using stream_starter_t = std::function<std::unique_ptr<stream_handle_t>(const stream_request_t &request, std::shared_ptr<media_sink_t> sink, std::string &error)>;

  /**
   * @brief Replace the function used to start host streams.
   *
   * The default starts a real Sunshine streaming session. Tests replace it with a fake.
   *
   * @param starter The new function, or an empty function to restore the default.
   */
  void set_stream_starter(stream_starter_t starter);

  /**
   * @brief Start a real host stream that delivers its media to a sink.
   *
   * Prepares the host the same way a Moonlight launch does: display configuration, streaming
   * layout, resolution matching, encoder probing, and launching the requested application.
   *
   * @param request What to stream.
   * @param sink Receiver of the encoded media.
   * @param error Receives a human readable reason on failure.
   * @return A handle to the running stream, or `nullptr` on failure with `error` set.
   */
  std::unique_ptr<stream_handle_t> start_host_stream(const stream_request_t &request, std::shared_ptr<media_sink_t> sink, std::string &error);

  /**
   * @brief Result of successfully accepting a browser's SDP offer.
   */
  struct answer_t {
    std::string id;  ///< Identifier the browser uses to close the session later.
    std::string sdp;  ///< Complete SDP answer, with all ICE candidates already gathered.
  };

  /**
   * @brief Accept an SDP offer from a browser and open a peer connection for it.
   *
   * ICE is not trickled: the answer is only returned once candidate gathering has finished, so
   * the browser needs no further signaling round trips.
   *
   * If the offer contains a video section, it must offer H.264, and a host stream is started for it.
   * An offer without video opens only the data channel and starts no stream.
   *
   * @param offer_sdp The browser's SDP offer.
   * @param stun_server Optional STUN server URL (for example `stun:host:19302`). Empty means host candidates only.
   * @param request What to stream, used when the offer contains video.
   * @param error Receives a human readable reason when no answer is produced.
   * @return The answer, or `std::nullopt` on failure, in which case `error` is set.
   */
  std::optional<answer_t> accept_offer(std::string_view offer_sdp, const std::string &stun_server, const stream_request_t &request, std::string &error);

  /**
   * @brief Close and forget a session.
   *
   * @param id Identifier returned by accept_offer().
   * @return `true` if the session existed.
   */
  bool close_session(const std::string &id);

  /**
   * @brief Close every open session.
   */
  void close_all();

  /**
   * @brief Count sessions that are still usable.
   *
   * Sessions whose connection has failed or closed are reaped first, so they are not counted.
   *
   * @return Number of live sessions.
   */
  std::size_t session_count();
}  // namespace webrtc
