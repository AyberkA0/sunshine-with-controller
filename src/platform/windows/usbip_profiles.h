/**
 * @file src/platform/windows/usbip_profiles.h
 * @brief Emulated USB controller profiles for the usbip-win2 gamepad backend.
 *
 * Each profile describes one USB controller: its descriptors, how Moonlight input is
 * turned into USB input reports, and how output reports from games are turned into
 * feedback for the client. The USB/IP transport lives in usbip_gamepad.cpp.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "src/platform/common.h"

namespace platf::usbip_gamepad {
  constexpr std::int32_t ERR_PIPE = -32;  ///< -EPIPE, the endpoint stalls.

  /**
   * @brief Controllers that the backend can emulate.
   */
  enum class profile_e {
    ds5,  ///< Sony DualSense (054C:0CE6).
    ds4,  ///< Sony DualShock 4 v2 (054C:09CC).
    x360,  ///< Microsoft Xbox 360 wired controller (045E:028E), XInput.
    switch_pro,  ///< Nintendo Switch Pro Controller (057E:2009).
    generic,  ///< Generic DirectInput HID gamepad (1209:0001), like 8BitDo D-input mode.
  };

  /**
   * @brief Parse a gamepad name from the configuration.
   * @param name Configuration name such as "ds5" or "switch".
   * @return Profile, or nothing for "auto" and unknown names.
   */
  std::optional<profile_e> profile_from_name(std::string_view name);

  /**
   * @brief Return the configuration name of a profile.
   * @param profile Profile.
   * @return Name such as "ds5".
   */
  std::string_view profile_name(profile_e profile);

  /**
   * @brief Choose the profile for a new controller.
   * @param configured Configured gamepad type ("auto", "ds5", ...).
   * @param arrival Controller type and capabilities reported by the client.
   * @return Profile to emulate.
   */
  profile_e select_profile(std::string_view configured, const gamepad_arrival_t &arrival);

  /**
   * @brief One emulated USB controller.
   */
  class profile_t {
  public:
    virtual ~profile_t() = default;

    /// Function that receives feedback for the client. The message ID is filled in by the caller.
    std::function<void(gamepad_feedback_msg_t)> emit;

    /** @return Human readable name for logs. */
    virtual std::string_view display_name() const = 0;
    /** @return USB vendor ID. */
    virtual std::uint16_t vid() const = 0;
    /** @return USB product ID. */
    virtual std::uint16_t pid() const = 0;
    /** @return USB device release number. */
    virtual std::uint16_t bcd_device() const = 0;
    /** @return Interface class reported in the USB/IP device list. */
    virtual std::uint8_t interface_class() const {
      return 0x03;
    }
    /** @return USB device descriptor. */
    virtual std::string device_descriptor() const = 0;
    /** @return Full USB configuration descriptor. */
    virtual std::string config_descriptor() const = 0;
    /** @return Manufacturer string. */
    virtual std::wstring manufacturer() const = 0;
    /** @return Product string. */
    virtual std::wstring product() const = 0;
    /** @return Serial number string, empty when the device has none. */
    virtual std::wstring serial() const {
      return {};
    }
    /** @return HID report descriptor, empty for non-HID devices. */
    virtual std::string report_descriptor() const {
      return {};
    }
    /** @return Interrupt IN endpoint number without the direction bit. */
    virtual std::uint8_t in_endpoint() const = 0;
    /** @return Interrupt OUT endpoint number, 0 when the device has none. */
    virtual std::uint8_t out_endpoint() const = 0;

    /**
     * @brief Handle a class or vendor control request.
     * @param setup Setup packet.
     * @param out Data stage of an OUT request.
     * @param in Data stage of an IN request.
     * @return 0 or a negative errno such as ERR_PIPE.
     */
    virtual std::int32_t control(const std::uint8_t *setup, const std::vector<std::uint8_t> &out, std::string &in) {
      return ERR_PIPE;
    }

    /**
     * @brief Handle data written by the host to the OUT endpoint.
     * @param data Report data.
     * @param size Report size.
     */
    virtual void output(const std::uint8_t *data, std::size_t size) = 0;

    /**
     * @brief Produce the next report for the IN endpoint.
     * @param report Receives the report.
     * @return False when there is nothing to send right now.
     */
    virtual bool input(std::string &report) = 0;

    /** @brief Apply buttons, sticks and triggers. @param state Client state. */
    virtual void update(const gamepad_state_t &state) = 0;

    /** @brief Apply a touchpad event. @param touch Touch event. */
    virtual void touch(const gamepad_touch_t &touch) {}

    /** @brief Apply a motion sample. @param motion Motion sample. */
    virtual void motion(const gamepad_motion_t &motion) {}

    /** @brief Apply a battery update. @param battery Battery state. */
    virtual void battery(const gamepad_battery_t &battery) {}

    /** @return True when the controller has motion sensors. */
    virtual bool has_motion() const {
      return false;
    }

    /** @return True when the controller has a touchpad. */
    virtual bool has_touchpad() const {
      return false;
    }
  };

  /**
   * @brief Create a profile.
   * @param profile Profile type.
   * @param index Global gamepad index, used to make MAC addresses and serials unique.
   * @return New profile.
   */
  std::unique_ptr<profile_t> make_profile(profile_e profile, int index);

  /**
   * @brief Pack two 12-bit values the way Nintendo and Sony touch data do.
   * @param out Three output bytes.
   * @param a First value.
   * @param b Second value.
   */
  void pack12(std::uint8_t *out, std::uint16_t a, std::uint16_t b);

  /**
   * @brief Convert a Moonlight stick value to 0..255.
   * @param v Stick value.
   * @param invert True to flip the axis (Sony Y axes point down).
   * @return Byte value.
   */
  std::uint8_t stick_u8(std::int16_t v, bool invert);
}  // namespace platf::usbip_gamepad
