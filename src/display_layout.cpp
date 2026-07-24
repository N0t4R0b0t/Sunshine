/**
 * @file src/display_layout.cpp
 * @brief Definitions for saving and applying named display output layouts.
 */
// standard includes
#include <algorithm>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "display_layout.h"
#include "file_handler.h"
#include "logging.h"

using namespace std::literals;

namespace display_layout {
  namespace {
    nlohmann::json to_json(const platf::display_output_t &output) {
      nlohmann::json j;
      j["id"] = output.id;
      j["friendly_name"] = output.friendly_name;
      j["connected"] = output.connected;
      j["enabled"] = output.enabled;
      j["primary"] = output.primary;
      j["x"] = output.x;
      j["y"] = output.y;
      j["width"] = output.width;
      j["height"] = output.height;
      j["refresh_rate"] = output.refresh_rate;
      j["rotation"] = output.rotation;
      return j;
    }

    platf::display_output_t output_from_json(const nlohmann::json &j) {
      platf::display_output_t output;
      output.id = j.value("id", std::string {});
      output.friendly_name = j.value("friendly_name", std::string {});
      output.connected = j.value("connected", false);
      output.enabled = j.value("enabled", false);
      output.primary = j.value("primary", false);
      output.x = j.value("x", 0);
      output.y = j.value("y", 0);
      output.width = j.value("width", 0);
      output.height = j.value("height", 0);
      output.refresh_rate = j.value("refresh_rate", 0.0);
      output.rotation = j.value("rotation", 0);
      return output;
    }

    nlohmann::json to_json(const layout_t &layout) {
      nlohmann::json j;
      j["name"] = layout.name;
      j["is_restore"] = layout.is_restore;
      j["is_streaming"] = layout.is_streaming;
      j["outputs"] = nlohmann::json::array();
      for (const auto &output : layout.outputs) {
        j["outputs"].push_back(to_json(output));
      }
      return j;
    }

    layout_t layout_from_json(const nlohmann::json &j) {
      layout_t layout;
      layout.name = j.value("name", std::string {});
      layout.is_restore = j.value("is_restore", false);
      layout.is_streaming = j.value("is_streaming", false);
      for (const auto &output_json : j.value("outputs", nlohmann::json::array())) {
        layout.outputs.push_back(output_from_json(output_json));
      }
      return layout;
    }
  }  // namespace

  std::vector<layout_t> load_layouts() {
    std::vector<layout_t> layouts;

    std::string content = file_handler::read_file(config::sunshine.file_layouts.c_str());
    if (content.empty()) {
      return layouts;
    }

    try {
      nlohmann::json file_tree = nlohmann::json::parse(content);
      for (const auto &layout_json : file_tree.value("layouts", nlohmann::json::array())) {
        layouts.push_back(layout_from_json(layout_json));
      }
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "display_layout::load_layouts: "sv << e.what();
    }

    return layouts;
  }

  bool save_layouts(const std::vector<layout_t> &layouts) {
    nlohmann::json file_tree;
    file_tree["layouts"] = nlohmann::json::array();
    for (const auto &layout : layouts) {
      file_tree["layouts"].push_back(to_json(layout));
    }

    return file_handler::write_file(config::sunshine.file_layouts.c_str(), file_tree.dump(4)) == 0;
  }

  std::optional<layout_t> find_restore_layout() {
    for (auto &layout : load_layouts()) {
      if (layout.is_restore) {
        return layout;
      }
    }
    return std::nullopt;
  }

  bool set_restore_layout(const std::string &name) {
    auto layouts = load_layouts();
    bool found = false;
    for (auto &layout : layouts) {
      layout.is_restore = !name.empty() && layout.name == name;
      found = found || layout.is_restore;
    }
    if (!name.empty() && !found) {
      BOOST_LOG(warning) << "display_layout::set_restore_layout: no layout named \""sv << name << "\" - clearing restore designation"sv;
    }
    return save_layouts(layouts);
  }

  bool apply_restore_layout() {
    auto restore_layout = find_restore_layout();
    if (!restore_layout) {
      return false;
    }

    BOOST_LOG(info) << "display_layout: applying restore layout \""sv << restore_layout->name << "\""sv;
    bool ok = platf::apply_display_outputs(restore_layout->outputs);
    if (!ok) {
      BOOST_LOG(warning) << "display_layout: failed to apply restore layout \""sv << restore_layout->name << "\""sv;
    }
    return ok;
  }

  std::optional<layout_t> find_streaming_layout() {
    for (auto &layout : load_layouts()) {
      if (layout.is_streaming) {
        return layout;
      }
    }
    return std::nullopt;
  }

  bool set_streaming_layout(const std::string &name) {
    auto layouts = load_layouts();
    bool found = false;
    for (auto &layout : layouts) {
      layout.is_streaming = !name.empty() && layout.name == name;
      found = found || layout.is_streaming;
    }
    if (!name.empty() && !found) {
      BOOST_LOG(warning) << "display_layout::set_streaming_layout: no layout named \""sv << name << "\" - clearing streaming designation"sv;
    }
    return save_layouts(layouts);
  }

  bool apply_streaming_layout() {
    auto streaming_layout = find_streaming_layout();
    if (!streaming_layout) {
      return false;
    }

    BOOST_LOG(info) << "display_layout: applying streaming layout \""sv << streaming_layout->name << "\""sv;
    bool ok = platf::apply_display_outputs(streaming_layout->outputs);
    if (!ok) {
      BOOST_LOG(warning) << "display_layout: failed to apply streaming layout \""sv << streaming_layout->name << "\""sv;
    }
    return ok;
  }
}  // namespace display_layout
