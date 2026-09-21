/**
 * @file src/webrtc/input_translate.h
 * @brief Declarations for turning browser input events into Moonlight input packets.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webrtc {
  /**
   * @brief Largest browser input message that will be looked at, in bytes.
   *
   * Every real message is a few dozen bytes, so anything bigger is malformed or hostile.
   */
  constexpr std::size_t max_input_message_size = 1024;

  /**
   * @brief Turn one browser input message into a Moonlight input packet.
   *
   * The browser sends small JSON objects, each with a `t` field naming what happened:
   *
   * | `t`  | Meaning | Other fields |
   * | ---- | ------- | ------------ |
   * | `mr` | Relative mouse move | `dx`, `dy` |
   * | `ma` | Absolute mouse move | `x`, `y`, and the size `w`, `h` of the surface they are measured in |
   * | `mb` | Mouse button | `b` (1 left, 2 middle, 3 right, 4 and 5 side buttons), `d` (true when pressed) |
   * | `sv` | Vertical scroll | `a`, in 120ths of a wheel notch |
   * | `sh` | Horizontal scroll | `a`, in 120ths of a wheel notch |
   * | `kd`, `ku` | Key press, release | `k` (Windows virtual-key code), optional `m` (modifier bitmask) |
   * | `tx` | Typed text | `s`, at most 32 bytes of UTF-8 |
   * | `ga` | Gamepad connected | `n` (0 to 15), optional `y` (type), `c` (capabilities), `f` (supported buttons) |
   * | `gp` | Gamepad state | `n`, optional `m` (active mask), `b` (buttons), `lt`, `rt`, `lx`, `ly`, `rx`, `ry` |
   *
   * The packet it returns is what Moonlight would send, so it can go straight to `input::passthrough`.
   *
   * @param message The JSON text.
   * @param error Receives a short reason when the message is rejected.
   * @return The packet, or `std::nullopt` if the message is malformed, out of range, or not understood.
   */
  std::optional<std::vector<std::uint8_t>> translate_input(std::string_view message, std::string &error);
}  // namespace webrtc
