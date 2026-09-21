/**
 * @file src/webrtc/media.h
 * @brief Declarations for delivering encoded media to a browser over WebRTC.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>

// lib includes
#include <rtc/rtc.hpp>

// local includes
#include "sink.h"

namespace webrtc {
  /**
   * @brief Sends encoded H.264 video and Opus audio to a browser as RTP.
   *
   * Video frames are dropped until the browser's track is open and a keyframe arrives, so the browser
   * never starts decoding mid-GOP. Loss recovery is handled here: NACKs are answered from a
   * retransmission buffer, and a keyframe request (PLI) from the browser is passed on to the encoder.
   */
  class rtc_media_sink_t: public media_sink_t, public std::enable_shared_from_this<rtc_media_sink_t> {
  public:
    /**
     * @brief Create a sink that sends video on a track.
     *
     * @param track Video track negotiated with the browser.
     * @param payload_type RTP payload type negotiated for H.264.
     * @param ssrc RTP synchronization source announced in the SDP.
     * @return The sink, with its media handlers and callbacks installed.
     */
    static std::shared_ptr<rtc_media_sink_t> create(std::shared_ptr<rtc::Track> track, std::uint8_t payload_type, std::uint32_t ssrc);

    /**
     * @brief Also send audio, on a second track.
     *
     * Must be called before the stream starts delivering frames.
     *
     * @param track Audio track negotiated with the browser.
     * @param payload_type RTP payload type negotiated for Opus.
     * @param ssrc RTP synchronization source announced in the SDP.
     */
    void add_audio(std::shared_ptr<rtc::Track> track, std::uint8_t payload_type, std::uint32_t ssrc);

    /**
     * @brief Set the function used to ask the encoder for a keyframe.
     *
     * It is called when the track opens and whenever the browser reports it lost the picture.
     *
     * @param requester Function to call. Must not block.
     */
    void set_keyframe_requester(std::function<void()> requester);

    /**
     * @brief Packetize and send one video frame.
     *
     * @param payload Annex-B bitstream.
     * @param idr Whether this frame is a keyframe.
     * @param frame_index Encoder frame index.
     */
    void video_frame(std::string_view payload, bool idr, std::int64_t frame_index) override;

    /**
     * @brief Send one Opus packet.
     *
     * Packets are timestamped at a fixed spacing rather than by when they arrive. Opus depends on
     * regular timestamps, and delivery from the capture thread is not perfectly even.
     *
     * @param payload One Opus packet.
     */
    void audio_frame(std::string_view payload) override;

  private:
    /**
     * @brief Construct an unconfigured sink; use create().
     *
     * @param track Video track negotiated with the browser.
     */
    explicit rtc_media_sink_t(std::shared_ptr<rtc::Track> track);

    /**
     * @brief Install the packetizer chain and callbacks on the track.
     *
     * @param payload_type RTP payload type negotiated for H.264.
     * @param ssrc RTP synchronization source announced in the SDP.
     */
    void attach(std::uint8_t payload_type, std::uint32_t ssrc);

    /**
     * @brief Ask the encoder for a keyframe, if a requester is set.
     */
    void request_keyframe();

    std::shared_ptr<rtc::Track> track_;  ///< Track the video is sent on.

    std::mutex requester_mutex_;  ///< Guards `requester_`.
    std::function<void()> requester_;  ///< Asks the encoder for a keyframe.

    std::shared_ptr<rtc::Track> audio_track_;  ///< Track the audio is sent on, if the browser wants audio.
    std::int64_t audio_packets_sent_ = 0;  ///< Audio packets sent, which sets their RTP timestamps. Only touched by the audio thread.

    bool started_ = false;  ///< Whether the first keyframe has been sent. Only touched by the video thread.
    std::chrono::steady_clock::time_point first_frame_;  ///< Time of the first frame sent, the origin of RTP timestamps.
    std::chrono::steady_clock::time_point last_keyframe_request_;  ///< When a keyframe was last requested while waiting to start.
  };
}  // namespace webrtc
