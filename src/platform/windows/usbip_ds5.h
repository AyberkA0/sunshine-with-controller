/**
 * @file src/platform/windows/usbip_ds5.h
 * @brief Declarations for the free USB/IP based virtual DualSense backend.
 *
 * The backend runs a minimal USB/IP server on the loopback interface that exports
 * one emulated wired DualSense controller per connected gamepad. The open source
 * usbip-win2 client driver attaches those devices, so Windows and games see them
 * as real USB DualSense controllers.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <memory>
#include <string>

// local includes
#include "src/platform/common.h"

namespace platf::usbip_ds5 {
  /**
   * @brief Maximum number of virtual DualSense controllers.
   */
  constexpr int MAX_GAMEPADS = 16;

  class server_t;

  /**
   * @brief Global state for the USB/IP DualSense backend.
   */
  struct context_t {
    context_t();
    ~context_t();
    context_t(const context_t &) = delete;
    context_t &operator=(const context_t &) = delete;

    std::unique_ptr<server_t> server;  ///< Lazily started USB/IP server.
  };

  /**
   * @brief Build the 64 byte USB input report for a DualSense controller.
   */
  struct ds5_state_t {
    std::array<std::uint8_t, 64> report {};  ///< Current input report including report ID.
    std::uint8_t seq = 0;  ///< Report sequence counter.
    std::array<std::uint32_t, 2> touch_ids {UINT32_MAX, UINT32_MAX};  ///< Client pointer ID per touch slot.
    std::uint8_t touch_counter = 0;  ///< Increments for every new touch contact.
  };

  /**
   * @brief Return the path of usbip.exe from usbip-win2.
   * @return Path, or an empty string when usbip-win2 is not installed.
   */
  std::string find_usbip_exe();

  /**
   * @brief Check whether the backend can create gamepads.
   * @return True when usbip-win2 is installed.
   */
  bool available();

  /**
   * @brief Create a virtual DualSense for a gamepad slot.
   * @param context Backend context.
   * @param id Gamepad identifiers.
   * @param feedback_queue Queue for rumble, LED and trigger feedback.
   * @return 0 on success, -1 on failure.
   */
  int alloc_gamepad(context_t &context, const gamepad_id_t &id, feedback_queue_t feedback_queue);

  /**
   * @brief Check whether a slot is handled by this backend.
   * @param context Backend context.
   * @param nr Global gamepad index.
   * @return True when the slot has a virtual DualSense.
   */
  bool has_gamepad(context_t &context, int nr);

  /**
   * @brief Route feedback of an existing gamepad to a resumed client.
   * @param context Backend context.
   * @param id Gamepad identifiers.
   * @param feedback_queue New feedback queue.
   * @return 0 on success, -1 when the slot is unknown.
   */
  int rebind_gamepad(context_t &context, const gamepad_id_t &id, feedback_queue_t feedback_queue);

  /**
   * @brief Unplug and destroy a virtual DualSense.
   * @param context Backend context.
   * @param nr Global gamepad index.
   */
  void free_gamepad(context_t &context, int nr);

  /**
   * @brief Update buttons, sticks and triggers.
   * @param context Backend context.
   * @param nr Global gamepad index.
   * @param state Client gamepad state.
   */
  void gamepad_update(context_t &context, int nr, const gamepad_state_t &state);

  /**
   * @brief Update touchpad contacts.
   * @param context Backend context.
   * @param touch Touch event.
   */
  void gamepad_touch(context_t &context, const gamepad_touch_t &touch);

  /**
   * @brief Update accelerometer or gyroscope values.
   * @param context Backend context.
   * @param motion Motion event.
   */
  void gamepad_motion(context_t &context, const gamepad_motion_t &motion);

  /**
   * @brief Update battery state.
   * @param context Backend context.
   * @param battery Battery event.
   */
  void gamepad_battery(context_t &context, const gamepad_battery_t &battery);

  // Pure helpers, exposed for unit tests.

  /**
   * @brief Reset an input report to neutral values.
   * @param state DualSense state.
   */
  void reset_state(ds5_state_t &state);

  /**
   * @brief Apply buttons, sticks and triggers to an input report.
   * @param state DualSense state.
   * @param gamepad Client gamepad state.
   */
  void apply_gamepad_state(ds5_state_t &state, const gamepad_state_t &gamepad);

  /**
   * @brief Apply a touch event to an input report.
   * @param state DualSense state.
   * @param touch Touch event.
   */
  void apply_touch(ds5_state_t &state, const gamepad_touch_t &touch);

  /**
   * @brief Apply a motion sample to an input report.
   * @param state DualSense state.
   * @param motion Motion event.
   */
  void apply_motion(ds5_state_t &state, const gamepad_motion_t &motion);

  /**
   * @brief Apply a battery update to an input report.
   * @param state DualSense state.
   * @param battery Battery event.
   */
  void apply_battery(ds5_state_t &state, const gamepad_battery_t &battery);

  /**
   * @brief Return the HID report descriptor of the emulated controller.
   * @return Descriptor bytes.
   */
  const std::string &hid_report_descriptor();
}  // namespace platf::usbip_ds5
