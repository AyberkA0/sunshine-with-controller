/**
 * @file tests/unit/platform/windows/test_usbip_ds5.cpp
 * @brief Test the report helpers of the USB/IP DualSense backend.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // local includes
  #include <src/platform/windows/usbip_ds5.h>

using namespace platf;
using namespace platf::usbip_ds5;

/**
 * @brief Fixture for USB/IP DualSense helper tests.
 */
class UsbipDs5Test: public BaseTest {
protected:
  ds5_state_t state;  ///< State under test.

  void SetUp() override {
    BaseTest::SetUp();
    reset_state(state);
  }
};

TEST_F(UsbipDs5Test, ResetIsNeutral) {
  EXPECT_EQ(state.report[0], 0x01);
  EXPECT_EQ(state.report[1], 0x80);
  EXPECT_EQ(state.report[4], 0x80);
  EXPECT_EQ(state.report[8], 0x08);
  EXPECT_EQ(state.report[33] & 0x80, 0x80);
  EXPECT_EQ(state.report[37] & 0x80, 0x80);
}

TEST_F(UsbipDs5Test, ButtonsAndSticks) {
  gamepad_state_t gp {};
  gp.buttonFlags = A | Y | DPAD_UP | DPAD_RIGHT | LEFT_BUTTON | START | HOME | TOUCHPAD_BUTTON;
  gp.lsX = 32767;
  gp.lsY = 32767;
  gp.rsX = -32768;
  gp.rsY = -32768;
  gp.lt = 200;
  apply_gamepad_state(state, gp);

  EXPECT_EQ(state.report[1], 255);
  EXPECT_EQ(state.report[2], 0);  // up is 0 on DualSense
  EXPECT_EQ(state.report[3], 0);
  EXPECT_EQ(state.report[4], 255);
  EXPECT_EQ(state.report[5], 200);
  EXPECT_EQ(state.report[8], 0x01 | 0x20 | 0x80);  // up-right hat, cross, triangle
  EXPECT_EQ(state.report[9], 0x01 | 0x04 | 0x20);  // L1, L2, options
  EXPECT_EQ(state.report[10], 0x03);  // PS, touchpad
}

TEST_F(UsbipDs5Test, TouchDownMoveUp) {
  gamepad_touch_t touch {};
  touch.eventType = LI_TOUCH_EVENT_DOWN;
  touch.pointerId = 7;
  touch.x = 1.0f;
  touch.y = 1.0f;
  apply_touch(state, touch);
  EXPECT_EQ(state.report[33] & 0x80, 0);
  const int x = state.report[34] | ((state.report[35] & 0x0F) << 8);
  const int y = (state.report[35] >> 4) | (state.report[36] << 4);
  EXPECT_EQ(x, 1919);
  EXPECT_EQ(y, 1079);

  touch.eventType = LI_TOUCH_EVENT_UP;
  apply_touch(state, touch);
  EXPECT_EQ(state.report[33] & 0x80, 0x80);
}

TEST_F(UsbipDs5Test, MotionScaling) {
  gamepad_motion_t motion {};
  motion.motionType = LI_MOTION_TYPE_GYRO;
  motion.x = 10.0f;
  apply_motion(state, motion);
  EXPECT_EQ(static_cast<std::int16_t>(state.report[16] | (state.report[17] << 8)), 160);
}

TEST_F(UsbipDs5Test, BatteryLevel) {
  gamepad_battery_t battery {};
  battery.state = LI_BATTERY_STATE_DISCHARGING;
  battery.percentage = 55;
  apply_battery(state, battery);
  EXPECT_EQ(state.report[53], 0x05);
}

TEST_F(UsbipDs5Test, ReportDescriptorIsClosed) {
  const auto &d = hid_report_descriptor();
  ASSERT_FALSE(d.empty());
  EXPECT_EQ(static_cast<std::uint8_t>(d.back()), 0xC0);
}
#endif
