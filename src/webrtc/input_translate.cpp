/**
 * @file src/webrtc/input_translate.cpp
 * @brief Definitions for turning browser input events into Moonlight input packets.
 */
// standard includes
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

// lib includes
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
#include <nlohmann/json.hpp>

// local includes
#include "input_translate.h"

using namespace std::literals;

namespace webrtc {
  namespace {
    /**
     * @brief Thrown when a message is unusable; caught in translate_input and reported as the error.
     */
    struct rejected_t: std::runtime_error {
      using std::runtime_error::runtime_error;
    };

    constexpr std::size_t max_gamepads = 16;  ///< Highest controller index plus one, matching Sunshine's own limit.
    constexpr int max_utf8_bytes = UTF8_TEXT_EVENT_MAX_COUNT;  ///< Longest text event Moonlight allows.
    constexpr int key_code_flag = 0x8000;  ///< Moonlight sets this high bit on every virtual-key code it sends.

    /**
     * @brief The modifier keys a browser can report, as Moonlight represents them.
     */
    constexpr int all_modifiers = MODIFIER_SHIFT | MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_META;

    /**
     * @brief The buttons a standard controller reports, used when the browser does not list its own.
     */
    constexpr std::uint32_t standard_controller_buttons = A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG | LB_FLAG | RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG | RS_CLK_FLAG | SPECIAL_FLAG;

    /**
     * @brief Convert a 16-bit value to big-endian byte order, as the mouse and header fields use.
     *
     * @param value Value in host order.
     * @return The value, byte-swapped if the host is little-endian.
     */
    constexpr std::uint16_t to_big(std::uint16_t value) {
      return std::endian::native == std::endian::little ? std::byteswap(value) : value;
    }

    /**
     * @brief Convert a 32-bit value to big-endian byte order.
     *
     * @param value Value in host order.
     * @return The value, byte-swapped if the host is little-endian.
     */
    constexpr std::uint32_t to_big(std::uint32_t value) {
      return std::endian::native == std::endian::little ? std::byteswap(value) : value;
    }

    /**
     * @brief Convert a 16-bit value to little-endian byte order, as the keyboard and controller fields use.
     *
     * @param value Value in host order.
     * @return The value, byte-swapped if the host is big-endian.
     */
    constexpr std::uint16_t to_little(std::uint16_t value) {
      return std::endian::native == std::endian::little ? value : std::byteswap(value);
    }

    /**
     * @brief Convert a 32-bit value to little-endian byte order.
     *
     * @param value Value in host order.
     * @return The value, byte-swapped if the host is big-endian.
     */
    constexpr std::uint32_t to_little(std::uint32_t value) {
      return std::endian::native == std::endian::little ? value : std::byteswap(value);
    }

    /**
     * @brief Fill in a packet header. The size counts everything after the size field itself.
     *
     * @param header Header to fill.
     * @param packet_size Size of the whole packet including the header.
     * @param magic Packet type.
     */
    void set_header(NV_INPUT_HEADER &header, std::size_t packet_size, std::uint32_t magic) {
      header.size = to_big(static_cast<std::uint32_t>(packet_size - sizeof(header.size)));
      header.magic = to_little(magic);
    }

    /**
     * @brief Copy a finished packet struct into a byte vector.
     *
     * @param packet The packet.
     * @return The packet's bytes.
     */
    template<typename Packet>
    std::vector<std::uint8_t> bytes_of(const Packet &packet) {
      std::vector<std::uint8_t> bytes(sizeof(Packet));
      std::memcpy(bytes.data(), &packet, sizeof(Packet));
      return bytes;
    }

    /**
     * @brief Read a required numeric field, and reject the message if it is missing or the wrong kind.
     *
     * @param message The message.
     * @param key Field name.
     * @return The value.
     */
    double number(const nlohmann::json &message, const char *key) {
      const auto found = message.find(key);
      if (found == message.end() || !found->is_number()) {
        throw rejected_t {std::string {"missing or non-numeric field '"} + key + "'"};
      }
      const double value = found->get<double>();
      if (!std::isfinite(value)) {
        throw rejected_t {std::string {"field '"} + key + "' is not finite"};
      }
      return value;
    }

    /**
     * @brief Read a required whole number that must lie in a range. Out-of-range values are rejected.
     *
     * @param message The message.
     * @param key Field name.
     * @param low Smallest allowed value.
     * @param high Largest allowed value.
     * @return The value.
     */
    int integer_in(const nlohmann::json &message, const char *key, int low, int high) {
      const double value = number(message, key);
      if (value < low || value > high || value != std::floor(value)) {
        throw rejected_t {std::string {"field '"} + key + "' is out of range"};
      }
      return static_cast<int>(value);
    }

    /**
     * @brief Read a required number and clamp it into a range, for values like mouse deltas where
     *        a large one is just a fast move.
     *
     * @param message The message.
     * @param key Field name.
     * @param low Smallest value to use.
     * @param high Largest value to use.
     * @return The rounded and clamped value.
     */
    int clamped(const nlohmann::json &message, const char *key, int low, int high) {
      return static_cast<int>(std::clamp(std::lround(number(message, key)), static_cast<long>(low), static_cast<long>(high)));
    }

    /**
     * @brief Read an optional whole number in a range, using a default when absent.
     *
     * @param message The message.
     * @param key Field name.
     * @param fallback Value when the field is absent.
     * @param low Smallest allowed value.
     * @param high Largest allowed value.
     * @return The value.
     */
    int optional_integer(const nlohmann::json &message, const char *key, int fallback, int low, int high) {
      return message.contains(key) ? integer_in(message, key, low, high) : fallback;
    }

    /**
     * @brief Build a keyboard packet.
     *
     * @param message The message.
     * @param release Whether the key is being released.
     * @return The packet bytes.
     */
    std::vector<std::uint8_t> keyboard(const nlohmann::json &message, bool release) {
      // 0 is not a key, and 255 is beyond what a virtual-key code can be.
      const int key = integer_in(message, "k", 1, 254);
      const int modifiers = optional_integer(message, "m", 0, 0, all_modifiers);

      NV_KEYBOARD_PACKET packet {};
      set_header(packet.header, sizeof(packet), release ? KEY_UP_EVENT_MAGIC : KEY_DOWN_EVENT_MAGIC);
      packet.flags = 0;
      packet.keyCode = static_cast<short>(to_little(static_cast<std::uint16_t>(key_code_flag | key)));
      packet.modifiers = static_cast<char>(modifiers);
      packet.zero2 = 0;
      return bytes_of(packet);
    }

    /**
     * @brief Build a relative mouse move packet.
     *
     * @param message The message.
     * @return The packet bytes.
     */
    std::vector<std::uint8_t> mouse_relative(const nlohmann::json &message) {
      constexpr int limit = std::numeric_limits<std::int16_t>::max();

      NV_REL_MOUSE_MOVE_PACKET packet {};
      set_header(packet.header, sizeof(packet), MOUSE_MOVE_REL_MAGIC_GEN5);
      packet.deltaX = static_cast<short>(to_big(static_cast<std::uint16_t>(static_cast<std::int16_t>(clamped(message, "dx", -limit, limit)))));
      packet.deltaY = static_cast<short>(to_big(static_cast<std::uint16_t>(static_cast<std::int16_t>(clamped(message, "dy", -limit, limit)))));
      return bytes_of(packet);
    }

    /**
     * @brief Build an absolute mouse move packet.
     *
     * The position is measured in the browser's own surface, whose size is sent along so the host
     * can scale it to the screen.
     *
     * @param message The message.
     * @return The packet bytes.
     */
    std::vector<std::uint8_t> mouse_absolute(const nlohmann::json &message) {
      constexpr int limit = std::numeric_limits<std::int16_t>::max();

      // A zero-sized surface would make the host divide by zero.
      const int width = integer_in(message, "w", 1, limit);
      const int height = integer_in(message, "h", 1, limit);

      NV_ABS_MOUSE_MOVE_PACKET packet {};
      set_header(packet.header, sizeof(packet), MOUSE_MOVE_ABS_MAGIC);
      packet.x = static_cast<short>(to_big(static_cast<std::uint16_t>(clamped(message, "x", 0, width))));
      packet.y = static_cast<short>(to_big(static_cast<std::uint16_t>(clamped(message, "y", 0, height))));
      packet.unused = 0;
      packet.width = static_cast<short>(to_big(static_cast<std::uint16_t>(width)));
      packet.height = static_cast<short>(to_big(static_cast<std::uint16_t>(height)));
      return bytes_of(packet);
    }

    /**
     * @brief Build a mouse button packet.
     *
     * @param message The message.
     * @return The packet bytes.
     */
    std::vector<std::uint8_t> mouse_button(const nlohmann::json &message) {
      const int button = integer_in(message, "b", BUTTON_LEFT, BUTTON_X2);
      const auto down = message.find("d");
      if (down == message.end() || !down->is_boolean()) {
        throw rejected_t {"missing or non-boolean field 'd'"};
      }

      NV_MOUSE_BUTTON_PACKET packet {};
      set_header(packet.header, sizeof(packet), down->get<bool>() ? MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5 : MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5);
      packet.button = static_cast<std::uint8_t>(button);
      return bytes_of(packet);
    }

    /**
     * @brief Build a vertical scroll packet.
     *
     * @param message The message.
     * @return The packet bytes.
     */
    std::vector<std::uint8_t> scroll_vertical(const nlohmann::json &message) {
      constexpr int limit = std::numeric_limits<std::int16_t>::max();
      const auto amount = static_cast<short>(to_big(static_cast<std::uint16_t>(static_cast<std::int16_t>(clamped(message, "a", -limit, limit)))));

      NV_SCROLL_PACKET packet {};
      set_header(packet.header, sizeof(packet), SCROLL_MAGIC_GEN5);
      // Moonlight repeats the amount in both fields; the host only reads the first.
      packet.scrollAmt1 = amount;
      packet.scrollAmt2 = amount;
      packet.zero3 = 0;
      return bytes_of(packet);
    }

    /**
     * @brief Build a horizontal scroll packet.
     *
     * @param message The message.
     * @return The packet bytes.
     */
    std::vector<std::uint8_t> scroll_horizontal(const nlohmann::json &message) {
      constexpr int limit = std::numeric_limits<std::int16_t>::max();

      SS_HSCROLL_PACKET packet {};
      set_header(packet.header, sizeof(packet), SS_HSCROLL_MAGIC);
      packet.scrollAmount = static_cast<short>(to_big(static_cast<std::uint16_t>(static_cast<std::int16_t>(clamped(message, "a", -limit, limit)))));
      return bytes_of(packet);
    }

    /**
     * @brief Build a typed-text packet.
     *
     * This one is variable length: the size covers only the text actually present.
     *
     * @param message The message.
     * @return The packet bytes.
     */
    std::vector<std::uint8_t> text(const nlohmann::json &message) {
      const auto found = message.find("s");
      if (found == message.end() || !found->is_string()) {
        throw rejected_t {"missing or non-string field 's'"};
      }
      const auto value = found->get<std::string>();
      if (value.empty() || value.size() > static_cast<std::size_t>(max_utf8_bytes)) {
        throw rejected_t {"text must be 1 to 32 bytes"};
      }
      // Well-formed UTF-8 is already guaranteed: the JSON parser refuses ill-formed strings.

      NV_UNICODE_PACKET packet {};
      set_header(packet.header, sizeof(NV_INPUT_HEADER) + value.size(), UTF8_TEXT_EVENT_MAGIC);
      std::memcpy(packet.text, value.data(), value.size());

      std::vector<std::uint8_t> bytes(sizeof(NV_INPUT_HEADER) + value.size());
      std::memcpy(bytes.data(), &packet, bytes.size());
      return bytes;
    }

    /**
     * @brief Build a controller-connected packet.
     *
     * @param message The message.
     * @return The packet bytes.
     */
    std::vector<std::uint8_t> gamepad_arrival(const nlohmann::json &message) {
      SS_CONTROLLER_ARRIVAL_PACKET packet {};
      set_header(packet.header, sizeof(packet), SS_CONTROLLER_ARRIVAL_MAGIC);
      packet.controllerNumber = static_cast<std::uint8_t>(integer_in(message, "n", 0, static_cast<int>(max_gamepads) - 1));
      packet.type = static_cast<std::uint8_t>(optional_integer(message, "y", LI_CTYPE_XBOX, LI_CTYPE_UNKNOWN, LI_CTYPE_STEAM));
      packet.capabilities = to_little(static_cast<std::uint16_t>(optional_integer(message, "c", LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE, 0, 0xFFFF)));
      packet.supportedButtonFlags = to_little(static_cast<std::uint32_t>(optional_integer(message, "f", static_cast<int>(standard_controller_buttons), 0, std::numeric_limits<int>::max())));
      return bytes_of(packet);
    }

    /**
     * @brief Build a controller state packet.
     *
     * @param message The message.
     * @return The packet bytes.
     */
    std::vector<std::uint8_t> gamepad_state(const nlohmann::json &message) {
      constexpr int stick = std::numeric_limits<std::int16_t>::max();
      const int number_of_pad = integer_in(message, "n", 0, static_cast<int>(max_gamepads) - 1);

      // The mask says which controllers are connected. A bit that is clear for this controller means it
      // has been unplugged, so by default the controller reporting state is treated as connected.
      const int mask = optional_integer(message, "m", 1 << number_of_pad, 0, 0xFFFF);
      const int buttons = optional_integer(message, "b", 0, 0, std::numeric_limits<int>::max());

      NV_MULTI_CONTROLLER_PACKET packet {};
      set_header(packet.header, sizeof(packet), MULTI_CONTROLLER_MAGIC_GEN5);
      packet.headerB = static_cast<short>(to_little(static_cast<std::uint16_t>(MC_HEADER_B)));
      packet.controllerNumber = static_cast<short>(to_little(static_cast<std::uint16_t>(number_of_pad)));
      packet.activeGamepadMask = static_cast<short>(to_little(static_cast<std::uint16_t>(mask)));
      packet.midB = static_cast<short>(to_little(static_cast<std::uint16_t>(MC_MID_B)));
      packet.buttonFlags = static_cast<short>(to_little(static_cast<std::uint16_t>(buttons & 0xFFFF)));
      packet.leftTrigger = static_cast<unsigned char>(optional_integer(message, "lt", 0, 0, 255));
      packet.rightTrigger = static_cast<unsigned char>(optional_integer(message, "rt", 0, 0, 255));
      packet.leftStickX = static_cast<short>(to_little(static_cast<std::uint16_t>(static_cast<std::int16_t>(optional_integer(message, "lx", 0, -stick - 1, stick)))));
      packet.leftStickY = static_cast<short>(to_little(static_cast<std::uint16_t>(static_cast<std::int16_t>(optional_integer(message, "ly", 0, -stick - 1, stick)))));
      packet.rightStickX = static_cast<short>(to_little(static_cast<std::uint16_t>(static_cast<std::int16_t>(optional_integer(message, "rx", 0, -stick - 1, stick)))));
      packet.rightStickY = static_cast<short>(to_little(static_cast<std::uint16_t>(static_cast<std::int16_t>(optional_integer(message, "ry", 0, -stick - 1, stick)))));
      packet.tailA = static_cast<short>(to_little(static_cast<std::uint16_t>(MC_TAIL_A)));
      // Sunshine's extension carries the buttons that do not fit in the original 16 bits.
      packet.buttonFlags2 = static_cast<short>(to_little(static_cast<std::uint16_t>((buttons >> 16) & 0xFFFF)));
      packet.tailB = static_cast<short>(to_little(static_cast<std::uint16_t>(MC_TAIL_B)));
      return bytes_of(packet);
    }
  }  // namespace

  std::optional<std::vector<std::uint8_t>> translate_input(std::string_view message, std::string &error) {
    if (message.size() > max_input_message_size) {
      error = "input message too large";
      return std::nullopt;
    }

    try {
      const auto parsed = nlohmann::json::parse(message);
      if (!parsed.is_object()) {
        error = "input message is not an object";
        return std::nullopt;
      }

      const auto type = parsed.find("t");
      if (type == parsed.end() || !type->is_string()) {
        error = "input message has no type";
        return std::nullopt;
      }

      const auto kind = type->get<std::string>();
      if (kind == "mr") {
        return mouse_relative(parsed);
      }
      if (kind == "ma") {
        return mouse_absolute(parsed);
      }
      if (kind == "mb") {
        return mouse_button(parsed);
      }
      if (kind == "sv") {
        return scroll_vertical(parsed);
      }
      if (kind == "sh") {
        return scroll_horizontal(parsed);
      }
      if (kind == "kd") {
        return keyboard(parsed, false);
      }
      if (kind == "ku") {
        return keyboard(parsed, true);
      }
      if (kind == "tx") {
        return text(parsed);
      }
      if (kind == "ga") {
        return gamepad_arrival(parsed);
      }
      if (kind == "gp") {
        return gamepad_state(parsed);
      }

      error = "unknown input message type";
      return std::nullopt;
    } catch (const rejected_t &e) {
      error = e.what();
      return std::nullopt;
    } catch (const std::exception &) {
      error = "input message is not valid JSON";
      return std::nullopt;
    }
  }
}  // namespace webrtc
