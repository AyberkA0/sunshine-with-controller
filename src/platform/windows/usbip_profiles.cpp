/**
 * @file src/platform/windows/usbip_profiles.cpp
 * @brief Emulated USB controller profiles for the usbip-win2 gamepad backend.
 *
 * Report layouts follow the Linux hid-playstation, hid-nintendo and xpad drivers,
 * and the public Nintendo Switch reverse engineering notes by dekuNukem.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

// local includes
#include "usbip_profiles.h"

using namespace std::literals;

namespace platf::usbip_gamepad {
  namespace {
    constexpr float EARTH_G = 9.80665f;

    void put_le16(std::uint8_t *p, std::int32_t v) {
      const auto u = static_cast<std::uint16_t>(static_cast<std::int16_t>(std::clamp<std::int32_t>(v, INT16_MIN, INT16_MAX)));
      p[0] = u & 0xFF;
      p[1] = u >> 8;
    }

    void put_le32(std::uint8_t *p, std::uint32_t v) {
      p[0] = v & 0xFF;
      p[1] = (v >> 8) & 0xFF;
      p[2] = (v >> 16) & 0xFF;
      p[3] = v >> 24;
    }

    std::string bytes(std::initializer_list<std::uint8_t> list) {
      return {list.begin(), list.end()};
    }

    std::string to_string(const std::vector<std::uint8_t> &v) {
      return {v.begin(), v.end()};
    }

    std::uint32_t now_us() {
      static const auto start = std::chrono::steady_clock::now();
      return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    }

    /**
     * @brief Build a standard 18 byte device descriptor.
     */
    std::string make_device_descriptor(std::uint8_t cls, std::uint8_t sub, std::uint8_t proto, std::uint8_t max_packet0, std::uint16_t vid, std::uint16_t pid, std::uint16_t bcd, bool has_serial) {
      return bytes({0x12, 0x01, 0x00, 0x02, cls, sub, proto, max_packet0, static_cast<std::uint8_t>(vid & 0xFF), static_cast<std::uint8_t>(vid >> 8), static_cast<std::uint8_t>(pid & 0xFF), static_cast<std::uint8_t>(pid >> 8), static_cast<std::uint8_t>(bcd & 0xFF), static_cast<std::uint8_t>(bcd >> 8), 0x01, 0x02, static_cast<std::uint8_t>(has_serial ? 3 : 0), 0x01});
    }

    /**
     * @brief Build a configuration with one HID interface.
     */
    std::string make_hid_config(std::size_t report_len, std::uint8_t in_ep, std::uint8_t out_ep, std::uint8_t packet, std::uint8_t interval) {
      const std::uint8_t total = 9 + 9 + 9 + 7 + (out_ep ? 7 : 0);
      std::string d = bytes({0x09, 0x02, total, 0x00, 0x01, 0x01, 0x00, 0xA0, 0xFA});
      d += bytes({0x09, 0x04, 0x00, 0x00, static_cast<std::uint8_t>(out_ep ? 2 : 1), 0x03, 0x00, 0x00, 0x00});
      d += bytes({0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, static_cast<std::uint8_t>(report_len & 0xFF), static_cast<std::uint8_t>(report_len >> 8)});
      d += bytes({0x07, 0x05, static_cast<std::uint8_t>(0x80 | in_ep), 0x03, packet, 0x00, interval});
      if (out_ep) {
        d += bytes({0x07, 0x05, out_ep, 0x03, packet, 0x00, interval});
      }
      return d;
    }

    /**
     * @brief Append "Report ID n, vendor usage, count bytes" items to a HID descriptor.
     */
    void add_vendor_reports(std::vector<std::uint8_t> &d, std::uint8_t main_item, std::initializer_list<std::pair<std::uint8_t, std::uint8_t>> reports, std::uint8_t usage) {
      d.insert(d.end(), {0x06, 0x00, 0xFF, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08});
      for (const auto &[id, size] : reports) {
        d.insert(d.end(), {0x85, id, 0x09, usage++, 0x95, size, main_item, 0x02});
      }
    }

    /**
     * @brief HID items for 6 axes (X Y Z Rz Rx Ry), 8 bit each.
     */
    void add_axes(std::vector<std::uint8_t> &d, std::initializer_list<std::uint8_t> usages) {
      d.insert(d.end(), {0x05, 0x01});
      for (auto u : usages) {
        d.insert(d.end(), {0x09, u});
      }
      d.insert(d.end(), {0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, static_cast<std::uint8_t>(usages.size()), 0x81, 0x02});
    }

    void add_hat(std::vector<std::uint8_t> &d) {
      d.insert(d.end(), {0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x65, 0x00, 0x45, 0x00});
    }

    void add_buttons(std::vector<std::uint8_t> &d, std::uint8_t count) {
      d.insert(d.end(), {0x05, 0x09, 0x19, 0x01, 0x29, count, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, count, 0x81, 0x02});
    }

    void add_padding_bits(std::vector<std::uint8_t> &d, std::uint8_t bits) {
      d.insert(d.end(), {0x75, 0x01, 0x95, bits, 0x81, 0x03});
    }

    std::uint8_t hat_value(std::uint32_t f) {
      const bool up = f & DPAD_UP;
      const bool down = f & DPAD_DOWN;
      const bool left = f & DPAD_LEFT;
      const bool right = f & DPAD_RIGHT;
      if (up && right) {
        return 1;
      }
      if (down && right) {
        return 3;
      }
      if (down && left) {
        return 5;
      }
      if (up && left) {
        return 7;
      }
      if (up) {
        return 0;
      }
      if (right) {
        return 2;
      }
      if (down) {
        return 4;
      }
      if (left) {
        return 6;
      }
      return 8;
    }

    /**
     * @brief Base class for HID controllers, handles the HID class requests.
     */
    class hid_profile_t: public profile_t {
    public:
      std::int32_t control(const std::uint8_t *setup, const std::vector<std::uint8_t> &out, std::string &in) override {
        const auto bm = setup[0];
        const auto req = setup[1];
        const auto type = setup[3];
        const auto id = setup[2];
        const auto length = static_cast<std::uint16_t>(setup[6] | (setup[7] << 8));
        if ((bm & 0x60) != 0x20) {
          return ERR_PIPE;
        }
        switch (req) {
          case 0x01:  // GET_REPORT
            if (type == 0x01) {
              in = current_input();
            } else if (type == 0x03) {
              in = feature(id, length);
              if (in.empty()) {
                return ERR_PIPE;
              }
            } else {
              return ERR_PIPE;
            }
            return 0;
          case 0x09:  // SET_REPORT
            if (type == 0x02 && !out.empty()) {
              output(out.data(), out.size());
            } else if (type == 0x03 && !out.empty()) {
              set_feature(out.data(), out.size());
            }
            return 0;
          case 0x02:  // GET_IDLE
            in = std::string(1, '\0');
            return 0;
          case 0x03:  // GET_PROTOCOL
            in = std::string(1, '\x01');
            return 0;
          default:  // SET_IDLE, SET_PROTOCOL
            return 0;
        }
      }

    protected:
      /** @return Current input report for GET_REPORT(Input). */
      virtual std::string current_input() = 0;

      /** @return Feature report, empty to stall. */
      virtual std::string feature(std::uint8_t id, std::uint16_t length) {
        std::string r(std::max<std::uint16_t>(length, 1), '\0');
        r[0] = static_cast<char>(id);
        return r;
      }

      /** @brief Handle SET_REPORT(Feature). */
      virtual void set_feature(const std::uint8_t *data, std::size_t size) {}
    };

    // ------------------------------------------------------------------------------------------
    // Sony DualSense and DualShock 4 share the touchpad and the motion calibration scheme.
    // ------------------------------------------------------------------------------------------

    constexpr int SONY_ACC_RES_PER_G = 8192;
    constexpr int SONY_GYRO_RAW_PER_DEG_S = 16;
    constexpr int SONY_GYRO_CAL_PLUS = 8640;
    constexpr int SONY_GYRO_CAL_SPEED = 540;

    /**
     * @brief Fill the Sony motion calibration layout used by both DS4 and DS5 over USB.
     */
    void fill_sony_calibration(std::uint8_t *p) {
      put_le16(p + 7, SONY_GYRO_CAL_PLUS);
      put_le16(p + 9, -SONY_GYRO_CAL_PLUS);
      put_le16(p + 11, SONY_GYRO_CAL_PLUS);
      put_le16(p + 13, -SONY_GYRO_CAL_PLUS);
      put_le16(p + 15, SONY_GYRO_CAL_PLUS);
      put_le16(p + 17, -SONY_GYRO_CAL_PLUS);
      put_le16(p + 19, SONY_GYRO_CAL_SPEED);
      put_le16(p + 21, SONY_GYRO_CAL_SPEED);
      for (int axis = 0; axis < 3; ++axis) {
        put_le16(p + 23 + axis * 4, SONY_ACC_RES_PER_G);
        put_le16(p + 25 + axis * 4, -SONY_ACC_RES_PER_G);
      }
    }

    /**
     * @brief Two-finger touchpad state shared by Sony controllers.
     */
    struct sony_touch_t {
      std::array<std::uint32_t, 2> ids {UINT32_MAX, UINT32_MAX};  ///< Client pointer per slot.
      std::uint8_t counter = 0;  ///< Contact counter.

      /**
       * @brief Apply a touch event to two 4 byte contact records.
       * @param touch Touch event.
       * @param points Pointers to the two contact records.
       * @param width Touchpad width in device units.
       * @param height Touchpad height in device units.
       * @return True when the report changed.
       */
      bool apply(const gamepad_touch_t &touch, std::array<std::uint8_t *, 2> points, std::uint16_t width, std::uint16_t height) {
        const auto release = [&](int slot) {
          points[slot][0] |= 0x80;
          ids[slot] = UINT32_MAX;
        };
        if (touch.eventType == LI_TOUCH_EVENT_CANCEL_ALL) {
          release(0);
          release(1);
          return true;
        }
        int slot = -1;
        for (int i = 0; i < 2; ++i) {
          if (ids[i] == touch.pointerId) {
            slot = i;
          }
        }
        if (touch.eventType == LI_TOUCH_EVENT_UP || touch.eventType == LI_TOUCH_EVENT_CANCEL) {
          if (slot >= 0) {
            release(slot);
          }
          return slot >= 0;
        }
        if (touch.eventType != LI_TOUCH_EVENT_DOWN && touch.eventType != LI_TOUCH_EVENT_MOVE) {
          return false;
        }
        if (slot < 0) {
          for (int i = 0; i < 2 && slot < 0; ++i) {
            if (ids[i] == UINT32_MAX) {
              slot = i;
            }
          }
          if (slot < 0) {
            return false;
          }
          ids[slot] = touch.pointerId;
          points[slot][0] = counter++ & 0x7F;
        }
        const auto x = static_cast<std::uint16_t>(std::clamp(touch.x, 0.0f, 1.0f) * (width - 1));
        const auto y = static_cast<std::uint16_t>(std::clamp(touch.y, 0.0f, 1.0f) * (height - 1));
        points[slot][0] &= 0x7F;
        pack12(points[slot] + 1, x, y);
        return true;
      }
    };

    void apply_sony_motion(std::uint8_t *gyro, std::uint8_t *accel, const gamepad_motion_t &motion) {
      if (motion.motionType == LI_MOTION_TYPE_ACCEL) {
        put_le16(accel + 0, static_cast<std::int32_t>(motion.x / EARTH_G * SONY_ACC_RES_PER_G));
        put_le16(accel + 2, static_cast<std::int32_t>(motion.y / EARTH_G * SONY_ACC_RES_PER_G));
        put_le16(accel + 4, static_cast<std::int32_t>(motion.z / EARTH_G * SONY_ACC_RES_PER_G));
      } else if (motion.motionType == LI_MOTION_TYPE_GYRO) {
        put_le16(gyro + 0, static_cast<std::int32_t>(motion.x * SONY_GYRO_RAW_PER_DEG_S));
        put_le16(gyro + 2, static_cast<std::int32_t>(motion.y * SONY_GYRO_RAW_PER_DEG_S));
        put_le16(gyro + 4, static_cast<std::int32_t>(motion.z * SONY_GYRO_RAW_PER_DEG_S));
      }
    }

    /**
     * @brief Sony DualSense, wired.
     */
    class ds5_t: public hid_profile_t {
      static constexpr std::size_t LX = 1, LY = 2, RX = 3, RY = 4, L2 = 5, R2 = 6, SEQ = 7, BTN0 = 8, BTN1 = 9, BTN2 = 10;
      static constexpr std::size_t GYRO = 16, ACCEL = 22, TIMESTAMP = 28, TOUCH0 = 33, TOUCH1 = 37, STATUS = 53;
      static constexpr std::array<std::pair<std::uint8_t, std::uint8_t>, 18> FEATURES {{
        {0x05, 40},
        {0x08, 47},
        {0x09, 19},
        {0x0A, 26},
        {0x20, 63},
        {0x21, 3},
        {0x22, 63},
        {0x80, 63},
        {0x81, 63},
        {0x82, 9},
        {0x83, 63},
        {0x84, 63},
        {0x85, 2},
        {0xA0, 1},
        {0xE0, 63},
        {0xF0, 63},
        {0xF1, 63},
        {0xF2, 15},
      }};

    public:
      explicit ds5_t(int index):
          index {index} {
        report.fill(0);
        report[0] = 0x01;
        report[LX] = report[LY] = report[RX] = report[RY] = 0x80;
        report[BTN0] = 0x08;
        report[TOUCH0] = report[TOUCH1] = 0x80;
        report[STATUS] = 0x1A;
        put_le16(&report[ACCEL + 2], SONY_ACC_RES_PER_G);
      }

      std::string_view display_name() const override {
        return "DualSense"sv;
      }

      std::uint16_t vid() const override {
        return 0x054C;
      }

      std::uint16_t pid() const override {
        return 0x0CE6;
      }

      std::uint16_t bcd_device() const override {
        return 0x0100;
      }

      std::string device_descriptor() const override {
        return make_device_descriptor(0, 0, 0, 64, vid(), pid(), bcd_device(), false);
      }

      std::string config_descriptor() const override {
        return make_hid_config(report_descriptor().size(), 4, 3, 64, 4);
      }

      std::wstring manufacturer() const override {
        return L"Sony Interactive Entertainment";
      }

      std::wstring product() const override {
        return L"DualSense Wireless Controller";
      }

      std::string report_descriptor() const override {
        std::vector<std::uint8_t> d {0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01};
        add_axes(d, {0x30, 0x31, 0x32, 0x35, 0x33, 0x34});
        d.insert(d.end(), {0x06, 0x00, 0xFF, 0x09, 0x20, 0x95, 0x01, 0x81, 0x02});
        add_hat(d);
        add_buttons(d, 15);
        d.insert(d.end(), {0x06, 0x00, 0xFF, 0x09, 0x21, 0x95, 0x0D, 0x81, 0x02});
        d.insert(d.end(), {0x06, 0x00, 0xFF, 0x09, 0x22, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x34, 0x81, 0x02});
        add_vendor_reports(d, 0x91, {{0x02, 47}}, 0x23);
        std::uint8_t usage = 0x24;
        for (const auto &[id, size] : FEATURES) {
          d.insert(d.end(), {0x85, id, 0x09, usage++, 0x95, size, 0xB1, 0x02});
        }
        d.push_back(0xC0);
        return to_string(d);
      }

      std::uint8_t in_endpoint() const override {
        return 4;
      }

      std::uint8_t out_endpoint() const override {
        return 3;
      }

      bool has_motion() const override {
        return true;
      }

      bool has_touchpad() const override {
        return true;
      }

      void update(const gamepad_state_t &gp) override {
        report[LX] = stick_u8(gp.lsX, false);
        report[LY] = stick_u8(gp.lsY, true);
        report[RX] = stick_u8(gp.rsX, false);
        report[RY] = stick_u8(gp.rsY, true);
        report[L2] = gp.lt;
        report[R2] = gp.rt;
        const auto f = gp.buttonFlags;
        report[BTN0] = hat_value(f) | ((f & X) ? 0x10 : 0) | ((f & A) ? 0x20 : 0) | ((f & B) ? 0x40 : 0) | ((f & Y) ? 0x80 : 0);
        report[BTN1] = ((f & LEFT_BUTTON) ? 0x01 : 0) | ((f & RIGHT_BUTTON) ? 0x02 : 0) | (gp.lt > 0 ? 0x04 : 0) | (gp.rt > 0 ? 0x08 : 0) |
                       ((f & BACK) ? 0x10 : 0) | ((f & START) ? 0x20 : 0) | ((f & LEFT_STICK) ? 0x40 : 0) | ((f & RIGHT_STICK) ? 0x80 : 0);
        report[BTN2] = ((f & HOME) ? 0x01 : 0) | ((f & TOUCHPAD_BUTTON) ? 0x02 : 0) | ((f & MISC_BUTTON) ? 0x04 : 0);
      }

      void touch(const gamepad_touch_t &t) override {
        touchpad.apply(t, {&report[TOUCH0], &report[TOUCH1]}, 1920, 1080);
      }

      void motion(const gamepad_motion_t &m) override {
        apply_sony_motion(&report[GYRO], &report[ACCEL], m);
      }

      void battery(const gamepad_battery_t &battery) override {
        auto &status = report[STATUS];
        std::uint8_t level = status & 0x0F;
        std::uint8_t charge = status >> 4;
        switch (battery.state) {
          case LI_BATTERY_STATE_CHARGING:
            charge = 0x1;
            break;
          case LI_BATTERY_STATE_DISCHARGING:
            charge = 0x0;
            break;
          case LI_BATTERY_STATE_FULL:
            charge = 0x2;
            level = 10;
            break;
          case LI_BATTERY_STATE_NOT_PRESENT:
          case LI_BATTERY_STATE_NOT_CHARGING:
            charge = 0xF;
            break;
          default:
            break;
        }
        if (battery.percentage != LI_BATTERY_PERCENTAGE_UNKNOWN) {
          level = static_cast<std::uint8_t>(std::min(10, battery.percentage / 10));
        }
        status = static_cast<std::uint8_t>((charge << 4) | (level & 0x0F));
      }

      bool input(std::string &out) override {
        report[SEQ] = seq++;
        put_le32(&report[TIMESTAMP], now_us() * 3);
        out.assign(reinterpret_cast<const char *>(report.data()), report.size());
        return true;
      }

      void output(const std::uint8_t *d, std::size_t n) override {
        if (n < 48 || d[0] != 0x02) {
          return;
        }
        const auto flags0 = d[1];
        const auto flags1 = d[2];
        if (flags0 & 0x03) {
          const std::uint16_t low = d[4] * 257;
          const std::uint16_t high = d[3] * 257;
          if (low != last_low || high != last_high) {
            last_low = low;
            last_high = high;
            emit(gamepad_feedback_msg_t::make_rumble(0, low, high));
          }
        }
        if (flags0 & 0x0C) {
          std::array<std::uint8_t, 22> triggers {};
          std::memcpy(triggers.data(), d + 11, 22);
          if (triggers != last_triggers) {
            last_triggers = triggers;
            std::array<std::uint8_t, 10> right {};
            std::array<std::uint8_t, 10> left {};
            std::memcpy(right.data(), d + 12, 10);
            std::memcpy(left.data(), d + 23, 10);
            emit(gamepad_feedback_msg_t::make_adaptive_triggers(0, flags0 & 0x0C, d[22], d[11], left, right));
          }
        }
        if (flags1 & 0x04) {
          const std::array<std::uint8_t, 3> rgb {d[45], d[46], d[47]};
          if (rgb != last_rgb) {
            last_rgb = rgb;
            emit(gamepad_feedback_msg_t::make_rgb_led(0, rgb[0], rgb[1], rgb[2]));
          }
        }
        if (flags1 & 0x10) {
          const std::uint8_t player = d[44] & 0x1F;
          if (player != last_player) {
            last_player = player;
            emit(gamepad_feedback_msg_t::make_player_leds(0, player, 0));
          }
        }
      }

    protected:
      std::string current_input() override {
        return {reinterpret_cast<const char *>(report.data()), report.size()};
      }

      std::string feature(std::uint8_t id, std::uint16_t length) override {
        std::uint8_t size = 63;
        for (const auto &[fid, fsize] : FEATURES) {
          if (fid == id) {
            size = fsize;
          }
        }
        std::string r(size + 1, '\0');
        auto *p = reinterpret_cast<std::uint8_t *>(r.data());
        p[0] = id;
        if (id == 0x05) {
          fill_sony_calibration(p);
        } else if (id == 0x09) {
          const std::uint8_t mac[6] {static_cast<std::uint8_t>(0x10 + index), 0x53, 0x4E, 0x55, 0x53, 0x02};
          std::memcpy(p + 1, mac, sizeof(mac));
        } else if (id == 0x20) {
          std::memcpy(p + 1, "Jun 15 2024", 11);
          std::memcpy(p + 12, "12:00:00", 8);
          put_le32(p + 24, 0x00000617);
          put_le32(p + 28, 0x0110002A);
          p[44] = 0x30;
          p[45] = 0x06;
        }
        return r;
      }

    private:
      int index;
      std::array<std::uint8_t, 64> report {};
      std::uint8_t seq = 0;
      sony_touch_t touchpad;
      std::uint16_t last_low = 0, last_high = 0;
      std::array<std::uint8_t, 3> last_rgb {};
      std::uint8_t last_player = 0;
      std::array<std::uint8_t, 22> last_triggers {};
    };

    /**
     * @brief Sony DualShock 4 v2, wired.
     */
    class ds4_t: public hid_profile_t {
      static constexpr std::size_t LX = 1, LY = 2, RX = 3, RY = 4, BTN0 = 5, BTN1 = 6, BTN2 = 7, L2 = 8, R2 = 9, TIMESTAMP = 10;
      static constexpr std::size_t GYRO = 13, ACCEL = 19, STATUS = 30, TOUCH_COUNT = 33, TOUCH_TS = 34, TOUCH0 = 35, TOUCH1 = 39;

    public:
      explicit ds4_t(int index):
          index {index} {
        report.fill(0);
        report[0] = 0x01;
        report[LX] = report[LY] = report[RX] = report[RY] = 0x80;
        report[BTN0] = 0x08;
        report[STATUS] = 0x1B;  // cable connected, full
        report[TOUCH_COUNT] = 1;
        report[TOUCH0] = report[TOUCH1] = 0x80;
        put_le16(&report[ACCEL + 2], SONY_ACC_RES_PER_G);
      }

      std::string_view display_name() const override {
        return "DualShock 4"sv;
      }

      std::uint16_t vid() const override {
        return 0x054C;
      }

      std::uint16_t pid() const override {
        return 0x09CC;
      }

      std::uint16_t bcd_device() const override {
        return 0x0100;
      }

      std::string device_descriptor() const override {
        return make_device_descriptor(0, 0, 0, 64, vid(), pid(), bcd_device(), false);
      }

      std::string config_descriptor() const override {
        return make_hid_config(report_descriptor().size(), 4, 3, 64, 5);
      }

      std::wstring manufacturer() const override {
        return L"Sony Interactive Entertainment";
      }

      std::wstring product() const override {
        return L"Wireless Controller";
      }

      std::string report_descriptor() const override {
        std::vector<std::uint8_t> d {0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01};
        add_axes(d, {0x30, 0x31, 0x32, 0x35});
        add_hat(d);
        add_buttons(d, 14);
        d.insert(d.end(), {0x06, 0x00, 0xFF, 0x09, 0x20, 0x75, 0x06, 0x95, 0x01, 0x81, 0x02});
        add_axes(d, {0x33, 0x34});
        d.insert(d.end(), {0x06, 0x00, 0xFF, 0x09, 0x21, 0x95, 0x36, 0x81, 0x02});
        add_vendor_reports(d, 0x91, {{0x05, 31}}, 0x22);
        add_vendor_reports(d, 0xB1, {{0x02, 36}, {0x04, 36}, {0x08, 3}, {0x10, 4}, {0x11, 2}, {0x12, 15}, {0x13, 22}, {0x14, 16}, {0x15, 44}, {0x80, 6}, {0x81, 6}, {0x82, 5}, {0x83, 1}, {0x84, 4}, {0x85, 6}, {0x86, 6}, {0x87, 35}, {0x88, 34}, {0x89, 2}, {0x90, 5}, {0x91, 3}, {0x92, 3}, {0x93, 12}, {0xA0, 6}, {0xA1, 1}, {0xA2, 1}, {0xA3, 48}, {0xA4, 13}, {0xA5, 21}, {0xA6, 21}, {0xA7, 1}, {0xA8, 1}, {0xA9, 8}, {0xAA, 1}, {0xAB, 57}, {0xAC, 57}, {0xAD, 11}, {0xAE, 1}, {0xAF, 2}, {0xB0, 63}, {0xF0, 63}, {0xF1, 63}, {0xF2, 15}}, 0x23);
        d.push_back(0xC0);
        return to_string(d);
      }

      std::uint8_t in_endpoint() const override {
        return 4;
      }

      std::uint8_t out_endpoint() const override {
        return 3;
      }

      bool has_motion() const override {
        return true;
      }

      bool has_touchpad() const override {
        return true;
      }

      void update(const gamepad_state_t &gp) override {
        report[LX] = stick_u8(gp.lsX, false);
        report[LY] = stick_u8(gp.lsY, true);
        report[RX] = stick_u8(gp.rsX, false);
        report[RY] = stick_u8(gp.rsY, true);
        report[L2] = gp.lt;
        report[R2] = gp.rt;
        const auto f = gp.buttonFlags;
        report[BTN0] = hat_value(f) | ((f & X) ? 0x10 : 0) | ((f & A) ? 0x20 : 0) | ((f & B) ? 0x40 : 0) | ((f & Y) ? 0x80 : 0);
        report[BTN1] = ((f & LEFT_BUTTON) ? 0x01 : 0) | ((f & RIGHT_BUTTON) ? 0x02 : 0) | (gp.lt > 0 ? 0x04 : 0) | (gp.rt > 0 ? 0x08 : 0) |
                       ((f & BACK) ? 0x10 : 0) | ((f & START) ? 0x20 : 0) | ((f & LEFT_STICK) ? 0x40 : 0) | ((f & RIGHT_STICK) ? 0x80 : 0);
        buttons2 = ((f & HOME) ? 0x01 : 0) | ((f & TOUCHPAD_BUTTON) ? 0x02 : 0);
      }

      void touch(const gamepad_touch_t &t) override {
        if (touchpad.apply(t, {&report[TOUCH0], &report[TOUCH1]}, 1920, 943)) {
          report[TOUCH_TS]++;
        }
      }

      void motion(const gamepad_motion_t &m) override {
        apply_sony_motion(&report[GYRO], &report[ACCEL], m);
      }

      void battery(const gamepad_battery_t &battery) override {
        std::uint8_t status = report[STATUS];
        if (battery.state == LI_BATTERY_STATE_CHARGING || battery.state == LI_BATTERY_STATE_FULL) {
          status |= 0x10;
        } else if (battery.state == LI_BATTERY_STATE_DISCHARGING) {
          status &= ~0x10;
        }
        if (battery.state == LI_BATTERY_STATE_FULL) {
          status = 0x1B;
        } else if (battery.percentage != LI_BATTERY_PERCENTAGE_UNKNOWN) {
          status = (status & 0xF0) | static_cast<std::uint8_t>(std::min(10, battery.percentage / 10));
        }
        report[STATUS] = status;
      }

      bool input(std::string &out) override {
        report[BTN2] = buttons2 | static_cast<std::uint8_t>((counter++ & 0x3F) << 2);
        put_le16(&report[TIMESTAMP], static_cast<std::int16_t>(static_cast<std::uint16_t>(now_us() * 3 / 16)));
        out.assign(reinterpret_cast<const char *>(report.data()), report.size());
        return true;
      }

      void output(const std::uint8_t *d, std::size_t n) override {
        if (n < 11 || d[0] != 0x05) {
          return;
        }
        if (d[1] & 0x01) {
          const std::uint16_t low = d[5] * 257;
          const std::uint16_t high = d[4] * 257;
          if (low != last_low || high != last_high) {
            last_low = low;
            last_high = high;
            emit(gamepad_feedback_msg_t::make_rumble(0, low, high));
          }
        }
        if (d[1] & 0x02) {
          const std::array<std::uint8_t, 3> rgb {d[6], d[7], d[8]};
          if (rgb != last_rgb) {
            last_rgb = rgb;
            emit(gamepad_feedback_msg_t::make_rgb_led(0, rgb[0], rgb[1], rgb[2]));
          }
        }
      }

    protected:
      std::string current_input() override {
        return {reinterpret_cast<const char *>(report.data()), report.size()};
      }

      std::string feature(std::uint8_t id, std::uint16_t length) override {
        std::string r(std::max<std::uint16_t>(length, 2), '\0');
        auto *p = reinterpret_cast<std::uint8_t *>(r.data());
        p[0] = id;
        if (id == 0x02 && r.size() >= 37) {
          fill_sony_calibration(p);
        } else if ((id == 0x12 || id == 0x81) && r.size() >= 7) {
          const std::uint8_t mac[6] {static_cast<std::uint8_t>(0x20 + index), 0x53, 0x4E, 0x55, 0x53, 0x02};
          std::memcpy(p + 1, mac, sizeof(mac));
        } else if (id == 0xA3 && r.size() >= 49) {
          std::memcpy(p + 1, "Sep 21 2018", 11);
          std::memcpy(p + 17, "04:50:51", 8);
          p[33] = 0x01;
          p[35] = 0x03;
          p[41] = 0xB4;
          p[42] = 0x01;
        }
        return r;
      }

    private:
      int index;
      std::array<std::uint8_t, 64> report {};
      std::uint8_t buttons2 = 0;
      std::uint8_t counter = 0;
      sony_touch_t touchpad;
      std::uint16_t last_low = 0, last_high = 0;
      std::array<std::uint8_t, 3> last_rgb {};
    };

    // ------------------------------------------------------------------------------------------
    // Microsoft Xbox 360 wired controller (XUSB). Handled by the Windows xusb22 driver (XInput).
    // ------------------------------------------------------------------------------------------

    class x360_t: public profile_t {
    public:
      x360_t() {
        report.fill(0);
        report[1] = 0x14;
      }

      std::string_view display_name() const override {
        return "Xbox 360 Controller"sv;
      }

      std::uint16_t vid() const override {
        return 0x045E;
      }

      std::uint16_t pid() const override {
        return 0x028E;
      }

      std::uint16_t bcd_device() const override {
        return 0x0114;
      }

      std::uint8_t interface_class() const override {
        return 0xFF;
      }

      std::string device_descriptor() const override {
        // Device class 0 lets Windows match xusb22 on the interface class FF/5D/01.
        return make_device_descriptor(0x00, 0x00, 0x00, 0x08, vid(), pid(), bcd_device(), true);
      }

      std::string config_descriptor() const override {
        return bytes({
          0x09, 0x02, 0x31, 0x00, 0x01, 0x01, 0x00, 0xA0, 0xFA,  // 49 bytes, 1 interface, 500 mA
          0x09, 0x04, 0x00, 0x00, 0x02, 0xFF, 0x5D, 0x01, 0x00,  // XUSB gamepad interface
          0x11, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, 0x01, 0x08, 0x00, 0x00,  // XUSB class descriptor
          0x07, 0x05, 0x81, 0x03, 0x20, 0x00, 0x04,  // interrupt IN
          0x07, 0x05, 0x01, 0x03, 0x20, 0x00, 0x08,  // interrupt OUT
        });
      }

      std::wstring manufacturer() const override {
        return L"©Microsoft Corporation";
      }

      std::wstring product() const override {
        return L"Controller";
      }

      std::wstring serial() const override {
        return L"08FEC93";
      }

      std::uint8_t in_endpoint() const override {
        return 1;
      }

      std::uint8_t out_endpoint() const override {
        return 1;
      }

      std::int32_t control(const std::uint8_t *setup, const std::vector<std::uint8_t> &out, std::string &in) override {
        // The XUSB driver issues a few vendor requests during start up; reply with zeros.
        const auto length = static_cast<std::uint16_t>(setup[6] | (setup[7] << 8));
        if (setup[0] & 0x80) {
          in = std::string(length, '\0');
        }
        return 0;
      }

      void update(const gamepad_state_t &gp) override {
        const auto f = gp.buttonFlags;
        report[2] = ((f & DPAD_UP) ? 0x01 : 0) | ((f & DPAD_DOWN) ? 0x02 : 0) | ((f & DPAD_LEFT) ? 0x04 : 0) | ((f & DPAD_RIGHT) ? 0x08 : 0) |
                    ((f & START) ? 0x10 : 0) | ((f & BACK) ? 0x20 : 0) | ((f & LEFT_STICK) ? 0x40 : 0) | ((f & RIGHT_STICK) ? 0x80 : 0);
        report[3] = ((f & LEFT_BUTTON) ? 0x01 : 0) | ((f & RIGHT_BUTTON) ? 0x02 : 0) | ((f & HOME) ? 0x04 : 0) |
                    ((f & A) ? 0x10 : 0) | ((f & B) ? 0x20 : 0) | ((f & X) ? 0x40 : 0) | ((f & Y) ? 0x80 : 0);
        report[4] = gp.lt;
        report[5] = gp.rt;
        put_le16(&report[6], gp.lsX);
        put_le16(&report[8], gp.lsY);
        put_le16(&report[10], gp.rsX);
        put_le16(&report[12], gp.rsY);
      }

      bool input(std::string &out) override {
        out.assign(reinterpret_cast<const char *>(report.data()), report.size());
        return true;
      }

      void output(const std::uint8_t *d, std::size_t n) override {
        if (n >= 5 && d[0] == 0x00 && d[1] == 0x08) {
          const std::uint16_t low = d[3] * 257;
          const std::uint16_t high = d[4] * 257;
          if (low != last_low || high != last_high) {
            last_low = low;
            last_high = high;
            emit(gamepad_feedback_msg_t::make_rumble(0, low, high));
          }
        } else if (n >= 3 && d[0] == 0x01 && d[1] == 0x03) {
          // LED patterns 0x02..0x05 flash then light player 1..4, 0x06..0x09 light player 1..4.
          const auto pattern = d[2];
          std::uint8_t solid = 0;
          if (pattern >= 0x06 && pattern <= 0x09) {
            solid = 1 << (pattern - 0x06);
          } else if (pattern >= 0x02 && pattern <= 0x05) {
            solid = 1 << (pattern - 0x02);
          }
          if (solid && solid != last_player) {
            last_player = solid;
            emit(gamepad_feedback_msg_t::make_player_leds(0, solid, 0));
          }
        }
      }

    private:
      std::array<std::uint8_t, 20> report {};
      std::uint16_t last_low = 0, last_high = 0;
      std::uint8_t last_player = 0;
    };

    // ------------------------------------------------------------------------------------------
    // Nintendo Switch Pro Controller over USB.
    // ------------------------------------------------------------------------------------------

    constexpr std::uint16_t SWITCH_STICK_CENTER = 2048;
    constexpr std::uint16_t SWITCH_STICK_RANGE = 1700;
    constexpr float SWITCH_ACCEL_PER_G = 4096.0f;  // with sensitivity coefficient 0x4000 and +-8G
    constexpr float SWITCH_GYRO_PER_DPS = 13371.0f / 936.0f;  // with coefficient 0x343B and +-2000 dps

    class switch_pro_t: public hid_profile_t {
    public:
      explicit switch_pro_t(int index):
          mac {0x98, 0xB6, 0xE9, 0x53, 0x4E, static_cast<std::uint8_t>(0x30 + index)} {
        update(gamepad_state_t {});
        accel = {0, 0, static_cast<std::int16_t>(SWITCH_ACCEL_PER_G)};
      }

      std::string_view display_name() const override {
        return "Switch Pro Controller"sv;
      }

      std::uint16_t vid() const override {
        return 0x057E;
      }

      std::uint16_t pid() const override {
        return 0x2009;
      }

      std::uint16_t bcd_device() const override {
        return 0x0200;
      }

      std::string device_descriptor() const override {
        return make_device_descriptor(0, 0, 0, 64, vid(), pid(), bcd_device(), true);
      }

      std::string config_descriptor() const override {
        return make_hid_config(report_descriptor().size(), 1, 2, 64, 8);
      }

      std::wstring manufacturer() const override {
        return L"Nintendo Co., Ltd.";
      }

      std::wstring product() const override {
        return L"Pro Controller";
      }

      std::wstring serial() const override {
        return L"000000000001";
      }

      std::string report_descriptor() const override {
        std::vector<std::uint8_t> d {0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xA1, 0x01};
        // Simple HID report 0x3F (buttons, hat, sticks) for DirectInput users.
        d.insert(d.end(), {0x85, 0x3F});
        add_buttons(d, 16);
        add_hat(d);
        add_padding_bits(d, 4);
        d.insert(d.end(), {0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02});
        add_vendor_reports(d, 0x81, {{0x21, 63}, {0x30, 63}, {0x31, 63}, {0x32, 63}, {0x33, 63}, {0x81, 63}}, 0x01);
        add_vendor_reports(d, 0x91, {{0x01, 63}, {0x10, 63}, {0x11, 63}, {0x12, 63}, {0x80, 63}, {0x82, 63}}, 0x10);
        d.push_back(0xC0);
        return to_string(d);
      }

      std::uint8_t in_endpoint() const override {
        return 1;
      }

      std::uint8_t out_endpoint() const override {
        return 2;
      }

      bool has_motion() const override {
        return true;
      }

      void update(const gamepad_state_t &gp) override {
        const auto f = gp.buttonFlags;
        // Moonlight buttons are positional (A = bottom), Nintendo's bottom button is B.
        buttons[0] = ((f & X) ? 0x01 : 0) | ((f & Y) ? 0x02 : 0) | ((f & A) ? 0x04 : 0) | ((f & B) ? 0x08 : 0) |
                     ((f & RIGHT_BUTTON) ? 0x40 : 0) | (gp.rt > 64 ? 0x80 : 0);
        buttons[1] = ((f & BACK) ? 0x01 : 0) | ((f & START) ? 0x02 : 0) | ((f & RIGHT_STICK) ? 0x04 : 0) | ((f & LEFT_STICK) ? 0x08 : 0) |
                     ((f & HOME) ? 0x10 : 0) | ((f & MISC_BUTTON) ? 0x20 : 0);
        buttons[2] = ((f & DPAD_DOWN) ? 0x01 : 0) | ((f & DPAD_UP) ? 0x02 : 0) | ((f & DPAD_RIGHT) ? 0x04 : 0) | ((f & DPAD_LEFT) ? 0x08 : 0) |
                     ((f & LEFT_BUTTON) ? 0x40 : 0) | (gp.lt > 64 ? 0x80 : 0);
        const auto axis = [](std::int16_t v) {
          return static_cast<std::uint16_t>(SWITCH_STICK_CENTER + (static_cast<std::int32_t>(v) * SWITCH_STICK_RANGE) / 32768);
        };
        pack12(left_stick.data(), axis(gp.lsX), axis(gp.lsY));
        pack12(right_stick.data(), axis(gp.rsX), axis(gp.rsY));
        simple = gp;
      }

      void motion(const gamepad_motion_t &m) override {
        // Moonlight uses SDL's controller frame; SDL maps Switch raw (x, y, z) to (-y, z, -x).
        auto &dst = m.motionType == LI_MOTION_TYPE_GYRO ? gyro : accel;
        const float scale = m.motionType == LI_MOTION_TYPE_GYRO ? SWITCH_GYRO_PER_DPS : SWITCH_ACCEL_PER_G / EARTH_G;
        const auto conv = [scale](float v) {
          return static_cast<std::int16_t>(std::clamp(v * scale, -32768.0f, 32767.0f));
        };
        if (m.motionType != LI_MOTION_TYPE_GYRO && m.motionType != LI_MOTION_TYPE_ACCEL) {
          return;
        }
        dst = {conv(-m.z), conv(-m.x), conv(m.y)};
      }

      void battery(const gamepad_battery_t &battery) override {
        if (battery.percentage != LI_BATTERY_PERCENTAGE_UNKNOWN) {
          battery_level = static_cast<std::uint8_t>(std::clamp(battery.percentage / 25, 0, 4) * 2);
        }
        charging = battery.state == LI_BATTERY_STATE_CHARGING;
      }

      bool input(std::string &out) override {
        if (!replies.empty()) {
          out = std::move(replies.front());
          replies.pop_front();
          return true;
        }
        if (mode == 0x30 && streaming) {
          out = std::string(64, '\0');
          auto *p = reinterpret_cast<std::uint8_t *>(out.data());
          p[0] = 0x30;
          fill_common(p);
          for (int i = 0; i < 3; ++i) {
            auto *s = p + 13 + i * 12;
            for (int a = 0; a < 3; ++a) {
              put_le16(s + a * 2, accel[a]);
              put_le16(s + 6 + a * 2, gyro[a]);
            }
          }
          return true;
        }
        if (mode == 0x3F && streaming) {
          out = std::string(12, '\0');
          auto *p = reinterpret_cast<std::uint8_t *>(out.data());
          const auto f = simple.buttonFlags;
          p[0] = 0x3F;
          p[1] = ((f & A) ? 0x01 : 0) | ((f & B) ? 0x02 : 0) | ((f & X) ? 0x04 : 0) | ((f & Y) ? 0x08 : 0) |
                 ((f & LEFT_BUTTON) ? 0x10 : 0) | ((f & RIGHT_BUTTON) ? 0x20 : 0) | (simple.lt > 64 ? 0x40 : 0) | (simple.rt > 64 ? 0x80 : 0);
          p[2] = ((f & BACK) ? 0x01 : 0) | ((f & START) ? 0x02 : 0) | ((f & LEFT_STICK) ? 0x04 : 0) | ((f & RIGHT_STICK) ? 0x08 : 0) |
                 ((f & HOME) ? 0x10 : 0) | ((f & MISC_BUTTON) ? 0x20 : 0);
          p[3] = hat_value(f);
          const auto u16 = [](std::int16_t v, bool invert) {
            const auto u = static_cast<std::uint16_t>(static_cast<std::int32_t>(v) + 32768);
            return invert ? static_cast<std::uint16_t>(65535 - u) : u;
          };
          put_le16(p + 4, static_cast<std::int16_t>(u16(simple.lsX, false)));
          put_le16(p + 6, static_cast<std::int16_t>(u16(simple.lsY, true)));
          put_le16(p + 8, static_cast<std::int16_t>(u16(simple.rsX, false)));
          put_le16(p + 10, static_cast<std::int16_t>(u16(simple.rsY, true)));
          return true;
        }
        return false;
      }

      void output(const std::uint8_t *d, std::size_t n) override {
        if (n < 2) {
          return;
        }
        if (d[0] == 0x80) {
          usb_command(d[1]);
        } else if (d[0] == 0x01 && n >= 11) {
          rumble(d + 2);
          subcommand(d[10], d + 11, n - 11);
        } else if (d[0] == 0x10 && n >= 10) {
          rumble(d + 2);
        }
      }

    protected:
      std::string current_input() override {
        std::string out(64, '\0');
        auto *p = reinterpret_cast<std::uint8_t *>(out.data());
        p[0] = 0x30;
        fill_common(p);
        return out;
      }

    private:
      void fill_common(std::uint8_t *p) {
        p[1] = timer++;
        p[2] = static_cast<std::uint8_t>((battery_level << 4) | (charging ? 0x10 : 0) | 0x01);
        p[3] = buttons[0];
        p[4] = buttons[1];
        p[5] = buttons[2];
        std::memcpy(p + 6, left_stick.data(), 3);
        std::memcpy(p + 9, right_stick.data(), 3);
        p[12] = 0x09;
      }

      void usb_command(std::uint8_t cmd) {
        std::string r(64, '\0');
        auto *p = reinterpret_cast<std::uint8_t *>(r.data());
        p[0] = 0x81;
        p[1] = cmd;
        switch (cmd) {
          case 0x01:  // status: controller type and MAC
            p[3] = 0x03;
            for (int i = 0; i < 6; ++i) {
              p[4 + i] = mac[5 - i];
            }
            replies.push_back(std::move(r));
            break;
          case 0x02:  // handshake
          case 0x03:  // baud rate
            replies.push_back(std::move(r));
            break;
          case 0x04:  // USB only, start sending reports
            streaming = true;
            break;
          case 0x05:  // allow Bluetooth again
            streaming = false;
            break;
          default:
            break;
        }
      }

      void reply(std::uint8_t ack, std::uint8_t sub, std::initializer_list<std::uint8_t> data) {
        std::vector<std::uint8_t> v(data);
        reply(ack, sub, v.data(), v.size());
      }

      void reply(std::uint8_t ack, std::uint8_t sub, const std::uint8_t *data, std::size_t size) {
        std::string r(64, '\0');
        auto *p = reinterpret_cast<std::uint8_t *>(r.data());
        p[0] = 0x21;
        fill_common(p);
        p[13] = ack;
        p[14] = sub;
        std::memcpy(p + 15, data, std::min<std::size_t>(size, 64 - 15));
        replies.push_back(std::move(r));
      }

      std::uint8_t spi(std::uint32_t addr) const {
        const auto in = [addr](std::uint32_t base, std::size_t size) {
          return addr >= base && addr < base + size;
        };
        if (in(0x6020, 24)) {  // factory IMU calibration
          const std::uint8_t cal[24] {0, 0, 0, 0, 0, 0, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0, 0, 0, 0, 0, 0, 0x3B, 0x34, 0x3B, 0x34, 0x3B, 0x34};
          return cal[addr - 0x6020];
        }
        if (in(0x603D, 18)) {  // factory stick calibration
          std::uint8_t cal[18];
          pack12(cal + 0, SWITCH_STICK_RANGE, SWITCH_STICK_RANGE);  // left: max above center
          pack12(cal + 3, SWITCH_STICK_CENTER, SWITCH_STICK_CENTER);  // left: center
          pack12(cal + 6, SWITCH_STICK_RANGE, SWITCH_STICK_RANGE);  // left: min below center
          pack12(cal + 9, SWITCH_STICK_CENTER, SWITCH_STICK_CENTER);  // right: center
          pack12(cal + 12, SWITCH_STICK_RANGE, SWITCH_STICK_RANGE);  // right: min below center
          pack12(cal + 15, SWITCH_STICK_RANGE, SWITCH_STICK_RANGE);  // right: max above center
          return cal[addr - 0x603D];
        }
        if (in(0x6050, 13)) {  // colors: body, buttons, left grip, right grip
          const std::uint8_t colors[13] {0x32, 0x32, 0x32, 0xFF, 0xFF, 0xFF, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x01};
          return colors[addr - 0x6050];
        }
        if (in(0x6080, 24)) {  // sensor and stick parameters
          const std::uint8_t params[24] {0x50, 0xFD, 0x00, 0x00, 0xC6, 0x0F, 0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14, 0x54, 0x41, 0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63};
          return params[addr - 0x6080];
        }
        if (in(0x6098, 18)) {  // right stick parameters
          const std::uint8_t params[18] {0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14, 0x54, 0x41, 0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63};
          return params[addr - 0x6098];
        }
        return 0xFF;  // unset, including user calibration at 0x8010
      }

      void subcommand(std::uint8_t sub, const std::uint8_t *args, std::size_t size) {
        const auto arg = [&](std::size_t i) -> std::uint8_t {
          return i < size ? args[i] : 0;
        };
        switch (sub) {
          case 0x01:  // manual Bluetooth pairing
            reply(0x81, sub, {0x03});
            break;
          case 0x02: {  // device info
            std::uint8_t info[12] {0x03, 0x8B, 0x03, 0x02};
            std::memcpy(info + 4, mac.data(), 6);
            info[10] = 0x01;
            info[11] = 0x02;
            reply(0x82, sub, info, sizeof(info));
            break;
          }
          case 0x03:  // set input report mode
            mode = arg(0);
            streaming = true;
            reply(0x80, sub, {});
            break;
          case 0x04:  // trigger buttons elapsed time
            reply(0x83, sub, {});
            break;
          case 0x10: {  // SPI flash read
            const std::uint32_t addr = arg(0) | (arg(1) << 8) | (arg(2) << 16) | (static_cast<std::uint32_t>(arg(3)) << 24);
            const auto len = std::min<std::uint8_t>(arg(4), 0x1D);
            std::vector<std::uint8_t> data {arg(0), arg(1), arg(2), arg(3), len};
            for (std::uint8_t i = 0; i < len; ++i) {
              data.push_back(spi(addr + i));
            }
            reply(0x90, sub, data.data(), data.size());
            break;
          }
          case 0x21:  // MCU configuration
            reply(0xA0, sub, {0x01, 0x00, 0xFF, 0x00, 0x03, 0x00, 0x05, 0x01});
            break;
          case 0x30: {  // player lights
            const auto solid = arg(0) & 0x0F;
            const auto flashing = (arg(0) >> 4) & 0x0F;
            if (emit && (solid != last_solid || flashing != last_flashing)) {
              last_solid = solid;
              last_flashing = flashing;
              emit(gamepad_feedback_msg_t::make_player_leds(0, solid, flashing));
            }
            reply(0x80, sub, {});
            break;
          }
          default:  // home light, IMU enable/sensitivity, vibration enable, shipment mode, ...
            reply(0x80, sub, {});
            break;
        }
      }

      /**
       * @brief Approximate the amplitude of one side of an HD rumble packet.
       * @return Intensity 0..65535.
       */
      static std::uint16_t rumble_amplitude(const std::uint8_t *side) {
        const float high = static_cast<float>(side[1] & 0xFE) / 0xC8;
        const float low = static_cast<float>(std::max(0, (side[3] & 0x7F) - 0x40)) / (0x72 - 0x40);
        return static_cast<std::uint16_t>(std::clamp(std::max(high, low), 0.0f, 1.0f) * 65535.0f);
      }

      void rumble(const std::uint8_t *data) {
        const auto low = rumble_amplitude(data);
        const auto high = rumble_amplitude(data + 4);
        if (emit && (low != last_low || high != last_high)) {
          last_low = low;
          last_high = high;
          emit(gamepad_feedback_msg_t::make_rumble(0, low, high));
        }
      }

      std::array<std::uint8_t, 6> mac;
      std::array<std::uint8_t, 3> buttons {};
      std::array<std::uint8_t, 3> left_stick {};
      std::array<std::uint8_t, 3> right_stick {};
      gamepad_state_t simple {};
      std::array<std::int16_t, 3> accel {};
      std::array<std::int16_t, 3> gyro {};
      std::deque<std::string> replies;
      std::uint8_t mode = 0x30;
      bool streaming = false;
      std::uint8_t timer = 0;
      std::uint8_t battery_level = 8;
      bool charging = true;
      std::uint16_t last_low = 0, last_high = 0;
      int last_solid = -1, last_flashing = -1;
    };

    // ------------------------------------------------------------------------------------------
    // Generic DirectInput gamepad (pid.codes test VID/PID), comparable to 8BitDo's D-input mode.
    // ------------------------------------------------------------------------------------------

    class generic_t: public hid_profile_t {
    public:
      generic_t() {
        report.fill(0);
        report[2] = 0x0F;  // hat neutral (null state)
        report[3] = report[4] = report[5] = report[6] = 0x80;
      }

      std::string_view display_name() const override {
        return "Generic HID Gamepad"sv;
      }

      std::uint16_t vid() const override {
        return 0x1209;
      }

      std::uint16_t pid() const override {
        return 0x0001;
      }

      std::uint16_t bcd_device() const override {
        return 0x0100;
      }

      std::string device_descriptor() const override {
        return make_device_descriptor(0, 0, 0, 64, vid(), pid(), bcd_device(), false);
      }

      std::string config_descriptor() const override {
        return make_hid_config(report_descriptor().size(), 1, 0, 16, 4);
      }

      std::wstring manufacturer() const override {
        return L"Sunshine";
      }

      std::wstring product() const override {
        return L"Sunshine Gamepad";
      }

      std::string report_descriptor() const override {
        std::vector<std::uint8_t> d {0x05, 0x01, 0x09, 0x05, 0xA1, 0x01};
        add_buttons(d, 16);
        add_hat(d);
        add_padding_bits(d, 4);
        add_axes(d, {0x30, 0x31, 0x32, 0x35, 0x33, 0x34});
        d.push_back(0xC0);
        return to_string(d);
      }

      std::uint8_t in_endpoint() const override {
        return 1;
      }

      std::uint8_t out_endpoint() const override {
        return 0;
      }

      void update(const gamepad_state_t &gp) override {
        const auto f = gp.buttonFlags;
        // Buttons follow the common DirectInput order: 1 A, 2 B, 3 X, 4 Y, 5 LB, 6 RB, 7 LT, 8 RT,
        // 9 Back, 10 Start, 11 LS, 12 RS, 13 Home, 14 Misc, 15-16 paddles.
        std::uint16_t b = 0;
        b |= (f & A) ? 0x0001 : 0;
        b |= (f & B) ? 0x0002 : 0;
        b |= (f & X) ? 0x0004 : 0;
        b |= (f & Y) ? 0x0008 : 0;
        b |= (f & LEFT_BUTTON) ? 0x0010 : 0;
        b |= (f & RIGHT_BUTTON) ? 0x0020 : 0;
        b |= gp.lt > 64 ? 0x0040 : 0;
        b |= gp.rt > 64 ? 0x0080 : 0;
        b |= (f & BACK) ? 0x0100 : 0;
        b |= (f & START) ? 0x0200 : 0;
        b |= (f & LEFT_STICK) ? 0x0400 : 0;
        b |= (f & RIGHT_STICK) ? 0x0800 : 0;
        b |= (f & HOME) ? 0x1000 : 0;
        b |= (f & MISC_BUTTON) ? 0x2000 : 0;
        b |= (f & PADDLE1) ? 0x4000 : 0;
        b |= (f & PADDLE2) ? 0x8000 : 0;
        report[0] = b & 0xFF;
        report[1] = b >> 8;
        const auto hat = hat_value(f);
        report[2] = hat == 8 ? 0x0F : hat;
        report[3] = stick_u8(gp.lsX, false);
        report[4] = stick_u8(gp.lsY, true);
        report[5] = stick_u8(gp.rsX, false);
        report[6] = stick_u8(gp.rsY, true);
        report[7] = gp.lt;
        report[8] = gp.rt;
      }

      bool input(std::string &out) override {
        out.assign(reinterpret_cast<const char *>(report.data()), report.size());
        return true;
      }

      void output(const std::uint8_t *, std::size_t) override {}

    protected:
      std::string current_input() override {
        return {reinterpret_cast<const char *>(report.data()), report.size()};
      }

    private:
      std::array<std::uint8_t, 9> report {};
    };
  }  // namespace

  void pack12(std::uint8_t *out, std::uint16_t a, std::uint16_t b) {
    out[0] = a & 0xFF;
    out[1] = static_cast<std::uint8_t>(((a >> 8) & 0x0F) | ((b & 0x0F) << 4));
    out[2] = static_cast<std::uint8_t>((b >> 4) & 0xFF);
  }

  std::uint8_t stick_u8(std::int16_t v, bool invert) {
    const auto u = static_cast<std::uint8_t>((static_cast<std::int32_t>(v) + 32768) >> 8);
    return invert ? static_cast<std::uint8_t>(255 - u) : u;
  }

  std::optional<profile_e> profile_from_name(std::string_view name) {
    if (name == "ds5"sv) {
      return profile_e::ds5;
    }
    if (name == "ds4"sv) {
      return profile_e::ds4;
    }
    if (name == "x360"sv || name == "xone"sv || name == "xseries"sv) {
      return profile_e::x360;
    }
    if (name == "switch"sv) {
      return profile_e::switch_pro;
    }
    if (name == "generic"sv) {
      return profile_e::generic;
    }
    return std::nullopt;
  }

  std::string_view profile_name(profile_e profile) {
    switch (profile) {
      case profile_e::ds5:
        return "ds5"sv;
      case profile_e::ds4:
        return "ds4"sv;
      case profile_e::x360:
        return "x360"sv;
      case profile_e::switch_pro:
        return "switch"sv;
      case profile_e::generic:
        return "generic"sv;
    }
    return "ds5"sv;
  }

  profile_e select_profile(std::string_view configured, const gamepad_arrival_t &arrival) {
    if (const auto fixed = profile_from_name(configured)) {
      return *fixed;
    }
    switch (arrival.type) {
      case LI_CTYPE_PS:
        return profile_e::ds5;
      case LI_CTYPE_XBOX:
        return profile_e::x360;
      case LI_CTYPE_NINTENDO:
        return profile_e::switch_pro;
      default:
        break;
    }
    if (arrival.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO | LI_CCAP_TOUCHPAD)) {
      return profile_e::ds5;
    }
    return profile_e::x360;
  }

  std::unique_ptr<profile_t> make_profile(profile_e profile, int index) {
    switch (profile) {
      case profile_e::ds5:
        return std::make_unique<ds5_t>(index);
      case profile_e::ds4:
        return std::make_unique<ds4_t>(index);
      case profile_e::x360:
        return std::make_unique<x360_t>();
      case profile_e::switch_pro:
        return std::make_unique<switch_pro_t>(index);
      case profile_e::generic:
        return std::make_unique<generic_t>();
    }
    return nullptr;
  }
}  // namespace platf::usbip_gamepad
