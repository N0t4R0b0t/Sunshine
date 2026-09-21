/**
 * @file tests/unit/test_webrtc_input.cpp
 * @brief Tests for turning browser input events into Moonlight input packets.
 *
 * Expected packets here are written out byte by byte from the protocol, not produced by the same
 * code under test, so a mistake in the translator cannot also be a mistake in its own test. Sunshine's
 * own packet validator and the real `input::passthrough` are then used as independent judges.
 */
#include "../tests_common.h"

#ifdef SUNSHINE_BUILD_WEBRTC

  // standard includes
  #include <chrono>
  #include <cstdint>
  #include <format>
  #include <memory>
  #include <string>
  #include <thread>
  #include <vector>

  // local includes
  #include "src/config.h"
  #include "src/globals.h"
  #include "src/input.h"
  #include "src/platform/virtualhid_input.h"
  #include "src/webrtc/input_translate.h"

using namespace std::literals;

namespace {
  using bytes = std::vector<std::uint8_t>;

  /**
   * @brief Translate a message that is expected to succeed.
   *
   * @param message The JSON text.
   * @return The packet, or an empty vector after a test failure.
   */
  bytes translate(const std::string &message) {
    std::string error;
    const auto packet = webrtc::translate_input(message, error);
    EXPECT_TRUE(packet.has_value()) << message << " -> " << error;
    return packet.value_or(bytes {});
  }

  /**
   * @brief Translate a message that is expected to be rejected.
   *
   * @param message The JSON text.
   * @return The reason given.
   */
  std::string rejection(const std::string &message) {
    std::string error;
    const auto packet = webrtc::translate_input(message, error);
    EXPECT_FALSE(packet.has_value()) << message << " was accepted";
    EXPECT_FALSE(error.empty()) << message;
    return error;
  }

  /**
   * @brief Whether Sunshine's own validator accepts the packet.
   *
   * @param packet Packet bytes.
   * @return `true` if it is safe to process.
   */
  bool sunshine_accepts(const bytes &packet) {
    return input::testing::is_valid_input_packet(packet);
  }

  /**
   * @brief Keyboard output recorded instead of typed into the machine running the tests.
   */
  struct recorded_key_t {
    std::uint16_t key_code;  ///< Key, after Sunshine's own remapping.
    bool release;  ///< Whether it was a release.
  };
}  // namespace

// ---- exact packets, written out from the protocol ------------------------------------------------

TEST(WebrtcInputTranslateTest, KeyDownIsTheExactMoonlightPacket) {
  // size 10 (big-endian), magic 3 (little-endian), flags 0, key 0x8041 (little-endian), shift, padding.
  const bytes expected {0x00, 0x00, 0x00, 0x0A, 0x03, 0x00, 0x00, 0x00, 0x00, 0x41, 0x80, 0x01, 0x00, 0x00};
  EXPECT_EQ(translate(R"({"t":"kd","k":65,"m":1})"), expected);
}

TEST(WebrtcInputTranslateTest, KeyUpUsesTheReleaseMagic) {
  const bytes expected {0x00, 0x00, 0x00, 0x0A, 0x04, 0x00, 0x00, 0x00, 0x00, 0x41, 0x80, 0x00, 0x00, 0x00};
  EXPECT_EQ(translate(R"({"t":"ku","k":65})"), expected);
}

TEST(WebrtcInputTranslateTest, RelativeMouseMoveIsBigEndianAndSigned) {
  // size 8, magic 7, then dx = -5 (0xFFFB) and dy = 300 (0x012C), both big-endian.
  const bytes expected {0x00, 0x00, 0x00, 0x08, 0x07, 0x00, 0x00, 0x00, 0xFF, 0xFB, 0x01, 0x2C};
  EXPECT_EQ(translate(R"({"t":"mr","dx":-5,"dy":300})"), expected);
}

TEST(WebrtcInputTranslateTest, AbsoluteMouseMoveCarriesTheSurfaceSize) {
  // size 14, magic 5, x = 640, y = 360, unused = 0, width = 1280, height = 720, all big-endian.
  const bytes expected {0x00, 0x00, 0x00, 0x0E, 0x05, 0x00, 0x00, 0x00, 0x02, 0x80, 0x01, 0x68, 0x00, 0x00, 0x05, 0x00, 0x02, 0xD0};
  EXPECT_EQ(translate(R"({"t":"ma","x":640,"y":360,"w":1280,"h":720})"), expected);
}

TEST(WebrtcInputTranslateTest, MouseButtonsPressAndRelease) {
  const bytes down {0x00, 0x00, 0x00, 0x05, 0x08, 0x00, 0x00, 0x00, 0x03};
  const bytes up {0x00, 0x00, 0x00, 0x05, 0x09, 0x00, 0x00, 0x00, 0x01};
  EXPECT_EQ(translate(R"({"t":"mb","b":3,"d":true})"), down);
  EXPECT_EQ(translate(R"({"t":"mb","b":1,"d":false})"), up);
}

TEST(WebrtcInputTranslateTest, ScrollRepeatsTheAmountAndUsesBigEndian) {
  // Vertical: size 10, magic 0x0A, amount -120 (0xFF88) in both fields, then zero.
  const bytes vertical {0x00, 0x00, 0x00, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0xFF, 0x88, 0xFF, 0x88, 0x00, 0x00};
  EXPECT_EQ(translate(R"({"t":"sv","a":-120})"), vertical);

  // Horizontal: size 6, magic 0x55000001 (little-endian), amount 120.
  const bytes horizontal {0x00, 0x00, 0x00, 0x06, 0x01, 0x00, 0x00, 0x55, 0x00, 0x78};
  EXPECT_EQ(translate(R"({"t":"sh","a":120})"), horizontal);
}

TEST(WebrtcInputTranslateTest, TypedTextIsAsLongAsTheText) {
  // size is the magic plus the 2 text bytes; magic 0x17.
  const bytes expected {0x00, 0x00, 0x00, 0x06, 0x17, 0x00, 0x00, 0x00, 'h', 'i'};
  EXPECT_EQ(translate(R"({"t":"tx","s":"hi"})"), expected);

  // Multi-byte text counts in bytes, not characters.
  const auto accented = translate(R"({"t":"tx","s":"é"})");
  ASSERT_EQ(accented.size(), 8u + 2u);
  EXPECT_EQ(accented[8], 0xC3);
  EXPECT_EQ(accented[9], 0xA9);
}

TEST(WebrtcInputTranslateTest, ControllerArrivalDescribesTheController) {
  // size 12, magic 0x55000004, controller 2, type 1 (Xbox), capabilities 3, buttons 0x0000F7FF-ish (little-endian).
  const auto packet = translate(R"({"t":"ga","n":2,"y":1,"c":3,"f":4660})");
  const bytes expected {0x00, 0x00, 0x00, 0x0C, 0x04, 0x00, 0x00, 0x55, 0x02, 0x01, 0x03, 0x00, 0x34, 0x12, 0x00, 0x00};
  EXPECT_EQ(packet, expected);
}

TEST(WebrtcInputTranslateTest, ControllerStateHasTheFixedFramingBytes) {
  // Controller 1 with A (0x1000) and the right stick pushed; the constant bytes come from the protocol.
  const auto packet = translate(R"({"t":"gp","n":1,"b":4096,"lt":255,"rt":7,"lx":-1,"ly":32767,"rx":100,"ry":-32768})");
  // clang-format off
  const bytes expected {
    0x00, 0x00, 0x00, 0x1E,  // size 30
    0x0C, 0x00, 0x00, 0x00,  // magic 0x0C
    0x1A, 0x00,              // header B
    0x01, 0x00,              // controller 1
    0x02, 0x00,              // active mask: only controller 1
    0x14, 0x00,              // mid B
    0x00, 0x10,              // buttons: A
    0xFF, 0x07,              // triggers
    0xFF, 0xFF,              // left X = -1
    0xFF, 0x7F,              // left Y = 32767
    0x64, 0x00,              // right X = 100
    0x00, 0x80,              // right Y = -32768
    0x9C, 0x00,              // tail A
    0x00, 0x00,              // buttons, upper half
    0x55, 0x00,              // tail B
  };
  // clang-format on
  EXPECT_EQ(packet, expected);
}

TEST(WebrtcInputTranslateTest, ControllerButtonsAboveSixteenBitsGoInTheExtensionField) {
  // 0x00030000 has only bits above the first 16, which live in Sunshine's extension field.
  const auto packet = translate(R"({"t":"gp","n":0,"b":196608})");
  ASSERT_EQ(packet.size(), 34u);
  // The original 16 bits are at offsets 16-17, and the extension at 30-31 (28-29 is a constant).
  EXPECT_EQ(packet[16], 0x00);
  EXPECT_EQ(packet[17], 0x00);
  EXPECT_EQ(packet[30], 0x03);
  EXPECT_EQ(packet[31], 0x00);
}

TEST(WebrtcInputTranslateTest, ControllerMaskCanMarkAControllerRemoved) {
  const auto packet = translate(R"({"t":"gp","n":3,"m":0})");
  EXPECT_EQ(packet[12], 0x00);
  EXPECT_EQ(packet[13], 0x00);
}

TEST(WebrtcInputTranslateTest, LargeMovesAreClampedRatherThanRejected) {
  const auto packet = translate(R"({"t":"mr","dx":90000,"dy":-90000})");
  EXPECT_EQ(packet[8], 0x7F);
  EXPECT_EQ(packet[9], 0xFF);
  EXPECT_EQ(packet[10], 0x80);
  EXPECT_EQ(packet[11], 0x01);
}

// ---- Sunshine's own validator accepts everything we build ----------------------------------------

TEST(WebrtcInputTranslateTest, SunshineAcceptsEveryPacketWeBuild) {
  const std::vector<std::string> messages {
    R"({"t":"kd","k":65,"m":15})",
    R"({"t":"ku","k":254})",
    R"({"t":"mr","dx":1,"dy":-1})",
    R"({"t":"ma","x":0,"y":0,"w":1,"h":1})",
    R"({"t":"ma","x":32767,"y":32767,"w":32767,"h":32767})",
    R"({"t":"mb","b":1,"d":true})",
    R"({"t":"mb","b":5,"d":false})",
    R"({"t":"sv","a":120})",
    R"({"t":"sh","a":-120})",
    R"({"t":"tx","s":"a"})",
    R"({"t":"tx","s":"12345678901234567890123456789012"})",
    R"({"t":"ga","n":0})",
    R"({"t":"ga","n":15,"y":4})",
    R"({"t":"gp","n":0})",
    R"({"t":"gp","n":15,"b":2147483647,"lt":255,"rt":255,"lx":-32768,"ly":-32768,"rx":32767,"ry":32767})",
  };
  for (const auto &message : messages) {
    EXPECT_TRUE(sunshine_accepts(translate(message))) << message;
  }
}

// ---- bad input is refused -------------------------------------------------------------------------

TEST(WebrtcInputTranslateTest, RefusesMalformedMessages) {
  EXPECT_EQ(rejection("not json"), "input message is not valid JSON");
  EXPECT_EQ(rejection("[1,2]"), "input message is not an object");
  EXPECT_EQ(rejection("{}"), "input message has no type");
  EXPECT_EQ(rejection(R"({"t":7})"), "input message has no type");
  EXPECT_EQ(rejection(R"({"t":"fly"})"), "unknown input message type");
  EXPECT_EQ(rejection(""), "input message is not valid JSON");
}

TEST(WebrtcInputTranslateTest, RefusesOversizedMessages) {
  const std::string huge = R"({"t":"tx","s":")" + std::string(webrtc::max_input_message_size, 'a') + R"("})";
  EXPECT_EQ(rejection(huge), "input message too large");
}

TEST(WebrtcInputTranslateTest, RefusesMissingOrWrongFields) {
  EXPECT_NE(rejection(R"({"t":"kd"})").find("'k'"), std::string::npos);
  EXPECT_NE(rejection(R"({"t":"kd","k":"A"})").find("'k'"), std::string::npos);
  EXPECT_NE(rejection(R"({"t":"mr","dx":1})").find("'dy'"), std::string::npos);
  EXPECT_NE(rejection(R"({"t":"mb","b":1})").find("'d'"), std::string::npos);
  EXPECT_NE(rejection(R"({"t":"mb","b":1,"d":1})").find("'d'"), std::string::npos);
  EXPECT_NE(rejection(R"({"t":"tx"})").find("'s'"), std::string::npos);
  EXPECT_NE(rejection(R"({"t":"gp"})").find("'n'"), std::string::npos);
}

TEST(WebrtcInputTranslateTest, RefusesOutOfRangeValues) {
  EXPECT_NE(rejection(R"({"t":"kd","k":0})").find("range"), std::string::npos);
  EXPECT_NE(rejection(R"({"t":"kd","k":255})").find("range"), std::string::npos);
  EXPECT_NE(rejection(R"({"t":"kd","k":65,"m":16})").find("range"), std::string::npos);
  EXPECT_NE(rejection(R"({"t":"kd","k":65.5})").find("range"), std::string::npos);
  EXPECT_NE(rejection(R"({"t":"mb","b":0,"d":true})").find("range"), std::string::npos);
  EXPECT_NE(rejection(R"({"t":"mb","b":6,"d":true})").find("range"), std::string::npos);
  EXPECT_NE(rejection(R"({"t":"ga","n":16})").find("range"), std::string::npos);
  EXPECT_NE(rejection(R"({"t":"gp","n":-1})").find("range"), std::string::npos);
  EXPECT_NE(rejection(R"({"t":"gp","n":0,"lt":256})").find("range"), std::string::npos);
  EXPECT_NE(rejection(R"({"t":"gp","n":0,"lx":32768})").find("range"), std::string::npos);
}

TEST(WebrtcInputTranslateTest, RefusesAnAbsoluteMoveOnAnEmptySurface) {
  // A zero-sized surface would make the host divide by zero.
  EXPECT_NE(rejection(R"({"t":"ma","x":1,"y":1,"w":0,"h":720})").find("'w'"), std::string::npos);
  EXPECT_NE(rejection(R"({"t":"ma","x":1,"y":1,"w":1280,"h":0})").find("'h'"), std::string::npos);
}

TEST(WebrtcInputTranslateTest, RefusesBadText) {
  EXPECT_EQ(rejection(R"({"t":"tx","s":""})"), "text must be 1 to 32 bytes");
  EXPECT_EQ(rejection(R"({"t":"tx","s":"123456789012345678901234567890123"})"), "text must be 1 to 32 bytes");

  // 16 two-byte characters is 32 bytes and fine; 17 is 34 bytes and not.
  std::string sixteen;
  std::string seventeen;
  for (int i = 0; i < 17; ++i) {
    if (i < 16) {
      sixteen += "é";
    }
    seventeen += "é";
  }
  EXPECT_TRUE(sunshine_accepts(translate(R"({"t":"tx","s":")" + sixteen + R"("})")));
  EXPECT_EQ(rejection(R"({"t":"tx","s":")" + seventeen + R"("})"), "text must be 1 to 32 bytes");
}

TEST(WebrtcInputTranslateTest, RefusesTextThatIsNotUtf8) {
  // The JSON parser is what guarantees typed text is well-formed, so prove it refuses both a raw
  // stray byte and an escaped lone surrogate, neither of which is a character.
  EXPECT_EQ(rejection("{\"t\":\"tx\",\"s\":\"\xC3\"}"), "input message is not valid JSON");
  EXPECT_EQ(rejection(R"({"t":"tx","s":"\ud800"})"), "input message is not valid JSON");
}

// ---- through the real input path ------------------------------------------------------------------

namespace {
  /**
   * @brief Runs translated packets through the real `input::passthrough` with a fake backend.
   */
  class WebrtcInputPassthroughTest: public BaseTest {
  protected:
    void SetUp() override {
      BaseTest::SetUp();
      original_input_ = config::input;
      config::input.keyboard = true;
      config::input.mouse = true;
      config::input.controller = true;
      config::input.gamepad = "xseries";
      config::input.keybindings.clear();
      config::input.key_rightalt_to_key_win = false;
      config::input.key_repeat_delay = 0ms;

      auto platform_input = platf::input();
      ASSERT_TRUE(platform_input);
      auto &context = platf::virtualhid::get_input_context(platform_input);
      context = platf::virtualhid::input_context_t {lvh::BackendKind::fake};
      input::testing::set_platform_input(std::move(platform_input));
      input::testing::reset_keyboard_state();

      // Keys are recorded, never typed into the machine running the tests.
      input::testing::set_keyboard_sink([this](const input::testing::keyboard_event_t &event) {
        std::lock_guard lg {mutex_};
        keys_.push_back({event.key_code, event.release});
      });

      static int session = 0;
      stream_ = input::alloc(std::make_shared<safe::mail_raw_t>(), std::format("webrtc-input-test-{}", ++session));
      ASSERT_NE(stream_, nullptr);

      // Packets are handled on Sunshine's task pool, which only the real program starts. Another test
      // asserts it is *not* running, so it is stopped again in TearDown.
      ASSERT_FALSE(task_pool.running());
      task_pool.start(1);
    }

    void TearDown() override {
      task_pool.stop();
      task_pool.join();
      input::testing::set_keyboard_sink({});
      input::testing::reset_keyboard_state();
      input::terminate_gamepads();
      stream_.reset();
      input::testing::set_platform_input({});
      config::input = std::move(original_input_);
      BaseTest::TearDown();
    }

    /**
     * @brief Send a browser message through the translator and the real input path, and wait for it to be handled.
     *
     * @param message The browser's JSON message.
     */
    void send(const std::string &message) {
      input::passthrough(stream_, translate(message));

      // Packets are handled on a worker thread, so wait until the queue has drained.
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (input::testing::queued_input_packet_count(stream_) > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
      }
      // The last packet may still be running after it leaves the queue.
      std::this_thread::sleep_for(50ms);
    }

    /**
     * @brief Keys Sunshine emitted toward the platform.
     *
     * @return Copy of the recorded events.
     */
    std::vector<recorded_key_t> keys() {
      std::lock_guard lg {mutex_};
      return keys_;
    }

    std::shared_ptr<input::input_t> stream_;  ///< Input state for the fake session.

  private:
    config::input_t original_input_;  ///< Input configuration restored after each test.
    std::mutex mutex_;  ///< Guards `keys_`.
    std::vector<recorded_key_t> keys_;  ///< Keys the sink saw.
  };
}  // namespace

TEST_F(WebrtcInputPassthroughTest, KeyPressesReachTheKeyboardAsTheRightKey) {
  send(R"({"t":"kd","k":65})");
  send(R"({"t":"ku","k":65})");

  const auto seen = keys();
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_FALSE(seen[0].release);
  EXPECT_TRUE(seen[1].release);
  // Sunshine hands the platform layer the key with the Moonlight flag bit set, whichever key it is.
  EXPECT_EQ(seen[0].key_code & 0xFF, 0x41);
  EXPECT_EQ(seen[1].key_code & 0xFF, 0x41);
}

TEST_F(WebrtcInputPassthroughTest, DifferentKeysStayDifferent) {
  send(R"({"t":"kd","k":66})");
  send(R"({"t":"kd","k":112})");

  const auto seen = keys();
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[0].key_code & 0xFF, 0x42);
  EXPECT_EQ(seen[1].key_code & 0xFF, 0x70);
}

TEST_F(WebrtcInputPassthroughTest, ControllerArrivalAllocatesAControllerSlot) {
  ASSERT_LT(input::testing::gamepad_id(stream_, 2), 0);
  send(R"({"t":"ga","n":2})");
  EXPECT_GE(input::testing::gamepad_id(stream_, 2), 0);
}

TEST_F(WebrtcInputPassthroughTest, ControllerStateAllocatesAndThenReleasesAController) {
  ASSERT_LT(input::testing::gamepad_id(stream_, 0), 0);

  // A state report for a controller that is marked active creates it, as Moonlight's older clients do.
  send(R"({"t":"gp","n":0,"b":4096,"lx":1000})");
  EXPECT_GE(input::testing::gamepad_id(stream_, 0), 0);

  // Clearing its bit in the mask removes it again.
  send(R"({"t":"gp","n":0,"m":0})");
  EXPECT_LT(input::testing::gamepad_id(stream_, 0), 0);
}

TEST_F(WebrtcInputPassthroughTest, MouseAndScrollPacketsAreAcceptedWithoutDisturbingTheKeyboard) {
  send(R"({"t":"mr","dx":10,"dy":-10})");
  send(R"({"t":"mb","b":1,"d":true})");
  send(R"({"t":"mb","b":1,"d":false})");
  send(R"({"t":"sv","a":120})");
  send(R"({"t":"sh","a":-120})");
  send(R"({"t":"tx","s":"x"})");
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_), 0u);
  EXPECT_TRUE(keys().empty());
}

#endif  // SUNSHINE_BUILD_WEBRTC
