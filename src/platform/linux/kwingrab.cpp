/**
 * @file src/platform/linux/kwingrab.cpp
 * @brief KWin direct ScreenCast capture via zkde_screencast_unstable_v1 Wayland protocol.
 *
 * Bypasses xdg-desktop-portal entirely. Sunshine connects directly to KWin's
 * Wayland protocol to obtain a PipeWire node_id, then streams frames via PipeWire.
 *
 * Chain: KWin -> Wayland kde_screencast -> PipeWire -> Sunshine
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <pwd.h>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>

// lib includes
#include <lizardbyte/common/env.h>
#include <pipewire/pipewire.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>

// generated protocol header
#include <kde-output-device-v2.h>
#include <kde-output-management-v2.h>
#include <kde-output-order-v1.h>
#include <zkde-screencast-unstable-v1.h>

// local includes
#include "cuda.h"
#include "graphics.h"
#include "pipewire.cpp"
#include "src/platform/common.h"
#include "src/video.h"

using namespace std::literals;

namespace kwin {
  /**
   * KWin Wayland ScreenCast permissions
   *
   * To have access to zkde_screencast_unstable_v1 (and the privileged output-management
   * protocols used for layout enumeration/apply) KWin checks for a .desktop file with
   * X-KDE-Wayland-Interfaces listing the needed interfaces and the current executable name
   * in the Exec= parameter.
   */
  class screencast_permission_helper_t {
  public:
    /**
     * @brief Check whether permission system deactivated.
     *
     * @return True when KWin reports that the permission system is disabled.
     */
    static bool is_permission_system_deactivated() {
      return lizardbyte::common::get_env("KWIN_WAYLAND_NO_PERMISSION_CHECKS") == "1";
    }

    /**
     * @brief Configure the KWin screencast session.
     */
    static void setup() {
      if (initialized) {
        return;
      }
      auto filenameprefix = std::format("{}.kwin", PROJECT_FQDN);
      auto executablepath = get_executable_full_path();

      // System: Check system XDG applications for permission (usually installed with Sunshine)
      if (check_kwin_system_permissions(filenameprefix, executablepath)) {
        create_file = false;
        initialized = true;
        return;
      }

      // If we do not have a system permission, check if we need a temporary permission via user's application directory
      if (is_permission_system_deactivated()) {
        BOOST_LOG(info) << "[kwingrab] No permission desktop file necessary. KWin permission system deactivated.";
        create_file = false;
        initialized = true;
        return;
      }

      // User: Check and (if necessary) update user's XDG applications for permission
      auto user_applications = get_xdg_user_applications_path();
      if (user_applications.empty()) {
        BOOST_LOG(error) << "[kwingrab] Failed to determine user application directory. Cannot continue with permission setup.";
        return;
      }
      // Create non-existing application directory so we can write into it
      if (!std::filesystem::exists(user_applications) && !std::filesystem::create_directories(user_applications)) {
        // In case of failure log and return
        BOOST_LOG(error) << "[kwingrab] Failed to create application directory. Cannot continue with permission setup.";
        create_file = false;
        initialized = true;
        return;
      }
      auto user_filepathprefix = (std::filesystem::path(user_applications) / filenameprefix).string();
      for (const auto &path : std::filesystem::directory_iterator(user_applications)) {
        // List existing files for prefix and check if they contain this executable or remove them
        const auto entry = path.path().string();
        if (entry.starts_with(user_filepathprefix)) {
          auto entry_executablepath = get_executable_from_desktop_file(entry);
          if (!entry_executablepath.empty() && entry_executablepath == executablepath) {
            // This entry is exactly the one we need
            BOOST_LOG(debug) << "[kwingrab] Ignoring current temporary KWin wayland permission file: "sv << entry;
            create_file = false;
            continue;
          }
          if (!entry_executablepath.empty() && std::filesystem::exists(entry_executablepath)) {
            // This entry is for another sunshine executable that still exists
            BOOST_LOG(debug) << "[kwingrab] Ignoring other valid temporary KWin wayland permission file: "sv << entry;
            continue;
          }
          if (std::filesystem::remove(path)) {
            BOOST_LOG(info) << "[kwingrab] Removed stale temporary KWin wayland permission file: "sv << entry << " executable: "sv << entry_executablepath;
          } else {
            BOOST_LOG(warning) << "[kwingrab] Failed to remove stale temporary KWin wayland permission file: "sv << entry << " executable: "sv << entry_executablepath;
          }
        }
      }
      if (create_file) {
        // Generate a unique file identifier based on current unixtime
        auto user_filepathidentifier = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
        auto user_filepath = std::format("{}{}.desktop", user_filepathprefix, user_filepathidentifier);
        // Write new file if necessary
        std::ofstream filestream(user_filepath);
        if (filestream.is_open()) {
          filestream << "[Desktop Entry]" << std::endl
                     << "Exec=" << executablepath << std::endl
                     << "X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1;kde_output_device_v2;kde_output_management_v2" << std::endl
                     << "Type=Application" << std::endl
                     << "Name="sv << PROJECT_FQDN << "-kwin-wayland-permission" << std::endl
                     << "Comment=Sunshine KWin screencast permission" << std::endl
                     << "NoDisplay=true" << std::endl;
          filestream.close();
          // Give KWin time to catch up to the new desktop file
          BOOST_LOG(info) << "[kwingrab] Created temporary KWin wayland permission file: "sv << user_filepath << " - Waiting 3 seconds for KDE to pick up new file.";
          std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        } else {
          BOOST_LOG(warning) << "[kwingrab] Failed to open temporary KWin wayland permission file: "sv << user_filepath;
        }
      }

      initialized = true;
    }

    /**
     * @brief Check whether newly initialized.
     *
     * @return True when KWin was initialized during this check.
     */
    static bool is_newly_initialized() {
      return create_file;
    }

  private:
    static inline bool initialized = false;
    static inline bool create_file = true;

    static std::filesystem::path get_home_dir() {
      // Check HOME environment variable
      if (std::string homedir = lizardbyte::common::get_env("HOME"); !homedir.empty()) {
        return homedir;
      }
      // Fall back to home directory from NSS passwd
      // Note: This should be thread-safe as we're always accessing the same entry for Sunshine
      return getpwuid(geteuid())->pw_dir;
    }

    static std::filesystem::path get_xdg_user_applications_path() {
      // Follow the XDG base directory specification for user data home:
      // https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html
      std::filesystem::path xdg_data_home;
      if (std::string dir = lizardbyte::common::get_env("XDG_DATA_HOME"); !dir.empty()) {
        xdg_data_home = std::filesystem::path(dir);
      } else {
        const auto homedir = get_home_dir();
        if (homedir.empty()) {
          return "";
        }
        xdg_data_home = std::filesystem::path(homedir) / ".local"sv / "share"sv;
      }
      return xdg_data_home / "applications";
    }

    static std::string get_executable_full_path() {
      // Adapted from https://linuxvox.com/blog/how-do-i-find-the-location-of-the-executable-in-c/
      constexpr auto path_len = PATH_MAX;  // PATH_MAX is defined in limits.h (e.g., 4096 on Linux)
      auto path_exe = std::make_unique<char[]>(path_len);
      // Read the symlink /proc/self/exe into path_exe
      const ssize_t len = readlink("/proc/self/exe", &path_exe[0], path_len - 1);
      if (len == -1) {
        return "";
      }
      // Return path_exe as a proper std::string with len returned by readlink
      return std::string(path_exe.get(), len);
    }

    static std::string get_executable_from_desktop_file(const std::string &path) {
      if (std::ifstream file(path); file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
          if (line.starts_with("Exec=") && line.length() > 5) {
            return line.substr(5);
          }
        }
      }
      return "";
    }

    static bool check_kwin_system_permissions(const std::string_view &filenameprefix, const std::string_view &executablepath) {
      // Find data dirs to check from XDG_DATA_DIRS
      std::vector<std::string> xdg_data_dirs;
      if (const std::string e = lizardbyte::common::get_env("XDG_DATA_DIRS"); !e.empty()) {
        std::stringstream ss(e);
        std::string item;

        while (getline(ss, item, ':')) {  // : is likely valid for all OSes supported, if a constant is available it should be used instead
          xdg_data_dirs.push_back(item);
        }
      }
      // Use defaults from https://specifications.freedesktop.org/basedir/latest/ if ENV var was empty
      if (xdg_data_dirs.empty()) {
        xdg_data_dirs.emplace_back("/usr/local/share/");
        xdg_data_dirs.emplace_back("/usr/share/");
      }
      // Check for ${filenameprefix}.desktop in each directory
      for (auto const &dir : xdg_data_dirs) {
        std::string filename = std::format("{0}{1}applications{1}{2}.desktop", dir, boost::filesystem::path::preferred_separator, filenameprefix);
        if (std::filesystem::exists(filename)) {
          auto file_executablepath = get_executable_from_desktop_file(filename);
          if (file_executablepath == executablepath) {
            BOOST_LOG(info) << "[kwingrab] Found matching system KWin desktop permission file: "sv << filename;
            return true;
          }
        }
      }
      return false;
    }
  };

  // Output parameters
  /**
   * @brief KWin screencast output name and geometry.
   */
  struct output_parameter_t {
    std::string name;  ///< KWin output name.
    int width = 0;  ///< Output width in pixels.
    int height = 0;  ///< Output height in pixels.
    int pos_x = 0;  ///< Output X position in the compositor layout.
    int pos_y = 0;  ///< Output Y position in the compositor layout.
    // order is needed to get a sorted output list and should be updated before sorting to have current values
    /**
     * @brief Order.
     */
    size_t order = SIZE_MAX;  // Use high number to keep monitors with uninitialized order value to the back
  };

  /**
   * Wayland KDE ScreenCast session
   *
   * Owns its own wl_display connection. Binds zkde_screencast_unstable_v1
   * and wl_output from the registry, then calls stream_output() to start
   * a ScreenCast. Waits for the created(node_id) event from KWin.
   */
  class screencast_t {
  public:
    screencast_t &operator=(screencast_t &&) = delete;  // Do not allow to copying

    ~screencast_t() {
      // Release KDE screencast wayland extensions and reset pointers
      if (kde_screencast_stream_v1_) {
        zkde_screencast_stream_unstable_v1_close(kde_screencast_stream_v1_);
        kde_screencast_stream_v1_ = nullptr;
      }
      if (kde_screencast_v1_) {
        zkde_screencast_unstable_v1_destroy(kde_screencast_v1_);
        kde_screencast_v1_ = nullptr;
      }
      if (kde_output_order) {
        kde_output_order_v1_destroy(kde_output_order);
        kde_output_order = nullptr;
      }

      // Clear output order list
      output_order.clear();
      // Clear current output parameters
      out_params.reset();
      out_params = nullptr;

      // wl_output is owned by the registry, released on disconnect
      // also cleanup associated output parameters and clear output list when done
      for (auto &[output, params] : outputs) {
        wl_output_destroy(output);
        params.reset();
      }
      outputs.clear();

      // Release wayland registry, display and reset pointers
      if (wl_registry) {
        wl_registry_destroy(wl_registry);
        wl_registry = nullptr;
      }
      if (wl_display) {
        wl_display_disconnect(wl_display);
        wl_display = nullptr;
      }
    }

    /**
     * @brief Connect to KWin wayland, enumerate outputs.
     * @param setup_permissions - Try to setup KWin permissions (default: true)
     * @return 0 on success, -1 on failure. On success, node_id and
     *         output width/height/x/y are populated.
     */
    int init(const bool setup_permissions = true) {
      if (setup_permissions) {
        // Try to set up permissions for zkde_screencast_unstable_v1
        screencast_permission_helper_t::setup();
      }

      std::string wl_name;
      if (!lizardbyte::common::get_env("WAYLAND_DISPLAY", wl_name)) {
        BOOST_LOG(error) << "[kwingrab] WAYLAND_DISPLAY not set"sv;
        return -1;
      }

      wl_display = wl_display_connect(wl_name.c_str());
      if (!wl_display) {
        BOOST_LOG(error) << "[kwingrab] cannot connect to Wayland display: "sv << wl_name;
        return -1;
      }

      wl_registry = wl_display_get_registry(wl_display);
      wl_registry_add_listener(wl_registry, &registry_listener, this);
      wl_display_roundtrip(wl_display);

      // We need a second roundtrip after binding outputs to get wl_output events
      wl_display_roundtrip(wl_display);

      return 0;
    }

    /**
     * @brief Check if kwin screencasting is currently available
     * @return true if screencast can be started, false otherwise
     */
    bool is_kwin_screencasting_available() const {
      return kde_screencast_v1_ != nullptr;
    }

    /**
     * @brief Generate a sorted list of known output names.
     * @return List of strings with output names to pass to start()
     */
    std::vector<std::string> get_output_names() {
      std::vector<std::shared_ptr<output_parameter_t>> sorted_outputs;
      for (const auto &output_parameter : outputs | std::views::values) {
        output_parameter->order = get_order_for_output_name(output_parameter->name);
        sorted_outputs.emplace_back(output_parameter);
      }
      std::ranges::sort(sorted_outputs, [](const auto &a, const auto &b) {
        return a->order < b->order || a->pos_x < b->pos_x || a->pos_y < b->pos_y;
      });
      std::vector<std::string> output_names;
      for (const auto &output_parameter : sorted_outputs) {
        BOOST_LOG(info) << "[kwingrab] Found output: "sv << output_parameter->name << " order: "sv << output_parameter->order << " position: "sv << output_parameter->pos_x << "x"sv << output_parameter->pos_y << " resolution: "sv << output_parameter->width << "x"sv << output_parameter->height;
        output_names.emplace_back(output_parameter->name);
      }
      return output_names;
    }

    /**
     * @brief Check if KWin is available for potential screencasting
     * @return True if KWin is detected
     */
    bool kwin_available() const {
      // Detect KWin using kde_output_order_v1 extension
      if (kde_output_order) {
        return true;
      }
      return false;
    }

    /**
     * @brief Request a screencast stream.
     * @param output_name Which wl_output to capture.
     * @return 0 on success, -1 on failure. On success, node_id and
     *         output width/height/x/y are populated.
     */
    int start(const std::string_view &output_name) {
      // Try find correct output by name
      if (outputs.empty()) {
        BOOST_LOG(error) << "[kwingrab] no wl_output found"sv;
        return -1;
      }
      struct wl_output *output = nullptr;
      if (!output_name.empty()) {
        for (auto const &[output_, params_] : outputs) {
          if (params_->name == output_name) {
            output = output_;
            out_params = params_;
          }
        }
      }
      // Fall back to first element from the map in case of error
      if (!output || !out_params) {
        const auto output_ = outputs.begin();
        output = output_->first;
        out_params = output_->second;
      }

      // Request a stream for the chosen output with embedded cursor
      if (kde_screencast_v1_) {
        kde_screencast_stream_v1_ = zkde_screencast_unstable_v1_stream_output(kde_screencast_v1_, output, ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
        zkde_screencast_stream_unstable_v1_add_listener(kde_screencast_stream_v1_, &stream_listener, this);
      } else {
        // No screencast protocol found. Output an error based on newly initialized permission file.
        if (screencast_permission_helper_t::is_newly_initialized()) {
          BOOST_LOG(error) << "[kwingrab] zkde_screencast_unstable_v1 not found in registry. "sv
                              "A new permission desktop file was automatically created but might now have been recognized yet. "sv
                              "Try restarting sunshine or set KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 to fully disable permission checks."sv;
        } else {
          BOOST_LOG(error) << "[kwingrab] zkde_screencast_unstable_v1 not found in registry. Check permission desktop file "sv
                              "for sunshine binary or set KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 to fully disable permission checks."sv;
        }
        return -1;
      }

      if (wait_for_stream() < 0) {
        return -1;
      }

      if (stream_failed) {
        BOOST_LOG(error) << "[kwingrab] stream_output failed: "sv << stream_error_msg;
        return -1;
      }
      // Check for valid node_id and/or object serial values here, stream_ready is just an internal flag
      if (out_node_id == PW_ID_ANY && (out_objectserial & SPA_ID_INVALID) == SPA_ID_INVALID) {
        BOOST_LOG(error) << "[kwingrab] timeout waiting for created event"sv;
        return -1;
      }

      if ((out_objectserial & SPA_ID_INVALID) == SPA_ID_INVALID) {
        BOOST_LOG(info) << "[kwingrab] Pipewire stream created: node="sv << out_node_id;
      } else {
        BOOST_LOG(info) << "[kwingrab] Pipewire stream created: objectserial="sv << out_objectserial << " (node="sv << out_node_id << ")"sv;
      }

      if (out_params->width == 0 || out_params->height == 0) {
        BOOST_LOG(error) << "[kwingrab] could not determine output dimensions"sv;
        return -1;
      }

      BOOST_LOG(info) << "[kwingrab] Screencasting output"sv
                      << " name "sv << out_params->name
                      << " position "sv << out_params->pos_x << "x"sv << out_params->pos_y
                      << " resolution "sv << out_params->width << "x"sv << out_params->height;
      return 0;
    }

    uint32_t out_node_id = PW_ID_ANY;  ///< Out node ID.
    uint64_t out_objectserial = SPA_ID_INVALID;  ///< Out objectserial.
    std::shared_ptr<output_parameter_t> out_params = nullptr;  ///< Out params.

  private:
    // Wayland objects
    struct wl_display *wl_display = nullptr;
    struct wl_registry *wl_registry = nullptr;
    struct kde_output_order_v1 *kde_output_order = nullptr;
    struct zkde_screencast_unstable_v1 *kde_screencast_v1_ = nullptr;
    struct zkde_screencast_stream_unstable_v1 *kde_screencast_stream_v1_ = nullptr;
    std::map<struct wl_output *, std::shared_ptr<output_parameter_t>> outputs;
    std::vector<std::string> output_order;
    bool stream_failed = false;
    bool stream_ready = false;
    std::string stream_error_msg;

    // Misc functions
    int wait_for_stream() {
      // Dispatch until we get created/failed, with a 5s timeout
      auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!stream_ready && !stream_failed && std::chrono::steady_clock::now() < deadline) {
        wl_display_flush(wl_display);

        struct pollfd pfd = {};
        pfd.fd = wl_display_get_fd(wl_display);
        pfd.events = POLLIN;

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()
        );
        if (remaining.count() <= 0) {
          break;
        }

        if (poll(&pfd, 1, remaining.count()) > 0 && (pfd.revents & POLLIN) && wl_display_dispatch(wl_display) < 0) {
          BOOST_LOG(error) << "[kwingrab] wl_display_dispatch failed"sv;
          return -1;
        }
      }
      return 0;
    }

    size_t get_order_for_output_name(const std::string_view &name) const {
      for (size_t i = 0; i < output_order.size(); i++) {
        if (output_order[i] == name) {
          return i;
        }
      }
      // If nothing matches return list size (to ensure highest order)
      return output_order.size();
    }

    // Registry listener
    static void on_registry_global(void *data, struct wl_registry *reg, const uint32_t name, const char *interface, const uint32_t version) {
      auto *self = static_cast<screencast_t *>(data);
      if (!std::strcmp(interface, kde_output_order_v1_interface.name)) {
        // Bind version 1
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(1));
        self->kde_output_order = static_cast<struct kde_output_order_v1 *>(
          wl_registry_bind(reg, name, &kde_output_order_v1_interface, bind_ver)
        );
        kde_output_order_v1_add_listener(self->kde_output_order, &output_order_listener, self);
        BOOST_LOG(debug) << "[kwingrab] bound kde_output_order_v1 version "sv << bind_ver;
      } else if (!std::strcmp(interface, zkde_screencast_unstable_v1_interface.name)) {
        // Bind version 1 to 6 — We use stream_output from v1 for node_id (deprecated but good as a fall-back)
        //                       but also try to get the newer (re-use safe) pipewire objectserial from v6
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(6));
        self->kde_screencast_v1_ = static_cast<struct zkde_screencast_unstable_v1 *>(
          wl_registry_bind(reg, name, &zkde_screencast_unstable_v1_interface, bind_ver)
        );
        BOOST_LOG(debug) << "[kwingrab] bound zkde_screencast_unstable_v1 version "sv << bind_ver;
      } else if (!std::strcmp(interface, wl_output_interface.name)) {
        // Bind version 4 - we need wl_output name for matching
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(4));
        auto *output = static_cast<struct wl_output *>(
          wl_registry_bind(reg, name, &wl_output_interface, bind_ver)
        );

        const auto [_, inserted] = self->outputs.try_emplace(output, std::make_shared<output_parameter_t>());
        if (inserted) {
          wl_output_add_listener(output, &output_listener, self);
          BOOST_LOG(debug) << "[kwingrab] bound wl_output version "sv << bind_ver << " instance: "sv << output;
        } else {
          // If we for some odd reason cannot add the output to the map clean it up and log a warning
          BOOST_LOG(warning) << "[kwingrab] Ignoring output "sv << output << " because map emplace failed."sv;
          wl_output_destroy(output);
        }
      }
    }

    static void on_registry_global_remove(void *data [[maybe_unused]], struct wl_registry *reg [[maybe_unused]], uint32_t name [[maybe_unused]]) {
      // We don't handle output hot-unplug during init
    }

    static constexpr struct wl_registry_listener registry_listener = {
      .global = on_registry_global,
      .global_remove = on_registry_global_remove,
    };

    // wl_output listener (for mode/dimensions/name)
    static void on_output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t pw [[maybe_unused]], int32_t ph [[maybe_unused]], int32_t subpixel [[maybe_unused]], const char *make [[maybe_unused]], const char *model [[maybe_unused]], int32_t transform [[maybe_unused]]) {
      const auto *self = static_cast<screencast_t *>(data);
      const auto output_parameter = self->outputs.at(output);
      output_parameter->pos_x = x;
      output_parameter->pos_y = y;
    }

    static void on_output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh [[maybe_unused]]) {
      if (!(flags & WL_OUTPUT_MODE_CURRENT)) {
        return;
      }
      const auto *self = static_cast<screencast_t *>(data);
      const auto output_parameter = self->outputs.at(output);
      output_parameter->width = width;
      output_parameter->height = height;
    }

    static void on_output_done(void *data [[maybe_unused]], struct wl_output *output [[maybe_unused]]) {
      // Currently unused
    }

    static void on_output_scale(void *data [[maybe_unused]], struct wl_output *output [[maybe_unused]], int32_t factor [[maybe_unused]]) {
      // Currently unused
    }

    static void on_output_name(void *data, struct wl_output *output, const char *name) {
      const auto *self = static_cast<screencast_t *>(data);
      self->outputs.at(output)->name = name;
    }

    static void on_output_description(void *data [[maybe_unused]], struct wl_output *output [[maybe_unused]], const char *description [[maybe_unused]]) {
      // Currently unused
    }

    static constexpr struct wl_output_listener output_listener = {
      .geometry = on_output_geometry,
      .mode = on_output_mode,
      .done = on_output_done,
      .scale = on_output_scale,
      .name = on_output_name,
      .description = on_output_description,
    };

    // Output order listener
    static void on_output_order_output(void *data, struct kde_output_order_v1 *kde_output_order_v1 [[maybe_unused]], const char *output_name) {
      auto *self = static_cast<screencast_t *>(data);
      self->output_order.emplace_back(output_name);
    }

    static void on_output_order_done(void *data [[maybe_unused]], struct kde_output_order_v1 *kde_output_order_v1 [[maybe_unused]]) {
      // Currently unused
    }

    static constexpr kde_output_order_v1_listener output_order_listener = {
      .output = on_output_order_output,
      .done = on_output_order_done,
    };

    // ScreenCast v1 stream listener
    static void on_stream_closed(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]]) {
      auto *self = static_cast<screencast_t *>(data);
      BOOST_LOG(warning) << "[kwingrab] stream closed by server"sv;
      self->stream_failed = false;
      self->stream_ready = false;
      self->stream_error_msg = "stream closed by server";
    }

    static void on_stream_created(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]], const uint32_t node) {
      auto *self = static_cast<screencast_t *>(data);
      self->out_node_id = node;
      self->stream_failed = false;
      self->stream_ready = true;
      BOOST_LOG(debug) << "[kwingrab] created event, node_id="sv << node;
    }

    static void on_stream_failed(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]], const char *err_msg) {
      auto *self = static_cast<screencast_t *>(data);
      self->stream_failed = true;
      self->stream_ready = false;
      self->stream_error_msg = err_msg ? err_msg : "unknown error";
      BOOST_LOG(error) << "[kwingrab] failed event: "sv << self->stream_error_msg;
    }

    static void on_stream_serial(void *data, struct zkde_screencast_stream_unstable_v1 *stream [[maybe_unused]], uint32_t object_serial_hi, uint32_t object_serial_low) {
      auto *self = static_cast<screencast_t *>(data);
      self->out_objectserial = static_cast<uint64_t>(object_serial_hi) << 32 | object_serial_low;
      // serial event always preceded the created event with the node id, so we only set stream_ready in created for v1
      BOOST_LOG(debug) << "[kwingrab] serial event, objectserial="sv << self->out_objectserial;
    }

    static constexpr struct zkde_screencast_stream_unstable_v1_listener stream_listener = {
      .closed = on_stream_closed,
      .created = on_stream_created,
      .failed = on_stream_failed,
      .serial = on_stream_serial,
    };
  };

  /**
   * Display backend
   *
   * Orchestrates screencast_t and implements pipewire_display_t
   */
  class kwin_t: public pipewire::pipewire_display_t {
  public:
    int configure_stream(const std::string &display_name, int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_objectserial) override {
      screencast = std::make_unique<screencast_t>();
      if (screencast->init(true) < 0) {
        return -1;
      }
#if !defined(__FreeBSD__)
      // Check if KWin screencasting extension is accessible after first init attempt
      if (!screencast->is_kwin_screencasting_available()) {
        // KWin screencasting extension was not found. Drop ALL elevated privileges in case KWin is missing CAP_SYS_NICE
        BOOST_LOG(warning) << "[kwingrab] KWin screencasting unavailable after init. Trying again after dropping ALL elevated privileges."sv;
        platf::drop_elevated_privileges(true);
        // Retry screencast session init after privilege drop
        screencast.reset();  // Cleanup current screencast instance
        screencast = std::make_unique<screencast_t>();  // Create new screencast instance
        if (screencast->init(true) < 0) {
          return -1;
        }
      }
#endif
      if (screencast->start(display_name) < 0) {
        return -1;
      }
      if (screencast->out_params) {
        // Return values for pipewire init
        out_pipewire_fd = -1;  // KWin screencast capture runs on the local pipewire core
        out_pipewire_node = screencast->out_node_id;
        out_pipewire_objectserial = screencast->out_objectserial;
        // Set/update basic stream parameters on display_t
        this->offset_x = screencast->out_params->pos_x;
        this->offset_y = screencast->out_params->pos_y;
        this->width = screencast->out_params->width;
        this->height = screencast->out_params->height;
        this->logical_width = 0;  // Explicitly mark for pipewire_display_t to try to figure this out.
        this->logical_height = 0;  // Explicitly Mark for pipewire_display_t to try to figure this out.
        return 0;
      }
      return -1;
    }

    std::unique_ptr<screencast_t> screencast;  ///< Screencast.
  };

  namespace {
    /**
     * @brief Cached, decoded state of a single kde_output_device_mode_v2.
     */
    struct kwin_mode_state_t {
      int width = 0;  ///< Mode width in pixels.
      int height = 0;  ///< Mode height in pixels.
      int refresh_mhz = 0;  ///< Mode refresh rate in milli-Hz.
    };

    /**
     * @brief Convert a KDE output transform enum value to a clockwise degree value.
     * @param transform KDE transform enum (0=normal, 1=90, 2=180, 3=270, 4-7=flipped variants).
     * @return 0, 90, 180, or 270 - flipped variants collapse to their base rotation.
     */
    int kde_transform_to_degrees(int32_t transform) {
      switch (transform & 0x3) {
        case 1:
          return 90;
        case 2:
          return 180;
        case 3:
          return 270;
        default:
          return 0;
      }
    }

    /**
     * @brief Convert a clockwise degree value to a KDE output transform enum value.
     * @param degrees 0, 90, 180, or 270 (other values are treated as 0). Flips are never produced.
     * @return KDE transform enum value.
     */
    int32_t degrees_to_kde_transform(int degrees) {
      switch (degrees) {
        case 90:
          return 1;
        case 180:
          return 2;
        case 270:
          return 3;
        default:
          return 0;
      }
    }

    /**
     * @brief Cached, decoded state of a single kde_output_device_v2.
     */
    struct kwin_device_state_t {
      std::string name;  ///< Output connector name, e.g. "eDP-1".
      int x = 0;  ///< X position within the compositor's global space.
      int y = 0;  ///< Y position within the compositor's global space.
      bool enabled = false;  ///< Whether the output currently has an active mode.
      bool primary = false;  ///< Whether this is the primary output (derived from the lowest `priority`).
      uint32_t priority = std::numeric_limits<uint32_t>::max();  ///< Output priority; 1 is highest/primary (KDE convention).
      int32_t transform = 0;  ///< KDE transform enum value (0=normal, 1=90, 2=180, 3=270, 4-7=flipped variants).
      int width = 0;  ///< Current mode width in pixels.
      int height = 0;  ///< Current mode height in pixels.
      int refresh_mhz = 0;  ///< Current mode refresh rate in milli-Hz.
    };
  }  // namespace

  /**
   * @brief One-shot KWin output-layout query/apply session.
   *
   * Uses the privileged, desktop-environment-implementation-detail
   * kde_output_device_v2 / kde_output_management_v2 Wayland protocols to
   * read and change monitor position/enabled/primary state. Primary is
   * derived from the lowest `priority` value (KDE's own convention, also
   * used by kscreen-doctor) since this KWin version does not advertise a
   * separate primary-output protocol. Access requires the same permission
   * desktop file mechanism as zkde_screencast_unstable_v1 (see
   * screencast_permission_helper_t).
   *
   * Connects, does its work, and disconnects - no persistent connection is
   * kept between calls, matching the one-shot query style used by the X11
   * backend's x11_enum_outputs()/x11_apply_outputs().
   */
  class output_management_t {
  public:
    output_management_t &operator=(output_management_t &&) = delete;

    ~output_management_t() {
      if (kde_output_configuration) {
        kde_output_configuration_v2_destroy(kde_output_configuration);
        kde_output_configuration = nullptr;
      }
      if (kde_output_management) {
        kde_output_management_v2_destroy(kde_output_management);
        kde_output_management = nullptr;
      }
      for (auto &device : devices | std::views::keys) {
        kde_output_device_v2_release(device);
      }
      devices.clear();
      modes.clear();
      if (kde_output_device_registry) {
        kde_output_device_registry_v2_destroy(kde_output_device_registry);
        kde_output_device_registry = nullptr;
      }
      if (wl_registry) {
        wl_registry_destroy(wl_registry);
        wl_registry = nullptr;
      }
      if (wl_display) {
        wl_display_disconnect(wl_display);
        wl_display = nullptr;
      }
    }

    /**
     * @brief Connect to KWin's Wayland socket and enumerate output devices.
     * @return 0 on success, -1 on failure.
     */
    int init() {
      screencast_permission_helper_t::setup();

      std::string wl_name;
      if (!lizardbyte::common::get_env("WAYLAND_DISPLAY", wl_name)) {
        BOOST_LOG(error) << "[kwingrab] WAYLAND_DISPLAY not set"sv;
        return -1;
      }

      wl_display = wl_display_connect(wl_name.c_str());
      if (!wl_display) {
        BOOST_LOG(error) << "[kwingrab] cannot connect to Wayland display: "sv << wl_name;
        return -1;
      }

      wl_registry = wl_display_get_registry(wl_display);
      wl_registry_add_listener(wl_registry, &registry_listener, this);
      wl_display_roundtrip(wl_display);  // bind globals (including direct-global kde_output_device_v2 objects, if any)
      wl_display_roundtrip(wl_display);  // receive per-device property events, and/or the registry wrapper's "output" events
      if (kde_output_device_registry) {
        // The registry wrapper's kde_output_device_v2 objects were only just created while
        // dispatching the previous roundtrip's "output" events - their own property events
        // haven't been sent yet, so one more roundtrip is needed to receive them.
        wl_display_roundtrip(wl_display);
      }

      if (!kde_output_management || devices.empty()) {
        BOOST_LOG(warning) << "[kwingrab] kde_output_management_v2/kde_output_device_v2 unavailable "sv
                               "(missing permission desktop file entry or unsupported KWin version)"sv;
        return -1;
      }

      // KDE has no separate "is this the primary output" event - the output with the lowest
      // `priority` (1 = highest) is the primary one, matching kscreen/kscreen-doctor's convention.
      uint32_t min_priority = std::numeric_limits<uint32_t>::max();
      for (const auto &state : devices | std::views::values) {
        min_priority = std::min(min_priority, state.priority);
      }
      for (auto &state : devices | std::views::values) {
        state.primary = state.priority == min_priority;
      }

      return 0;
    }

    /**
     * @brief Convert enumerated devices to the platform-neutral output list.
     * @return Live output state for every bound device.
     */
    std::vector<platf::display_output_t> to_display_outputs() const {
      std::vector<platf::display_output_t> result;
      result.reserve(devices.size());
      for (const auto &state : devices | std::views::values) {
        platf::display_output_t output;
        output.id = state.name;
        output.friendly_name = state.name;
        output.connected = true;  // KWin only advertises devices that are physically present
        output.enabled = state.enabled;
        output.primary = state.primary;
        output.x = state.x;
        output.y = state.y;
        output.width = state.width;
        output.height = state.height;
        output.refresh_rate = state.refresh_mhz / 1000.0;
        output.rotation = kde_transform_to_degrees(state.transform);
        result.emplace_back(std::move(output));
      }
      return result;
    }

    /**
     * @brief Apply a desired arrangement to the bound output devices.
     *
     * Enabled/position/primary/rotation are changed - resolution and refresh
     * rate are left untouched, matching x11_apply_outputs()'s behavior.
     *
     * @param desired Desired output states, matched to bound devices by name (== id).
     * @return True if the compositor applied the changes successfully.
     */
    bool apply(const std::vector<platf::display_output_t> &desired) {
      // A saved layout's positions reflect wherever those outputs happened to sit when it was
      // captured - e.g. a dummy that used to sit alongside other monitors keeps that offset in
      // the saved layout even if this application only re-enables the dummy alone. Applying that
      // position verbatim would leave the compositor's own desktop geometry genuinely anchored
      // away from (0,0), which no amount of capture-side coordinate math can compensate for since
      // it's the compositor's own cursor/pointer boundary that ends up wrong, not just what
      // Sunshine computes. Normalize so the enabled outputs' bounding box always starts at (0,0).
      int min_x = std::numeric_limits<int>::max();
      int min_y = std::numeric_limits<int>::max();
      for (const auto &output : desired) {
        if (!output.enabled) {
          continue;
        }
        min_x = std::min(min_x, output.x);
        min_y = std::min(min_y, output.y);
      }
      if (min_x == std::numeric_limits<int>::max()) {
        min_x = 0;
      }
      if (min_y == std::numeric_limits<int>::max()) {
        min_y = 0;
      }

      kde_output_configuration = kde_output_management_v2_create_configuration(kde_output_management);
      kde_output_configuration_v2_add_listener(kde_output_configuration, &configuration_listener, this);

      struct kde_output_device_v2 *primary_device = nullptr;
      for (auto &[device, state] : devices) {
        auto it = std::ranges::find_if(desired, [&](const platf::display_output_t &output) {
          return output.id == state.name;
        });
        if (it == desired.end()) {
          continue;
        }

        kde_output_configuration_v2_enable(kde_output_configuration, device, it->enabled ? 1 : 0);
        if (it->enabled) {
          kde_output_configuration_v2_position(kde_output_configuration, device, it->x - min_x, it->y - min_y);
          int32_t desired_transform = degrees_to_kde_transform(it->rotation);
          if (desired_transform != state.transform) {
            kde_output_configuration_v2_transform(kde_output_configuration, device, desired_transform);
          }
        }
        if (it->primary) {
          primary_device = device;
        }
      }

      if (primary_device) {
        kde_output_configuration_v2_set_primary_output(kde_output_configuration, primary_device);
      }

      kde_output_configuration_v2_apply(kde_output_configuration);

      return wait_for_apply();
    }

    /**
     * @brief Set a bound device to a resolution/refresh rate already advertised by that device.
     *
     * Synthesizing genuinely new modes via kde_mode_list_v2 was tried and removed - KWin accepts
     * the request, but the underlying NVIDIA DRM-KMS driver rejects the atomic modeset test for
     * anything outside the display's own EDID mode list (confirmed via direct protocol testing:
     * kwin_wayland logs "Atomic modeset test failed! Invalid argument" for the synthesized mode).
     * That's a driver limitation below both KWin and Sunshine - only EDID-advertised modes work.
     *
     * @param output_id Device name (== id) to change.
     * @param width Desired width, in pixels.
     * @param height Desired height, in pixels.
     * @param refresh_rate Desired refresh rate, in Hz.
     * @return True if a matching mode was found and selected successfully.
     */
    bool apply_resolution(const std::string &output_id, int width, int height, double refresh_rate) {
      struct kde_output_device_v2 *target_device = nullptr;
      for (auto &[device, state] : devices) {
        if (state.name == output_id) {
          target_device = device;
          break;
        }
      }
      if (!target_device) {
        BOOST_LOG(warning) << "[kwingrab] set_display_resolution: no device named \""sv << output_id << "\""sv;
        return false;
      }

      // Remember the device's current mode so we can explicitly restore it if the compositor
      // rejects our requested mode. Left alone, a rejected modeset can take the compositor
      // several seconds to settle back to a valid state on its own, during which KMS capture
      // can't find the monitor at all - explicitly reselecting the previous mode avoids that.
      const auto &current_state = devices[target_device];
      const int previous_width = current_state.width;
      const int previous_height = current_state.height;
      const int previous_refresh_mhz = current_state.refresh_mhz;

      const auto refresh_mhz = static_cast<int32_t>(std::llround(refresh_rate * 1000.0));

      struct kde_output_device_mode_v2 *target_mode = find_mode(width, height, refresh_mhz);
      if (!target_mode) {
        target_mode = find_closest_mode(width, height, refresh_mhz);
        if (target_mode) {
          const auto &chosen = modes[target_mode];
          BOOST_LOG(info) << "[kwingrab] no exact mode for "sv << width << 'x' << height << '@' << refresh_rate
                           << ", using closest match "sv << chosen.width << 'x' << chosen.height << '@' << (chosen.refresh_mhz / 1000.0);
        }
      }
      if (!target_mode) {
        BOOST_LOG(warning) << "[kwingrab] no advertised mode matches "sv << width << 'x' << height << '@' << refresh_rate;
        return false;
      }

      if (!begin_configuration()) {
        return false;
      }
      kde_output_configuration_v2_mode(kde_output_configuration, target_device, target_mode);
      kde_output_configuration_v2_apply(kde_output_configuration);
      if (wait_for_apply()) {
        return true;
      }

      // The compositor/driver rejected the mode. Explicitly reselect whatever was active before,
      // instead of leaving the output in whatever state the failed apply left it in.
      BOOST_LOG(warning) << "[kwingrab] mode "sv << width << 'x' << height << '@' << refresh_rate
                          << " was rejected, restoring previous mode "sv << previous_width << 'x' << previous_height;
      struct kde_output_device_mode_v2 *previous_mode = find_mode(previous_width, previous_height, previous_refresh_mhz);
      if (previous_mode && begin_configuration()) {
        kde_output_configuration_v2_mode(kde_output_configuration, target_device, previous_mode);
        kde_output_configuration_v2_apply(kde_output_configuration);
        wait_for_apply();
      }
      return false;
    }

  private:
    /**
     * @brief Find an already-advertised mode matching the given resolution/refresh, within 50mHz.
     */
    struct kde_output_device_mode_v2 *find_mode(int width, int height, int32_t refresh_mhz) {
      for (auto &[mode, state] : modes) {
        if (state.width == width && state.height == height && std::abs(state.refresh_mhz - refresh_mhz) < 50) {
          return mode;
        }
      }
      return nullptr;
    }

    /**
     * @brief Find the best available substitute for a resolution with no exact advertised match.
     *
     * Ranks candidates by aspect-ratio closeness first (so a 1024x600 request prefers a 16:9 mode
     * over an equally-sized 4:3 one), then by smallest area among equally-close aspect ratios (so
     * it doesn't jump straight to 4K when a smaller mode fits just as well - e.g. 1024x600 lands on
     * 1280x720 rather than 2560x1440, both being 16:9). Modes at least as large as the request in
     * both dimensions are always preferred over smaller ones, since cropping is only possible when
     * the physical mode is at least the requested size. Refresh rate is the final tiebreaker.
     */
    struct kde_output_device_mode_v2 *find_closest_mode(int width, int height, int32_t refresh_mhz) {
      const double target_ar = static_cast<double>(width) / height;

      struct kde_output_device_mode_v2 *best = nullptr;
      bool best_fits = false;
      double best_ar_diff = std::numeric_limits<double>::max();
      long long best_area = std::numeric_limits<long long>::max();
      int32_t best_refresh_diff = std::numeric_limits<int32_t>::max();

      for (auto &[mode, state] : modes) {
        if (state.width <= 0 || state.height <= 0) {
          continue;
        }
        const bool fits = state.width >= width && state.height >= height;
        const double ar_diff = std::abs(static_cast<double>(state.width) / state.height - target_ar);
        const long long area = static_cast<long long>(state.width) * state.height;
        const int32_t refresh_diff = std::abs(state.refresh_mhz - refresh_mhz);

        // A mode that fits always beats one that doesn't, regardless of aspect ratio/size.
        if (fits != best_fits) {
          if (!fits) {
            continue;
          }
        } else if (std::abs(ar_diff - best_ar_diff) > 1e-6) {
          if (ar_diff > best_ar_diff) {
            continue;
          }
        } else if (fits ? area > best_area : area < best_area) {
          // Among equally-close aspect ratios: prefer the smallest fitting mode, or the largest
          // non-fitting one (closest we can offer when nothing actually contains the request).
          continue;
        } else if (area == best_area && refresh_diff >= best_refresh_diff) {
          continue;
        }

        best = mode;
        best_fits = fits;
        best_ar_diff = ar_diff;
        best_area = area;
        best_refresh_diff = refresh_diff;
      }

      return best;
    }

    /**
     * @brief Destroy any previous configuration object and start a fresh one, resetting apply state.
     * @return True on success.
     */
    bool begin_configuration() {
      if (kde_output_configuration) {
        kde_output_configuration_v2_destroy(kde_output_configuration);
        kde_output_configuration = nullptr;
      }
      apply_done = false;
      apply_succeeded = false;
      apply_failure_reason.clear();

      kde_output_configuration = kde_output_management_v2_create_configuration(kde_output_management);
      if (!kde_output_configuration) {
        return false;
      }
      kde_output_configuration_v2_add_listener(kde_output_configuration, &configuration_listener, this);
      return true;
    }

    int wait_for_apply() {
      // Dispatch until we get applied/failed, with a 5s timeout
      auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!apply_done && std::chrono::steady_clock::now() < deadline) {
        wl_display_flush(wl_display);

        struct pollfd pfd = {};
        pfd.fd = wl_display_get_fd(wl_display);
        pfd.events = POLLIN;

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()
        );
        if (remaining.count() <= 0) {
          break;
        }

        if (poll(&pfd, 1, remaining.count()) > 0 && (pfd.revents & POLLIN) && wl_display_dispatch(wl_display) < 0) {
          BOOST_LOG(error) << "[kwingrab] wl_display_dispatch failed while applying output layout"sv;
          return false;
        }
      }

      if (!apply_done) {
        BOOST_LOG(error) << "[kwingrab] timeout waiting for output configuration to apply"sv;
        return false;
      }
      if (!apply_failure_reason.empty()) {
        BOOST_LOG(error) << "[kwingrab] output configuration failed: "sv << apply_failure_reason;
      }
      return apply_succeeded;
    }

    // Wayland objects
    struct wl_display *wl_display = nullptr;
    struct wl_registry *wl_registry = nullptr;
    struct kde_output_management_v2 *kde_output_management = nullptr;
    struct kde_output_configuration_v2 *kde_output_configuration = nullptr;
    struct kde_output_device_registry_v2 *kde_output_device_registry = nullptr;
    std::map<struct kde_output_device_v2 *, kwin_device_state_t> devices;
    std::map<struct kde_output_device_mode_v2 *, kwin_mode_state_t> modes;
    bool apply_done = false;
    bool apply_succeeded = false;
    std::string apply_failure_reason;

    // wl_registry listener. Older plasma-wayland-protocols revisions advertised each
    // kde_output_device_v2 directly as a wl_registry global, the same way wl_output is bound
    // in screencast_t. Newer revisions (seen on KWin 6.7+) instead route discovery through a
    // kde_output_device_registry_v2 wrapper global, whose "output" event announces each device -
    // no direct kde_output_device_v2 globals appear in the registry at all in that case. Both
    // paths are handled here so this works across KWin versions.
    static void on_registry_global(void *data, struct wl_registry *reg, const uint32_t name, const char *interface, const uint32_t version) {
      auto *self = static_cast<output_management_t *>(data);
      if (!std::strcmp(interface, kde_output_device_v2_interface.name)) {
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(23));
        auto *device = static_cast<struct kde_output_device_v2 *>(
          wl_registry_bind(reg, name, &kde_output_device_v2_interface, bind_ver)
        );
        self->bind_device(device);
        BOOST_LOG(debug) << "[kwingrab] bound kde_output_device_v2 version "sv << bind_ver << " instance: "sv << device;
      } else if (!std::strcmp(interface, kde_output_management_v2_interface.name)) {
        // kde_output_management_v2 has no events (request-only), so unlike kde_output_device_v2
        // there's no ABI concern binding at whatever version the compositor advertises - needed
        // for set_primary_output (v2).
        self->kde_output_management = static_cast<struct kde_output_management_v2 *>(
          wl_registry_bind(reg, name, &kde_output_management_v2_interface, version)
        );
        BOOST_LOG(debug) << "[kwingrab] bound kde_output_management_v2 version "sv << version;
      } else if (!std::strcmp(interface, kde_output_device_registry_v2_interface.name)) {
        uint32_t bind_ver = std::min(version, static_cast<uint32_t>(23));
        self->kde_output_device_registry = static_cast<struct kde_output_device_registry_v2 *>(
          wl_registry_bind(reg, name, &kde_output_device_registry_v2_interface, bind_ver)
        );
        kde_output_device_registry_v2_add_listener(self->kde_output_device_registry, &device_registry_listener, self);
        BOOST_LOG(debug) << "[kwingrab] bound kde_output_device_registry_v2 version "sv << bind_ver;
      }
    }

    static void on_registry_global_remove(void *data [[maybe_unused]], struct wl_registry *reg [[maybe_unused]], uint32_t name [[maybe_unused]]) {
      // We don't handle output hot-unplug during a one-shot query/apply
    }

    static constexpr struct wl_registry_listener registry_listener = {
      .global = on_registry_global,
      .global_remove = on_registry_global_remove,
    };

    void bind_device(struct kde_output_device_v2 *device) {
      devices.try_emplace(device);
      kde_output_device_v2_add_listener(device, &device_listener, this);
    }

    // kde_output_device_registry_v2 listener - only used on KWin versions that route device
    // discovery through this wrapper instead of direct wl_registry globals (see on_registry_global).
    static void on_registry_output(void *data, struct kde_output_device_registry_v2 *registry [[maybe_unused]], struct kde_output_device_v2 *output) {
      auto *self = static_cast<output_management_t *>(data);
      self->bind_device(output);
      BOOST_LOG(debug) << "[kwingrab] bound kde_output_device_v2 via registry wrapper, instance: "sv << output;
    }

    static void on_registry_finished(void *data [[maybe_unused]], struct kde_output_device_registry_v2 *registry [[maybe_unused]]) {}

    static constexpr struct kde_output_device_registry_v2_listener device_registry_listener = {
      .finished = on_registry_finished,
      .output = on_registry_output,
    };

    // kde_output_device_v2 listener - bound at v23, so all events must be handled
    static void on_device_geometry(void *data, struct kde_output_device_v2 *device, int32_t x, int32_t y, int32_t physical_width [[maybe_unused]], int32_t physical_height [[maybe_unused]], int32_t subpixel [[maybe_unused]], const char *make [[maybe_unused]], const char *model [[maybe_unused]], int32_t transform) {
      auto *self = static_cast<output_management_t *>(data);
      auto &state = self->devices[device];
      state.x = x;
      state.y = y;
      state.transform = transform;
    }

    static void on_device_current_mode(void *data, struct kde_output_device_v2 *device, struct kde_output_device_mode_v2 *mode) {
      auto *self = static_cast<output_management_t *>(data);
      auto &state = self->devices[device];
      if (const auto it = self->modes.find(mode); it != self->modes.end()) {
        state.width = it->second.width;
        state.height = it->second.height;
        state.refresh_mhz = it->second.refresh_mhz;
      }
    }

    static void on_device_mode(void *data, struct kde_output_device_v2 *device [[maybe_unused]], struct kde_output_device_mode_v2 *mode) {
      auto *self = static_cast<output_management_t *>(data);
      self->modes.try_emplace(mode);
      kde_output_device_mode_v2_add_listener(mode, &mode_listener, self);
    }

    static void on_device_done(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]]) {}
    static void on_device_scale(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], wl_fixed_t factor [[maybe_unused]]) {}
    static void on_device_edid(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], const char *raw [[maybe_unused]]) {}

    static void on_device_enabled(void *data, struct kde_output_device_v2 *device, int32_t enabled) {
      auto *self = static_cast<output_management_t *>(data);
      self->devices[device].enabled = enabled != 0;
    }

    static void on_device_uuid(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], const char *uuid [[maybe_unused]]) {}
    static void on_device_serial_number(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], const char *serial_number [[maybe_unused]]) {}
    static void on_device_eisa_id(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], const char *eisa_id [[maybe_unused]]) {}
    static void on_device_capabilities(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t flags [[maybe_unused]]) {}
    static void on_device_overscan(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t overscan [[maybe_unused]]) {}
    static void on_device_vrr_policy(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t vrr_policy [[maybe_unused]]) {}
    static void on_device_rgb_range(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t rgb_range [[maybe_unused]]) {}

    static void on_device_name(void *data, struct kde_output_device_v2 *device, const char *name) {
      auto *self = static_cast<output_management_t *>(data);
      self->devices[device].name = name;
    }

    static void on_device_high_dynamic_range(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t hdr_enabled [[maybe_unused]]) {}
    static void on_device_sdr_brightness(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t sdr_brightness [[maybe_unused]]) {}
    static void on_device_wide_color_gamut(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t wcg_enabled [[maybe_unused]]) {}
    static void on_device_auto_rotate_policy(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t policy [[maybe_unused]]) {}
    static void on_device_icc_profile_path(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], const char *profile_path [[maybe_unused]]) {}
    static void on_device_brightness_metadata(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t max_peak_brightness [[maybe_unused]], uint32_t max_frame_average_brightness [[maybe_unused]], uint32_t min_brightness [[maybe_unused]]) {}
    static void on_device_brightness_overrides(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], int32_t max_peak_brightness [[maybe_unused]], int32_t max_average_brightness [[maybe_unused]], int32_t min_brightness [[maybe_unused]]) {}
    static void on_device_sdr_gamut_wideness(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t gamut_wideness [[maybe_unused]]) {}
    static void on_device_color_profile_source(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t source [[maybe_unused]]) {}
    static void on_device_brightness(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t brightness [[maybe_unused]]) {}
    static void on_device_color_power_tradeoff(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t preference [[maybe_unused]]) {}
    static void on_device_dimming(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t multiplier [[maybe_unused]]) {}
    static void on_device_replication_source(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], const char *source [[maybe_unused]]) {}
    static void on_device_ddc_ci_allowed(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t allowed [[maybe_unused]]) {}
    static void on_device_max_bits_per_color(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t max_bpc [[maybe_unused]]) {}
    static void on_device_max_bits_per_color_range(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t min_value [[maybe_unused]], uint32_t max_value [[maybe_unused]]) {}
    static void on_device_automatic_max_bits_per_color_limit(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t max_bpc_limit [[maybe_unused]]) {}
    static void on_device_edr_policy(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t policy [[maybe_unused]]) {}
    static void on_device_sharpness(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t sharpness [[maybe_unused]]) {}
    static void on_device_priority(void *data, struct kde_output_device_v2 *device, uint32_t priority) {
      auto *self = static_cast<output_management_t *>(data);
      self->devices[device].priority = priority;
    }
    static void on_device_auto_brightness(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t enabled [[maybe_unused]]) {}
    static void on_device_removed(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]]) {}
    static void on_device_hdr_icc_profile_path(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], const char *profile_path [[maybe_unused]]) {}
    static void on_device_hdr_color_profile_source(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t source [[maybe_unused]]) {}
    static void on_device_abm_level(void *data [[maybe_unused]], struct kde_output_device_v2 *device [[maybe_unused]], uint32_t level [[maybe_unused]]) {}

    static constexpr struct kde_output_device_v2_listener device_listener = {
      .geometry = on_device_geometry,
      .current_mode = on_device_current_mode,
      .mode = on_device_mode,
      .done = on_device_done,
      .scale = on_device_scale,
      .edid = on_device_edid,
      .enabled = on_device_enabled,
      .uuid = on_device_uuid,
      .serial_number = on_device_serial_number,
      .eisa_id = on_device_eisa_id,
      .capabilities = on_device_capabilities,
      .overscan = on_device_overscan,
      .vrr_policy = on_device_vrr_policy,
      .rgb_range = on_device_rgb_range,
      .name = on_device_name,
      .high_dynamic_range = on_device_high_dynamic_range,
      .sdr_brightness = on_device_sdr_brightness,
      .wide_color_gamut = on_device_wide_color_gamut,
      .auto_rotate_policy = on_device_auto_rotate_policy,
      .icc_profile_path = on_device_icc_profile_path,
      .brightness_metadata = on_device_brightness_metadata,
      .brightness_overrides = on_device_brightness_overrides,
      .sdr_gamut_wideness = on_device_sdr_gamut_wideness,
      .color_profile_source = on_device_color_profile_source,
      .brightness = on_device_brightness,
      .color_power_tradeoff = on_device_color_power_tradeoff,
      .dimming = on_device_dimming,
      .replication_source = on_device_replication_source,
      .ddc_ci_allowed = on_device_ddc_ci_allowed,
      .max_bits_per_color = on_device_max_bits_per_color,
      .max_bits_per_color_range = on_device_max_bits_per_color_range,
      .automatic_max_bits_per_color_limit = on_device_automatic_max_bits_per_color_limit,
      .edr_policy = on_device_edr_policy,
      .sharpness = on_device_sharpness,
      .priority = on_device_priority,
      .auto_brightness = on_device_auto_brightness,
      .removed = on_device_removed,
      .hdr_icc_profile_path = on_device_hdr_icc_profile_path,
      .hdr_color_profile_source = on_device_hdr_color_profile_source,
      .abm_level = on_device_abm_level,
    };

    // kde_output_device_mode_v2 listener
    static void on_mode_size(void *data, struct kde_output_device_mode_v2 *mode, int32_t width, int32_t height) {
      auto *self = static_cast<output_management_t *>(data);
      auto &m = self->modes[mode];
      m.width = width;
      m.height = height;
    }

    static void on_mode_refresh(void *data, struct kde_output_device_mode_v2 *mode, int32_t refresh) {
      auto *self = static_cast<output_management_t *>(data);
      self->modes[mode].refresh_mhz = refresh;
    }

    static void on_mode_preferred(void *data [[maybe_unused]], struct kde_output_device_mode_v2 *mode [[maybe_unused]]) {}
    static void on_mode_removed(void *data [[maybe_unused]], struct kde_output_device_mode_v2 *mode [[maybe_unused]]) {}
    static void on_mode_flags(void *data [[maybe_unused]], struct kde_output_device_mode_v2 *mode [[maybe_unused]], uint32_t flags [[maybe_unused]]) {}

    static constexpr struct kde_output_device_mode_v2_listener mode_listener = {
      .size = on_mode_size,
      .refresh = on_mode_refresh,
      .preferred = on_mode_preferred,
      .removed = on_mode_removed,
      .flags = on_mode_flags,
    };

    // kde_output_configuration_v2 listener
    static void on_configuration_applied(void *data, struct kde_output_configuration_v2 *config [[maybe_unused]]) {
      auto *self = static_cast<output_management_t *>(data);
      self->apply_succeeded = true;
      self->apply_done = true;
    }

    static void on_configuration_failed(void *data, struct kde_output_configuration_v2 *config [[maybe_unused]]) {
      auto *self = static_cast<output_management_t *>(data);
      self->apply_succeeded = false;
      self->apply_done = true;
    }

    static void on_configuration_failure_reason(void *data, struct kde_output_configuration_v2 *config [[maybe_unused]], const char *reason) {
      auto *self = static_cast<output_management_t *>(data);
      self->apply_failure_reason = reason;
    }

    static constexpr struct kde_output_configuration_v2_listener configuration_listener = {
      .applied = on_configuration_applied,
      .failed = on_configuration_failed,
      .failure_reason = on_configuration_failure_reason,
    };
  };
}  // namespace kwin

// Public API for misc.cpp
namespace platf {
  /**
   * @brief Create a KWin screencast display backend.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   * @return KWin/PipeWire display backend, or nullptr when initialization fails.
   */
  std::shared_ptr<display_t> kwin_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (!pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(hwdevice_type)) {
      BOOST_LOG(error) << "[kwingrab] Could not initialize pipewire-based display with the given hw device type."sv;
      return nullptr;
    }

    auto display = std::make_shared<kwin::kwin_t>();
    if (display->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return display;
  }

  /**
   * @brief Enumerate KWin screencast display names.
   *
   * @return KWin display names, or an empty list when KWin capture is unavailable.
   */
  std::vector<std::string> kwin_display_names() {
    if (has_elevated_privileges(false)) {
      // We're still in the probing phase of Sunshine startup. Dropping portal security early will break KMS.
      // Just return a dummy screen for now. Display re-enumeration after encoder probing will yield full result.
      std::vector<std::string> display_names;
      display_names.emplace_back("");
      return display_names;
    }

    const auto screencast = std::make_unique<kwin::screencast_t>();
    if (screencast->init() < 0) {
      return {};
    }
    return screencast->get_output_names();
  }

  /**
   * @brief Check whether KWin screencast capture is available.
   *
   * @return True when KWin capture support is available.
   */
  bool kwin_available() {
    // Init screencast without permission setup (to not cause unneeded logs / temporary desktop files) and check KWin availability
    if (const auto screencast = std::make_unique<kwin::screencast_t>(); screencast->init(false) < 0 || !screencast->kwin_available()) {
      return false;
    }
    return true;
  }

  /**
   * @brief Enumerate the live state of every KWin output device.
   *
   * @return KWin display outputs, or an empty list when the privileged
   *         output-management protocols are unavailable.
   */
  std::vector<display_output_t> kwin_enum_outputs() {
    kwin::output_management_t management;
    if (management.init() < 0) {
      return {};
    }
    return management.to_display_outputs();
  }

  /**
   * @brief Apply a desired arrangement of KWin output devices.
   *
   * @param desired Desired output states, typically from a saved layout.
   * @return True if the compositor applied the changes successfully.
   */
  bool kwin_apply_outputs(const std::vector<display_output_t> &desired) {
    kwin::output_management_t management;
    if (management.init() < 0) {
      return false;
    }
    return management.apply(desired);
  }

  /**
   * @brief Set a KWin output device to a resolution/refresh rate already advertised by that device.
   *
   * @param output_id Device name (== id) to change.
   * @param width Desired width, in pixels.
   * @param height Desired height, in pixels.
   * @param refresh_rate Desired refresh rate, in Hz.
   * @return True if a matching mode was found and applied successfully.
   */
  bool kwin_set_display_resolution(const std::string &output_id, int width, int height, double refresh_rate) {
    kwin::output_management_t management;
    if (management.init() < 0) {
      return false;
    }
    return management.apply_resolution(output_id, width, height, refresh_rate);
  }
}  // namespace platf
