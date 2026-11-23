/**
 * @file src/platform/windows/usbip_gamepad.h
 * @brief Free virtual gamepads for Windows through usbip-win2.
 *
 * The backend runs a minimal USB/IP server on the loopback interface that exports one
 * emulated USB controller per connected gamepad (DualSense, DualShock 4, Xbox 360,
 * Switch Pro or a generic HID gamepad). The open source usbip-win2 client driver
 * attaches those devices, so Windows and games see them as real USB controllers.
 */
#pragma once

// standard includes
#include <memory>
#include <string>

// local includes
#include "src/platform/common.h"
#include "usbip_profiles.h"

namespace platf::usbip_gamepad {
  /**
   * @brief Maximum number of virtual controllers.
   */
  constexpr int MAX_GAMEPADS = 16;

  class server_t;

  /**
   * @brief Global state for the usbip-win2 gamepad backend.
   */
  struct context_t {
    context_t();
    ~context_t();
    context_t(const context_t &) = delete;
    context_t &operator=(const context_t &) = delete;

    std::unique_ptr<server_t> server;  ///< Lazily started USB/IP server.
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
   * @brief Create a virtual controller for a gamepad slot.
   * @param context Backend context.
   * @param id Gamepad identifiers.
   * @param profile Controller to emulate.
   * @param feedback_queue Queue for rumble, LED and trigger feedback.
   * @return 0 on success, -1 on failure.
   */
  int alloc_gamepad(context_t &context, const gamepad_id_t &id, profile_e profile, feedback_queue_t feedback_queue);

  /**
   * @brief Check whether a slot is handled by this backend.
   * @param context Backend context.
   * @param nr Global gamepad index.
   * @return True when the slot has a virtual controller.
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
   * @brief Unplug and destroy a virtual controller.
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
}  // namespace platf::usbip_gamepad
