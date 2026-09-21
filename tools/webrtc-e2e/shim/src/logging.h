/**
 * @file tools/webrtc-e2e/shim/src/logging.h
 * @brief Minimal stand-in for Sunshine's logging, so the browser session layer can be built without the rest of Sunshine.
 *
 * This shadows `src/logging.h` only when the include path puts this directory first, which only the
 * end-to-end host does. It is never part of the Sunshine build.
 */
#pragma once

// standard includes
#include <iostream>

/**
 * @brief Log sink that writes to standard error, one `LOG` line per statement.
 */
struct e2e_log_t {
  bool active = true;  ///< Whether this statement still owns the line it is writing.

  e2e_log_t() {
    std::cerr << "LOG ";
  }

  e2e_log_t(const e2e_log_t &) = delete;

  e2e_log_t(e2e_log_t &&other) noexcept:
      active {other.active} {
    other.active = false;
  }

  ~e2e_log_t() {
    if (active) {
      std::cerr << '\n';
    }
  }

  /**
   * @brief Append a value to the line.
   *
   * @param value Value to print.
   * @return This sink, for chaining.
   */
  template<typename T>
  e2e_log_t &operator<<(const T &value) {
    std::cerr << value;
    return *this;
  }
};

#define BOOST_LOG(level) e2e_log_t()
