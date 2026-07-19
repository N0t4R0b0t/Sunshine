/**
 * @file src/display_layout.h
 * @brief Declarations for saving and applying named display output layouts.
 */
#pragma once

// standard includes
#include <optional>
#include <string>
#include <vector>

// local includes
#include "platform/common.h"

namespace display_layout {
  /**
   * @brief A named, saved arrangement of display outputs.
   */
  struct layout_t {
    std::string name;  ///< User-assigned layout name, unique within the saved set.
    std::vector<platf::display_output_t> outputs;  ///< Desired state of each output in this layout.
    bool is_restore = false;  ///< Whether this layout should be applied automatically on startup and client disconnect.
  };

  /**
   * @brief Load all saved layouts from `config::sunshine.file_layouts`.
   * @return Saved layouts, or an empty list if the file does not exist or fails to parse.
   */
  std::vector<layout_t> load_layouts();

  /**
   * @brief Persist the given layouts to `config::sunshine.file_layouts`, replacing any existing content.
   * @param layouts Layouts to persist.
   * @return `true` on success.
   */
  bool save_layouts(const std::vector<layout_t> &layouts);

  /**
   * @brief Find the layout currently designated to be applied automatically, if any.
   * @return The designated layout, or `std::nullopt` if none is designated.
   */
  std::optional<layout_t> find_restore_layout();

  /**
   * @brief Designate a layout to be applied automatically on startup and client disconnect,
   * clearing the designation from every other saved layout.
   * @param name Name of the layout to designate. An empty string, or a name that matches no
   * saved layout, clears the designation entirely.
   * @return `true` on success.
   */
  bool set_restore_layout(const std::string &name);

  /**
   * @brief Apply the designated restore layout, if any is set and it can be applied.
   * Safe to call unconditionally - it is a no-op when no restore layout is designated or
   * when the active platform/backend does not support applying layouts.
   * @return `true` if a restore layout was designated and applied successfully.
   */
  bool apply_restore_layout();
}  // namespace display_layout
