/**
 * @file tools/webrtc-e2e/shim/src/crypto.h
 * @brief Minimal stand-in for the one Sunshine crypto helper the browser session layer uses.
 *
 * Only the end-to-end host builds against this; it is never part of the Sunshine build.
 */
#pragma once

// standard includes
#include <cstddef>
#include <string>
#include <string_view>

namespace crypto {
  /**
   * @brief Generate a random string.
   *
   * @param bytes Length of the string.
   * @param alphabet Characters to draw from.
   * @return The random string.
   */
  std::string rand_alphabet(std::size_t bytes, const std::string_view &alphabet);
}  // namespace crypto
