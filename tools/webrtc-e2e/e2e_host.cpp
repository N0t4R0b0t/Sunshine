/**
 * @file tools/webrtc-e2e/e2e_host.cpp
 * @brief Host for end-to-end tests of the browser streaming client that needs no display and no user.
 *
 * It runs Sunshine's real browser session layer (`src/webrtc/session.cpp` and `media.cpp`) but replaces
 * the part that would capture the desktop with ffmpeg, which encodes a test pattern to real H.264.
 * That keeps everything a browser can observe genuine (the SDP negotiation, the RTP packetization,
 * loss recovery, and the decoded picture) without touching anyone's monitors.
 *
 * It is driven over standard input and output, one JSON object per line.
 *
 * Requests:
 *   {"cmd":"offer","req":1,"sdp":"...","width":640,"height":360,"fps":30,"die_after_s":0}
 *   {"cmd":"close","req":2,"id":"..."}
 *   {"cmd":"count","req":3}
 *
 * Every request gets one reply carrying the same `req`. Events are written to standard error as
 * `EVENT <name> <detail>` lines, so a test can wait on them.
 */
// standard includes
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

// system includes
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "src/webrtc/session.h"

extern char **environ;

using namespace std::literals;

namespace {
  std::mutex out_mutex;  ///< Serializes writes so replies and events never interleave mid-line.

  /**
   * @brief Announce something that happened, for a test to wait on.
   *
   * @param name Event name.
   * @param detail Free-form detail.
   */
  void event(const std::string &name, const std::string &detail = {}) {
    std::lock_guard lg {out_mutex};
    std::cerr << "EVENT " << name << ' ' << detail << std::endl;
  }

  /**
   * @brief Write one reply line.
   *
   * @param reply The reply to send.
   */
  void reply(const nlohmann::json &reply) {
    std::lock_guard lg {out_mutex};
    std::cout << reply.dump() << std::endl;
  }

  /**
   * @brief Split an H.264 byte stream into access units and hand each to a sink.
   *
   * ffmpeg is asked to insert access unit delimiters (NAL type 9), which mark where one frame ends
   * and the next begins.
   */
  class access_unit_splitter_t {
  public:
    /**
     * @brief Create a splitter.
     *
     * @param sink Receives each complete frame.
     */
    explicit access_unit_splitter_t(std::shared_ptr<webrtc::media_sink_t> sink):
        sink_ {std::move(sink)} {
    }

    /**
     * @brief Consume more of the byte stream.
     *
     * @param data Bytes read.
     * @param size Number of bytes.
     */
    void feed(const char *data, std::size_t size) {
      buffer_.insert(buffer_.end(), data, data + size);

      // A frame is complete once the next delimiter shows up behind it.
      while (true) {
        const auto second = find_delimiter(1);
        if (second == npos) {
          return;
        }
        emit(0, second);
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(second));
      }
    }

  private:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);  ///< Not found.

    /**
     * @brief Find the nth access unit delimiter in the buffer.
     *
     * @param nth Zero-based index of the delimiter wanted.
     * @return Offset of its start code, or npos.
     */
    std::size_t find_delimiter(int nth) const {
      int seen = 0;
      for (std::size_t i = 0; i + 4 < buffer_.size(); ++i) {
        // 00 00 00 01 09: a four-byte start code followed by an access unit delimiter.
        if (buffer_[i] == 0 && buffer_[i + 1] == 0 && buffer_[i + 2] == 0 && buffer_[i + 3] == 1 && (buffer_[i + 4] & 0x1f) == 9) {
          if (seen++ == nth) {
            return i;
          }
        }
      }
      return npos;
    }

    /**
     * @brief Deliver buffer bytes [from, to) as one frame.
     *
     * @param from Start offset.
     * @param to End offset.
     */
    void emit(std::size_t from, std::size_t to) {
      bool idr = false;
      for (std::size_t i = from; i + 3 < to; ++i) {
        if (buffer_[i] == 0 && buffer_[i + 1] == 0 && buffer_[i + 2] == 1 && (buffer_[i + 3] & 0x1f) == 5) {
          idr = true;
          break;
        }
      }
      sink_->video_frame(std::string_view {buffer_.data() + from, to - from}, idr, index_++);
    }

    std::shared_ptr<webrtc::media_sink_t> sink_;  ///< Receives each frame.
    std::vector<char> buffer_;  ///< Bytes not yet delivered.
    std::int64_t index_ = 0;  ///< Frame counter.
  };

  /**
   * @brief A stream that plays an ffmpeg test pattern in place of a captured desktop.
   */
  class ffmpeg_stream_t: public webrtc::stream_handle_t {
  public:
    /**
     * @brief Start encoding.
     *
     * @param request What the browser asked for.
     * @param sink Where the encoded frames go.
     * @param die_after_s If positive, make the stream end by itself after this many seconds.
     */
    ffmpeg_stream_t(const webrtc::stream_request_t &request, std::shared_ptr<webrtc::media_sink_t> sink, int die_after_s):
        splitter_ {std::move(sink)} {
      int fds[2];
      if (pipe(fds) != 0) {
        return;
      }

      const auto size = std::to_string(request.width) + "x" + std::to_string(request.height);
      const auto source = "testsrc2=size=" + size + ":rate=" + std::to_string(request.fps);
      const auto keyint = std::to_string(request.fps);
      const auto bitrate = std::to_string(request.bitrate_kbps) + "k";

      std::vector<std::string> args {
        "ffmpeg",
        "-loglevel",
        "error",
        "-re",
        "-f",
        "lavfi",
        "-i",
        source,
        "-c:v",
        "libx264",
        "-profile:v",
        "high",
        "-preset",
        "ultrafast",
        "-tune",
        "zerolatency",
        "-bf",
        "0",
        "-g",
        keyint,
        "-b:v",
        bitrate,
        "-bsf:v",
        "h264_metadata=aud=insert",
        "-f",
        "h264",
        "-",
      };
      std::vector<char *> argv;
      for (auto &arg : args) {
        argv.push_back(arg.data());
      }
      argv.push_back(nullptr);

      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
      posix_spawn_file_actions_addclose(&actions, fds[0]);
      posix_spawn_file_actions_addclose(&actions, fds[1]);

      const auto status = posix_spawnp(&pid_, "ffmpeg", &actions, nullptr, argv.data(), environ);
      posix_spawn_file_actions_destroy(&actions);
      close(fds[1]);
      if (status != 0) {
        close(fds[0]);
        pid_ = 0;
        return;
      }

      read_fd_ = fds[0];
      alive_ = true;
      reader_ = std::thread {[this]() {
        char chunk[1 << 16];
        while (alive_) {
          const auto got = read(read_fd_, chunk, sizeof(chunk));
          if (got <= 0) {
            break;
          }
          splitter_.feed(chunk, static_cast<std::size_t>(got));
        }
        alive_ = false;
        event("stream_ended");
      }};

      if (die_after_s > 0) {
        killer_ = std::thread {[this, die_after_s]() {
          for (int i = 0; i < die_after_s * 10 && alive_; ++i) {
            std::this_thread::sleep_for(100ms);
          }
          if (alive_) {
            event("stream_dying", "as requested");
            terminate_ffmpeg();
          }
        }};
      }

      event("stream_started", size + "@" + std::to_string(request.fps));
    }

    ~ffmpeg_stream_t() override {
      stopped_ = true;
      terminate_ffmpeg();
      if (reader_.joinable()) {
        reader_.join();
      }
      if (killer_.joinable()) {
        killer_.join();
      }
      if (read_fd_ >= 0) {
        close(read_fd_);
      }
    }

    /**
     * @brief Whether the stream is still producing frames.
     *
     * @return `false` once ffmpeg has gone, or the stream was stopped.
     */
    bool running() override {
      return alive_ && !stopped_;
    }

    /**
     * @brief Stop the stream.
     */
    void stop() override {
      if (!stopped_.exchange(true)) {
        event("stream_stopped");
      }
      terminate_ffmpeg();
    }

    /**
     * @brief Record that a keyframe was requested.
     *
     * The test pattern already produces a keyframe every second, which is soon enough for a test.
     */
    void request_idr() override {
      event("keyframe_requested");
    }

  private:
    /**
     * @brief Ask ffmpeg to exit, once.
     */
    void terminate_ffmpeg() {
      if (pid_ > 0 && !terminated_.exchange(true)) {
        kill(pid_, SIGTERM);
        waitpid(pid_, nullptr, 0);
      }
    }

    access_unit_splitter_t splitter_;  ///< Cuts ffmpeg's output into frames.
    pid_t pid_ = 0;  ///< The ffmpeg process.
    int read_fd_ = -1;  ///< Read end of ffmpeg's output.
    std::atomic_bool alive_ {false};  ///< Whether ffmpeg is still producing output.
    std::atomic_bool stopped_ {false};  ///< Whether stop() was called.
    std::atomic_bool terminated_ {false};  ///< Whether ffmpeg was told to exit.
    std::thread reader_;  ///< Reads ffmpeg's output.
    std::thread killer_;  ///< Ends the stream early when a test asks for that.
  };

  /**
   * @brief Carries per-request test knobs to the stream starter.
   */
  std::atomic_int next_die_after_s {0};
}  // namespace

namespace crypto {
  std::string rand_alphabet(std::size_t bytes, const std::string_view &alphabet) {
    static std::mt19937 generator {std::random_device {}()};
    std::string result;
    for (std::size_t i = 0; i < bytes; ++i) {
      result += alphabet[generator() % alphabet.size()];
    }
    return result;
  }
}  // namespace crypto

namespace webrtc {
  // The real starter drives a live Sunshine. This host always installs its own, so this is never reached.
  std::unique_ptr<stream_handle_t> start_host_stream(const stream_request_t &, std::shared_ptr<media_sink_t>, std::string &error) {
    error = "the real host stream is not part of the end-to-end host";
    return nullptr;
  }
}  // namespace webrtc

int main() {
  // Writing to a closed pipe must not kill the host.
  std::signal(SIGPIPE, SIG_IGN);

  webrtc::set_stream_starter([](const webrtc::stream_request_t &request, std::shared_ptr<webrtc::media_sink_t> sink, std::string &error) -> std::unique_ptr<webrtc::stream_handle_t> {
    auto stream = std::make_unique<ffmpeg_stream_t>(request, std::move(sink), next_die_after_s.exchange(0));
    if (!stream->running()) {
      error = "Could not start ffmpeg";
      return nullptr;
    }
    return stream;
  });

  event("ready");

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      continue;
    }

    nlohmann::json request;
    try {
      request = nlohmann::json::parse(line);
    } catch (const std::exception &e) {
      reply({{"ok", false}, {"error", std::string {"bad request: "} + e.what()}});
      continue;
    }

    const auto req = request.value("req", 0);
    const auto cmd = request.value("cmd", "");

    if (cmd == "offer") {
      webrtc::stream_request_t stream_request;
      stream_request.width = request.value("width", stream_request.width);
      stream_request.height = request.value("height", stream_request.height);
      stream_request.fps = request.value("fps", stream_request.fps);
      stream_request.bitrate_kbps = request.value("bitrate", stream_request.bitrate_kbps);
      next_die_after_s = request.value("die_after_s", 0);

      std::string error;
      const auto answer = webrtc::accept_offer(request.value("sdp", ""), request.value("stun", ""), stream_request, error);
      if (answer) {
        reply({{"req", req}, {"ok", true}, {"id", answer->id}, {"sdp", answer->sdp}});
      } else {
        reply({{"req", req}, {"ok", false}, {"error", error}});
      }
    } else if (cmd == "close") {
      reply({{"req", req}, {"ok", webrtc::close_session(request.value("id", ""))}});
    } else if (cmd == "count") {
      reply({{"req", req}, {"ok", true}, {"count", webrtc::session_count()}});
    } else {
      reply({{"req", req}, {"ok", false}, {"error", "unknown command"}});
    }
  }

  webrtc::close_all();
  return 0;
}
