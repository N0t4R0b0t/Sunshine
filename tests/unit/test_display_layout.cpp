/**
 * @file tests/unit/test_display_layout.cpp
 * @brief Test src/display_layout.*.
 */
#include "../tests_common.h"

#include <src/config.h>
#include <src/display_layout.h>

namespace {
  platf::display_output_t make_output(const std::string &id, bool primary) {
    platf::display_output_t output;
    output.id = id;
    output.friendly_name = "HDMI-" + id;
    output.connected = true;
    output.enabled = true;
    output.primary = primary;
    output.x = primary ? 0 : 1920;
    output.y = 0;
    output.width = 1920;
    output.height = 1080;
    output.refresh_rate = 60.0;
    return output;
  }
}  // namespace

class DisplayLayoutTest: public BaseTest {
protected:
  void SetUp() override {
    BaseTest::SetUp();
    config::sunshine.file_layouts = platf::appdata().string() + "/test_layouts.json";
  }
};

TEST_F(DisplayLayoutTest, LoadMissingFileReturnsEmpty) {
  config::sunshine.file_layouts = platf::appdata().string() + "/nonexistent_layouts.json";
  auto layouts = display_layout::load_layouts();
  EXPECT_TRUE(layouts.empty());
}

TEST_F(DisplayLayoutTest, SaveAndLoadRoundTrip) {
  std::vector<display_layout::layout_t> layouts;

  display_layout::layout_t docked;
  docked.name = "Docked";
  docked.outputs.push_back(make_output("0", true));
  docked.outputs.push_back(make_output("1", false));
  layouts.push_back(docked);

  display_layout::layout_t laptop_only;
  laptop_only.name = "Laptop Only";
  laptop_only.outputs.push_back(make_output("0", true));
  layouts.push_back(laptop_only);

  ASSERT_TRUE(display_layout::save_layouts(layouts));

  auto loaded = display_layout::load_layouts();
  ASSERT_EQ(loaded.size(), 2);

  EXPECT_EQ(loaded[0].name, "Docked");
  ASSERT_EQ(loaded[0].outputs.size(), 2);
  EXPECT_EQ(loaded[0].outputs[0].id, "0");
  EXPECT_TRUE(loaded[0].outputs[0].primary);
  EXPECT_EQ(loaded[0].outputs[0].width, 1920);
  EXPECT_EQ(loaded[0].outputs[1].x, 1920);
  EXPECT_FALSE(loaded[0].outputs[1].primary);

  EXPECT_EQ(loaded[1].name, "Laptop Only");
  ASSERT_EQ(loaded[1].outputs.size(), 1);
}

TEST_F(DisplayLayoutTest, SaveEmptyLayoutsOverwritesFile) {
  std::vector<display_layout::layout_t> layouts;
  display_layout::layout_t layout;
  layout.name = "Temp";
  layout.outputs.push_back(make_output("0", true));
  layouts.push_back(layout);
  ASSERT_TRUE(display_layout::save_layouts(layouts));
  ASSERT_FALSE(display_layout::load_layouts().empty());

  ASSERT_TRUE(display_layout::save_layouts({}));
  EXPECT_TRUE(display_layout::load_layouts().empty());
}
