/**
 * @file src/webrtc/host_stream.cpp
 * @brief Definitions for starting a host streaming session on behalf of a browser.
 */
// standard includes
#include <atomic>
#include <string>

// lib includes
#include <openssl/rand.h>

// local includes
#include "session.h"
#include "src/config.h"
#include "src/crypto.h"
#include "src/display_device.h"
#include "src/display_layout.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/process.h"
#include "src/rtsp.h"
#include "src/stream.h"
#include "src/utility.h"
#include "src/video.h"

using namespace std::literals;

namespace webrtc {
  namespace {
    /**
     * @brief First launch-session identifier handed to browser sessions.
     *
     * GameStream launches count up from zero, so starting high keeps the two from colliding.
     */
    constexpr std::uint32_t first_browser_launch_id = 0x40000000;

    std::atomic<std::uint32_t> next_launch_id {first_browser_launch_id};  ///< Source of unique launch-session identifiers.

    /**
     * @brief Speaker mask for a stereo pair: front left and front right.
     */
    constexpr int stereo_speaker_mask = 0x3;

    /**
     * @brief Colorspace and range selector meaning Rec. 709, limited range.
     *
     * The value is `(colorspace << 1) | full_range`, and browsers assume Rec. 709 limited for H.264.
     */
    constexpr int csc_rec709_limited = 2;

    /**
     * @brief Controls a streaming session started for a browser.
     */
    class host_stream_t: public stream_handle_t {
    public:
      /**
       * @brief Wrap a running session.
       *
       * @param session The session to control.
       */
      explicit host_stream_t(std::shared_ptr<stream::session_t> session):
          session_ {std::move(session)} {
      }

      bool running() override {
        return stream::session::state(*session_) == stream::session::state_e::RUNNING;
      }

      void stop() override {
        stream::session::stop(*session_);
      }

      void request_idr() override {
        stream::session::request_idr(*session_);
      }

      void send_input(std::vector<std::uint8_t> &&packet) override {
        stream::session::passthrough_input(*session_, std::move(packet));
      }

    private:
      std::shared_ptr<stream::session_t> session_;  ///< The session being controlled.
    };

    /**
     * @brief Build the launch state for a browser session.
     *
     * The GameStream keys and ping payloads are generated locally because no client supplies them;
     * the media never travels over the GameStream transport, so they are never used to send data.
     *
     * @param request What the browser wants to stream.
     * @return Launch state for the session.
     */
    std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(const stream_request_t &request) {
      auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

      launch_session->id = next_launch_id++;

      const auto key = crypto::rand(16);
      launch_session->gcm_key.assign(key.begin(), key.end());
      launch_session->iv.assign(16, 0);

      launch_session->host_audio = false;
      launch_session->width = request.width;
      launch_session->height = request.height;
      launch_session->fps = request.fps;
      launch_session->appid = request.appid;
      launch_session->gcmap = 0;
      launch_session->enable_sops = false;
      launch_session->enable_hdr = false;
      launch_session->continuous_audio = false;

      // Each browser gets its own input identity, so one browser's held keys are never carried over
      // to another.
      launch_session->unique_id = "webrtc-"s + std::to_string(launch_session->id);
      launch_session->client_name = "Browser"s;
      launch_session->rtsp_url_scheme = "rtsp://"s;

      unsigned char ping[8];
      RAND_bytes(ping, sizeof(ping));
      launch_session->av_ping_payload = util::hex_vec(ping);
      RAND_bytes(reinterpret_cast<unsigned char *>(&launch_session->control_connect_data), sizeof(launch_session->control_connect_data));

      return launch_session;
    }

    /**
     * @brief Build the stream configuration for a browser session.
     *
     * @param request What the browser wants to stream.
     * @return Stream configuration. H.264, 8-bit 4:2:0, since that is what browsers decode everywhere.
     */
    stream::config_t make_stream_config(const stream_request_t &request) {
      stream::config_t config {};

      config.audio.channels = 2;
      config.audio.mask = stereo_speaker_mask;
      config.audio.packetDuration = audio_packet_duration_ms;

      config.monitor.width = request.width;
      config.monitor.height = request.height;
      config.monitor.framerate = request.fps;
      config.monitor.framerateX100 = request.fps * 100;
      config.monitor.bitrate = request.bitrate_kbps;
      config.monitor.slicesPerFrame = 1;
      config.monitor.numRefFrames = 0;
      config.monitor.encoderCscMode = csc_rec709_limited;
      config.monitor.videoFormat = 0;
      config.monitor.dynamicRange = 0;
      config.monitor.chromaSamplingType = 0;
      config.monitor.enableIntraRefresh = 0;

      // The fields below only matter to the GameStream transport, which this session does not use.
      config.packetsize = 1024;
      config.minRequiredFecPackets = 0;
      config.mlFeatureFlags = 0;
      config.controlProtocolType = 13;
      config.audioQosType = 0;
      config.videoQosType = 0;
      config.encryptionFlagsEnabled = 0;

      return config;
    }

    /**
     * @brief Point the host's display at the requested mode.
     *
     * This mirrors what nvhttp::launch does for a Moonlight client, and like it, only runs when no
     * other stream is active, since it changes the displays for everyone.
     *
     * @param launch_session Launch state describing the requested mode.
     */
    void prepare_display(const rtsp_stream::launch_session_t &launch_session) {
      display_device::configure_display(config::video, launch_session);

      if (config::sunshine.layout_management_enabled) {
        display_layout::apply_streaming_layout();
      }

      // Applied after the layout, so the target output is already enabled if a streaming layout just turned it on.
      if (launch_session.width <= 0 || launch_session.height <= 0 || launch_session.fps <= 0) {
        return;
      }

      auto output_id = display_device::map_output_name(config::video.output_name);
      if (output_id.empty()) {
        // No explicit output is configured, so use whichever one is enabled now, preferring the primary.
        for (const auto &output : platf::enum_display_outputs()) {
          if (!output.enabled) {
            continue;
          }
          output_id = output.id;
          if (output.primary) {
            break;
          }
        }
      }

      if (!output_id.empty() && !platf::set_display_resolution(output_id, launch_session.width, launch_session.height, static_cast<double>(launch_session.fps))) {
        BOOST_LOG(warning) << "WebRTC: failed to match the requested resolution "sv << launch_session.width << 'x' << launch_session.height << "; streaming at the display's current resolution instead"sv;
      }
    }
  }  // namespace

  std::unique_ptr<stream_handle_t> start_host_stream(const stream_request_t &request, std::shared_ptr<media_sink_t> sink, std::string &error) {
    auto launch_session = make_launch_session(request);

    // Undo display changes if anything below fails and no other stream is relying on them.
    bool revert_display_configuration = false;
    auto revert_guard = util::fail_guard([&]() {
      if (revert_display_configuration) {
        display_device::revert_configuration();
      }
    });

    if (rtsp_stream::session_count() == 0) {
      revert_display_configuration = true;

      prepare_display(*launch_session);

      // The active GPU may have changed since the last stream, so probe again before capturing.
      if (video::probe_encoders()) {
        error = "Failed to initialize video capture/encoding. Is a display connected and turned on?";
        return nullptr;
      }
    }

    // Like a Moonlight resume, join whatever is already running rather than starting a second app.
    if (request.appid > 0 && proc::proc.running() <= 0) {
      if (proc::proc.execute(request.appid, launch_session)) {
        error = "Failed to start the specified application";
        return nullptr;
      }
    }

    auto config = make_stream_config(request);
    auto session = rtsp_stream::start_direct_session(config, *launch_session, std::move(sink));
    if (!session) {
      error = "Failed to start the streaming session";
      return nullptr;
    }

    // The stream now owns the display changes, and reverts them when it ends.
    revert_display_configuration = false;
    return std::make_unique<host_stream_t>(std::move(session));
  }
}  // namespace webrtc
