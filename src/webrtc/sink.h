/**
 * @file src/webrtc/sink.h
 * @brief Declarations for the receiver of encoded media from a streaming session.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string_view>

namespace webrtc {
  /**
   * @brief Duration of each audio packet a browser session asks the host to produce, in milliseconds.
   *
   * GameStream's default of 5 ms would mean 200 packets a second for no benefit over WebRTC, so
   * browser sessions use 20 ms, which is also what Opus is normally carried at. The audio sender
   * relies on it to timestamp packets, so the two must agree.
   */
  constexpr int audio_packet_duration_ms = 20;

  /**
   * @brief Receives encoded audio and video in place of the GameStream UDP path.
   *
   * A streaming session with a sink attached does not packetize, FEC-protect, encrypt or send its
   * media itself. Instead each encoded frame is handed here. This header deliberately depends on
   * nothing but the standard library, so the streaming code can use it whether or not the browser
   * client is built.
   *
   * Methods are called from the streaming broadcast threads. Implementations must return quickly
   * and must not block on the network.
   */
  class media_sink_t {
  public:
    virtual ~media_sink_t() = default;

    /**
     * @brief Accept one encoded video frame.
     *
     * The payload is only valid for the duration of the call, so implementations that keep it
     * must copy it.
     *
     * @param payload Annex-B bitstream with in-band parameter sets on keyframes.
     * @param idr Whether this frame is a keyframe.
     * @param frame_index Encoder frame index, increasing by one per frame.
     */
    virtual void video_frame(std::string_view payload, bool idr, std::int64_t frame_index) = 0;

    /**
     * @brief Accept one encoded audio frame.
     *
     * The payload is only valid for the duration of the call, so implementations that keep it
     * must copy it.
     *
     * @param payload One Opus packet.
     */
    virtual void audio_frame(std::string_view payload) = 0;
  };
}  // namespace webrtc
