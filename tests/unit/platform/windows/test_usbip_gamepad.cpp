/**
 * @file tests/unit/platform/windows/test_usbip_gamepad.cpp
 * @brief Test the controller profiles of the usbip-win2 gamepad backend.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <vector>

  // local includes
  #include <src/platform/windows/usbip_profiles.h>

using namespace platf;
using namespace platf::usbip_gamepad;

namespace {
  std::uint8_t at(const std::string &s, std::size_t i) {
    return static_cast<std::uint8_t>(s.at(i));
  }

  std::string next_input(profile_t &profile) {
    std::string report;
    EXPECT_TRUE(profile.input(report));
    return report;
  }

  /**
   * @brief Collects feedback emitted by a profile.
   */
  struct feedback_sink_t {
    std::vector<gamepad_feedback_msg_t> messages;  ///< Emitted messages.

    void attach(profile_t &profile) {
      profile.emit = [this](gamepad_feedback_msg_t msg) {
        messages.push_back(msg);
      };
    }
  };
}  // namespace

/**
 * @brief Fixture for usbip-win2 profile tests.
 */
class UsbipGamepadTest: public BaseTest {};

TEST_F(UsbipGamepadTest, SelectsProfileFromClientType) {
  gamepad_arrival_t arrival {};
  arrival.type = LI_CTYPE_PS;
  EXPECT_EQ(select_profile("auto", arrival), profile_e::ds5);
  arrival.type = LI_CTYPE_XBOX;
  EXPECT_EQ(select_profile("auto", arrival), profile_e::x360);
  arrival.type = LI_CTYPE_NINTENDO;
  EXPECT_EQ(select_profile("auto", arrival), profile_e::switch_pro);
  arrival.type = LI_CTYPE_UNKNOWN;
  EXPECT_EQ(select_profile("auto", arrival), profile_e::x360);
  arrival.capabilities = LI_CCAP_GYRO;
  EXPECT_EQ(select_profile("auto", arrival), profile_e::ds5);
  EXPECT_EQ(select_profile("generic", arrival), profile_e::generic);
  EXPECT_EQ(select_profile("xseries", arrival), profile_e::x360);
}

TEST_F(UsbipGamepadTest, ProfileNamesRoundTrip) {
  for (const auto profile : {profile_e::ds5, profile_e::ds4, profile_e::x360, profile_e::switch_pro, profile_e::generic}) {
    EXPECT_EQ(profile_from_name(profile_name(profile)), profile);
  }
  EXPECT_FALSE(profile_from_name("auto").has_value());
}

TEST_F(UsbipGamepadTest, DescriptorsAreWellFormed) {
  for (const auto type : {profile_e::ds5, profile_e::ds4, profile_e::x360, profile_e::switch_pro, profile_e::generic}) {
    const auto profile = make_profile(type, 0);
    const auto device = profile->device_descriptor();
    ASSERT_EQ(device.size(), 18u);
    EXPECT_EQ(at(device, 8) | (at(device, 9) << 8), profile->vid());
    EXPECT_EQ(at(device, 10) | (at(device, 11) << 8), profile->pid());
    const auto config = profile->config_descriptor();
    EXPECT_EQ(at(config, 2) | (at(config, 3) << 8), config.size());
    const auto report = profile->report_descriptor();
    if (!report.empty()) {
      EXPECT_EQ(at(report, report.size() - 1), 0xC0);
    }
  }
}

TEST_F(UsbipGamepadTest, DualSenseButtonsAndSticks) {
  const auto profile = make_profile(profile_e::ds5, 0);
  gamepad_state_t gp {};
  gp.buttonFlags = A | Y | DPAD_UP | DPAD_RIGHT | LEFT_BUTTON | START | HOME | TOUCHPAD_BUTTON;
  gp.lsX = 32767;
  gp.lsY = 32767;
  gp.rsX = -32768;
  gp.rsY = -32768;
  gp.lt = 200;
  profile->update(gp);
  const auto r = next_input(*profile);
  ASSERT_EQ(r.size(), 64u);
  EXPECT_EQ(at(r, 1), 255);
  EXPECT_EQ(at(r, 2), 0);
  EXPECT_EQ(at(r, 3), 0);
  EXPECT_EQ(at(r, 4), 255);
  EXPECT_EQ(at(r, 5), 200);
  EXPECT_EQ(at(r, 8), 0x01 | 0x20 | 0x80);
  EXPECT_EQ(at(r, 9), 0x01 | 0x04 | 0x20);
  EXPECT_EQ(at(r, 10), 0x03);
}

TEST_F(UsbipGamepadTest, DualSenseTouchAndMotion) {
  const auto profile = make_profile(profile_e::ds5, 0);
  gamepad_touch_t touch {};
  touch.eventType = LI_TOUCH_EVENT_DOWN;
  touch.pointerId = 7;
  touch.x = 1.0f;
  touch.y = 1.0f;
  profile->touch(touch);
  gamepad_motion_t motion {};
  motion.motionType = LI_MOTION_TYPE_GYRO;
  motion.x = 10.0f;
  profile->motion(motion);
  auto r = next_input(*profile);
  EXPECT_EQ(at(r, 33) & 0x80, 0);
  EXPECT_EQ(at(r, 34) | ((at(r, 35) & 0x0F) << 8), 1919);
  EXPECT_EQ((at(r, 35) >> 4) | (at(r, 36) << 4), 1079);
  EXPECT_EQ(static_cast<std::int16_t>(at(r, 16) | (at(r, 17) << 8)), 160);

  touch.eventType = LI_TOUCH_EVENT_UP;
  profile->touch(touch);
  r = next_input(*profile);
  EXPECT_EQ(at(r, 33) & 0x80, 0x80);
}

TEST_F(UsbipGamepadTest, DualSenseRumbleAndLightbar) {
  const auto profile = make_profile(profile_e::ds5, 0);
  feedback_sink_t sink;
  sink.attach(*profile);
  std::uint8_t out[48] {0x02, 0x03, 0x04};
  out[3] = 0x40;
  out[4] = 0x80;
  out[45] = 255;
  out[47] = 128;
  profile->output(out, sizeof(out));
  ASSERT_EQ(sink.messages.size(), 2u);
  EXPECT_EQ(sink.messages[0].type, gamepad_feedback_e::rumble);
  EXPECT_EQ(sink.messages[0].data.rumble.lowfreq, 0x80 * 257);
  EXPECT_EQ(sink.messages[1].type, gamepad_feedback_e::set_rgb_led);
}

TEST_F(UsbipGamepadTest, DualShock4Report) {
  const auto profile = make_profile(profile_e::ds4, 0);
  gamepad_state_t gp {};
  gp.buttonFlags = B | HOME;
  gp.rt = 255;
  profile->update(gp);
  const auto r = next_input(*profile);
  ASSERT_EQ(r.size(), 64u);
  EXPECT_EQ(at(r, 5), 0x08 | 0x40);  // neutral hat, circle
  EXPECT_EQ(at(r, 6), 0x08);  // R2
  EXPECT_EQ(at(r, 7) & 0x03, 0x01);  // PS
  EXPECT_EQ(at(r, 9), 255);
}

TEST_F(UsbipGamepadTest, Xbox360ReportAndRumble) {
  const auto profile = make_profile(profile_e::x360, 0);
  feedback_sink_t sink;
  sink.attach(*profile);
  gamepad_state_t gp {};
  gp.buttonFlags = A | HOME | DPAD_LEFT;
  gp.lsY = 1000;
  profile->update(gp);
  const auto r = next_input(*profile);
  ASSERT_EQ(r.size(), 20u);
  EXPECT_EQ(at(r, 1), 0x14);
  EXPECT_EQ(at(r, 2), 0x04);
  EXPECT_EQ(at(r, 3), 0x10 | 0x04);
  EXPECT_EQ(static_cast<std::int16_t>(at(r, 8) | (at(r, 9) << 8)), 1000);

  const std::uint8_t rumble[8] {0x00, 0x08, 0x00, 0xFF, 0x10, 0, 0, 0};
  profile->output(rumble, sizeof(rumble));
  ASSERT_EQ(sink.messages.size(), 1u);
  EXPECT_EQ(sink.messages[0].data.rumble.lowfreq, 0xFF * 257);
  EXPECT_EQ(sink.messages[0].data.rumble.highfreq, 0x10 * 257);
}

TEST_F(UsbipGamepadTest, SwitchProHandshakeAndSpi) {
  const auto profile = make_profile(profile_e::switch_pro, 0);
  std::string r;
  EXPECT_FALSE(profile->input(r));  // silent until the host talks to it

  std::uint8_t status[64] {0x80, 0x01};
  profile->output(status, sizeof(status));
  r = next_input(*profile);
  EXPECT_EQ(at(r, 0), 0x81);
  EXPECT_EQ(at(r, 3), 0x03);

  std::uint8_t spi[64] {0x01, 0x00};
  spi[10] = 0x10;  // SPI read
  spi[11] = 0x3D;  // 0x603D, stick calibration
  spi[12] = 0x60;
  spi[15] = 18;
  profile->output(spi, sizeof(spi));
  r = next_input(*profile);
  EXPECT_EQ(at(r, 0), 0x21);
  EXPECT_EQ(at(r, 13), 0x90);
  EXPECT_EQ(at(r, 14), 0x10);
  EXPECT_EQ(at(r, 19), 18);  // length echo

  std::uint8_t usb_only[64] {0x80, 0x04};
  profile->output(usb_only, sizeof(usb_only));
  gamepad_state_t gp {};
  gp.buttonFlags = A;  // bottom button is B on Nintendo controllers
  gp.lsX = 32767;
  profile->update(gp);
  r = next_input(*profile);
  EXPECT_EQ(at(r, 0), 0x30);
  EXPECT_EQ(at(r, 3), 0x04);
  const int lx = at(r, 6) | ((at(r, 7) & 0x0F) << 8);
  EXPECT_GT(lx, 2048 + 1600);
}

TEST_F(UsbipGamepadTest, GenericReport) {
  const auto profile = make_profile(profile_e::generic, 0);
  gamepad_state_t gp {};
  gp.buttonFlags = X | START;
  gp.lt = 10;
  profile->update(gp);
  const auto r = next_input(*profile);
  ASSERT_EQ(r.size(), 9u);
  EXPECT_EQ(at(r, 0), 0x04);
  EXPECT_EQ(at(r, 1), 0x02);
  EXPECT_EQ(at(r, 2), 0x0F);
  EXPECT_EQ(at(r, 7), 10);
}

TEST_F(UsbipGamepadTest, PackTwelveBits) {
  std::uint8_t out[3];
  pack12(out, 0xABC, 0x123);
  EXPECT_EQ(out[0], 0xBC);
  EXPECT_EQ(out[1], 0x3A);
  EXPECT_EQ(out[2], 0x12);
  EXPECT_EQ(stick_u8(32767, false), 255);
  EXPECT_EQ(stick_u8(32767, true), 0);
}
#endif
