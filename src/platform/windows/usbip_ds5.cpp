/**
 * @file src/platform/windows/usbip_ds5.cpp
 * @brief Free USB/IP based virtual DualSense backend for Windows.
 *
 * Sunshine acts as a tiny USB/IP server on 127.0.0.1:3240 and exports one
 * emulated wired DualSense per gamepad. usbip-win2 (BSD-2-Clause,
 * https://github.com/vadimgrn/usbip-win2) is used as the client driver that
 * plugs the device into Windows.
 *
 * Report layouts follow the Linux hid-playstation driver.
 */
// platform includes
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// local includes
#include "usbip_ds5.h"
#include "src/logging.h"

using namespace std::literals;

namespace platf::usbip_ds5 {
  namespace {
    constexpr std::uint16_t USBIP_PORT = 3240;
    constexpr std::uint16_t USBIP_VERSION = 0x0111;
    constexpr std::uint16_t OP_REQ_DEVLIST = 0x8005;
    constexpr std::uint16_t OP_REP_DEVLIST = 0x0005;
    constexpr std::uint16_t OP_REQ_IMPORT = 0x8003;
    constexpr std::uint16_t OP_REP_IMPORT = 0x0003;
    constexpr std::uint32_t USBIP_CMD_SUBMIT = 1;
    constexpr std::uint32_t USBIP_CMD_UNLINK = 2;
    constexpr std::uint32_t USBIP_RET_SUBMIT = 3;
    constexpr std::uint32_t USBIP_RET_UNLINK = 4;
    constexpr std::uint32_t USBIP_DIR_OUT = 0;
    constexpr std::uint32_t USBIP_DIR_IN = 1;
    constexpr std::uint32_t USB_SPEED_HIGH = 3;
    constexpr std::int32_t ERR_PIPE = -32;  ///< -EPIPE, endpoint stall.
    constexpr std::int32_t ERR_CONNRESET = -104;  ///< -ECONNRESET, URB unlinked.

    constexpr std::uint16_t DS5_VID = 0x054C;
    constexpr std::uint16_t DS5_PID = 0x0CE6;
    constexpr std::uint8_t EP_IN = 4;  ///< Interrupt IN endpoint 0x84.
    constexpr std::uint8_t EP_OUT = 3;  ///< Interrupt OUT endpoint 0x03.
    constexpr std::size_t INPUT_REPORT_SIZE = 64;
    constexpr std::size_t OUTPUT_REPORT_SIZE = 48;

    constexpr float EARTH_G = 9.80665f;
    constexpr int DS5_ACC_RES_PER_G = 8192;
    constexpr int DS5_GYRO_RAW_PER_DEG_S = 16;  ///< Matches the calibration report below.
    constexpr int DS5_GYRO_CAL_PLUS = 8640;
    constexpr int DS5_GYRO_CAL_SPEED = 540;

    // Offsets in the USB input report, including the report ID byte.
    constexpr std::size_t IN_LX = 1, IN_LY = 2, IN_RX = 3, IN_RY = 4, IN_L2 = 5, IN_R2 = 6, IN_SEQ = 7;
    constexpr std::size_t IN_BTN0 = 8, IN_BTN1 = 9, IN_BTN2 = 10;
    constexpr std::size_t IN_GYRO = 16, IN_ACCEL = 22, IN_TIMESTAMP = 28;
    constexpr std::size_t IN_TOUCH0 = 33, IN_TOUCH1 = 37;
    constexpr std::size_t IN_STATUS = 53;

    /// Feature report IDs and their sizes without the report ID byte.
    constexpr std::array<std::pair<std::uint8_t, std::uint8_t>, 18> FEATURE_REPORTS {{
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

    void put_be16(std::uint8_t *p, std::uint16_t v) {
      p[0] = v >> 8;
      p[1] = v & 0xFF;
    }

    void put_be32(std::uint8_t *p, std::uint32_t v) {
      p[0] = v >> 24;
      p[1] = (v >> 16) & 0xFF;
      p[2] = (v >> 8) & 0xFF;
      p[3] = v & 0xFF;
    }

    std::uint32_t get_be32(const std::uint8_t *p) {
      return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
    }

    std::uint16_t get_be16(const std::uint8_t *p) {
      return std::uint16_t((p[0] << 8) | p[1]);
    }

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

    std::string build_hid_report_descriptor() {
      std::vector<std::uint8_t> d {
        0x05, 0x01,  // Usage Page (Generic Desktop)
        0x09, 0x05,  // Usage (Game Pad)
        0xA1, 0x01,  // Collection (Application)
        0x85, 0x01,  // Report ID (1)
        0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x09, 0x33, 0x09, 0x34,  // X Y Z Rz Rx Ry
        0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x02,  // 6 x 8 bit axes
        0x06, 0x00, 0xFF, 0x09, 0x20, 0x95, 0x01, 0x81, 0x02,  // sequence counter
        0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,  // hat switch
        0x65, 0x00, 0x45, 0x00,  // reset unit and physical max
        0x05, 0x09, 0x19, 0x01, 0x29, 0x0F, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0F, 0x81, 0x02,  // 15 buttons
        0x06, 0x00, 0xFF, 0x09, 0x21, 0x95, 0x0D, 0x81, 0x02,  // 13 vendor bits
        0x06, 0x00, 0xFF, 0x09, 0x22, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x34, 0x81, 0x02,  // 52 vendor bytes
        0x85, 0x02, 0x09, 0x23, 0x95, 0x2F, 0x91, 0x02,  // output report 2, 47 bytes
      };
      std::uint8_t usage = 0x24;
      for (const auto &[id, size] : FEATURE_REPORTS) {
        d.insert(d.end(), {0x85, id, 0x09, usage++, 0x95, size, 0xB1, 0x02});
      }
      d.push_back(0xC0);  // End Collection
      return {d.begin(), d.end()};
    }

    std::string usb_string(std::wstring_view text) {
      std::string out;
      out.push_back(static_cast<char>(2 + text.size() * 2));
      out.push_back(0x03);
      for (const auto c : text) {
        out.push_back(static_cast<char>(c & 0xFF));
        out.push_back(static_cast<char>(c >> 8));
      }
      return out;
    }

    std::string device_descriptor() {
      const std::uint8_t d[] {
        0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, DS5_VID & 0xFF, DS5_VID >> 8, DS5_PID & 0xFF, DS5_PID >> 8, 0x00, 0x01, 0x01, 0x02, 0x00, 0x01
      };
      return {reinterpret_cast<const char *>(d), sizeof(d)};
    }

    std::string config_descriptor() {
      const auto report_len = hid_report_descriptor().size();
      const std::uint8_t d[] {
        0x09, 0x02, 0x29, 0x00, 0x01, 0x01, 0x00, 0xC0, 0xFA,  // configuration
        0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,  // HID interface
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, static_cast<std::uint8_t>(report_len & 0xFF), static_cast<std::uint8_t>(report_len >> 8),
        0x07, 0x05, 0x80 | EP_IN, 0x03, 0x40, 0x00, 0x04,  // interrupt IN, 64 bytes, 1 ms
        0x07, 0x05, EP_OUT, 0x03, 0x40, 0x00, 0x04,  // interrupt OUT, 64 bytes, 1 ms
      };
      return {reinterpret_cast<const char *>(d), sizeof(d)};
    }

    std::string device_qualifier() {
      const std::uint8_t d[] {0x0A, 0x06, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x01, 0x00};
      return {reinterpret_cast<const char *>(d), sizeof(d)};
    }

    bool recv_all(SOCKET s, std::uint8_t *buf, std::size_t len) {
      while (len > 0) {
        const auto n = ::recv(s, reinterpret_cast<char *>(buf), static_cast<int>(std::min<std::size_t>(len, INT32_MAX)), 0);
        if (n <= 0) {
          return false;
        }
        buf += n;
        len -= n;
      }
      return true;
    }

    bool send_all(SOCKET s, const std::uint8_t *buf, std::size_t len) {
      while (len > 0) {
        const auto n = ::send(s, reinterpret_cast<const char *>(buf), static_cast<int>(len), 0);
        if (n <= 0) {
          return false;
        }
        buf += n;
        len -= n;
      }
      return true;
    }

    /**
     * @brief Run a process hidden and capture its output.
     */
    int run_process(const std::wstring &cmdline, std::string &output, DWORD timeout_ms) {
      SECURITY_ATTRIBUTES sa {sizeof(sa), nullptr, TRUE};
      HANDLE read_pipe = nullptr;
      HANDLE write_pipe = nullptr;
      if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        return -1;
      }
      SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

      STARTUPINFOW si {};
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESTDHANDLES;
      si.hStdOutput = write_pipe;
      si.hStdError = write_pipe;
      si.hStdInput = nullptr;
      PROCESS_INFORMATION pi {};
      std::wstring mutable_cmd = cmdline;
      const auto ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
      CloseHandle(write_pipe);
      if (!ok) {
        CloseHandle(read_pipe);
        return -1;
      }

      char buf[512];
      DWORD n = 0;
      while (ReadFile(read_pipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
        output.append(buf, n);
      }
      CloseHandle(read_pipe);

      DWORD exit_code = 1;
      if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exit_code);
      } else {
        TerminateProcess(pi.hProcess, 1);
      }
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
      return static_cast<int>(exit_code);
    }

    std::wstring widen(const std::string &s) {
      return {s.begin(), s.end()};
    }

    /**
     * @brief Find the virtual hub port number printed by "usbip attach".
     */
    int parse_attached_port(const std::string &output) {
      const auto pos = output.rfind("port");
      if (pos == std::string::npos) {
        return -1;
      }
      int port = 0;
      bool found = false;
      for (auto i = pos + 4; i < output.size(); ++i) {
        if (output[i] >= '0' && output[i] <= '9') {
          port = port * 10 + (output[i] - '0');
          found = true;
        } else if (found) {
          break;
        }
      }
      return found ? port : -1;
    }
  }  // namespace

  const std::string &hid_report_descriptor() {
    static const auto descriptor = build_hid_report_descriptor();
    return descriptor;
  }

  void reset_state(ds5_state_t &state) {
    state.report.fill(0);
    state.report[0] = 0x01;
    state.report[IN_LX] = state.report[IN_LY] = state.report[IN_RX] = state.report[IN_RY] = 0x80;
    state.report[IN_BTN0] = 0x08;  // hat neutral
    state.report[IN_TOUCH0] = 0x80;  // no contact
    state.report[IN_TOUCH1] = 0x80;
    state.report[IN_STATUS] = 0x1A;  // charging, full level
    put_le16(&state.report[IN_ACCEL + 2], DS5_ACC_RES_PER_G);  // gravity on Y while resting
    state.touch_ids = {UINT32_MAX, UINT32_MAX};
  }

  void apply_gamepad_state(ds5_state_t &state, const gamepad_state_t &gp) {
    auto &r = state.report;
    const auto axis = [](std::int16_t v) {
      return static_cast<std::uint8_t>((static_cast<std::int32_t>(v) + 32768) >> 8);
    };
    r[IN_LX] = axis(gp.lsX);
    r[IN_LY] = 255 - axis(gp.lsY);
    r[IN_RX] = axis(gp.rsX);
    r[IN_RY] = 255 - axis(gp.rsY);
    r[IN_L2] = gp.lt;
    r[IN_R2] = gp.rt;

    const auto f = gp.buttonFlags;
    const bool up = f & DPAD_UP;
    const bool down = f & DPAD_DOWN;
    const bool left = f & DPAD_LEFT;
    const bool right = f & DPAD_RIGHT;
    std::uint8_t hat = 8;
    if (up && right) {
      hat = 1;
    } else if (down && right) {
      hat = 3;
    } else if (down && left) {
      hat = 5;
    } else if (up && left) {
      hat = 7;
    } else if (up) {
      hat = 0;
    } else if (right) {
      hat = 2;
    } else if (down) {
      hat = 4;
    } else if (left) {
      hat = 6;
    }

    std::uint8_t b0 = hat;
    b0 |= (f & X) ? 0x10 : 0;  // square
    b0 |= (f & A) ? 0x20 : 0;  // cross
    b0 |= (f & B) ? 0x40 : 0;  // circle
    b0 |= (f & Y) ? 0x80 : 0;  // triangle

    std::uint8_t b1 = 0;
    b1 |= (f & LEFT_BUTTON) ? 0x01 : 0;
    b1 |= (f & RIGHT_BUTTON) ? 0x02 : 0;
    b1 |= gp.lt > 0 ? 0x04 : 0;
    b1 |= gp.rt > 0 ? 0x08 : 0;
    b1 |= (f & BACK) ? 0x10 : 0;  // create
    b1 |= (f & START) ? 0x20 : 0;  // options
    b1 |= (f & LEFT_STICK) ? 0x40 : 0;
    b1 |= (f & RIGHT_STICK) ? 0x80 : 0;

    std::uint8_t b2 = 0;
    b2 |= (f & HOME) ? 0x01 : 0;
    b2 |= (f & TOUCHPAD_BUTTON) ? 0x02 : 0;
    b2 |= (f & MISC_BUTTON) ? 0x04 : 0;  // mute

    r[IN_BTN0] = b0;
    r[IN_BTN1] = b1;
    r[IN_BTN2] = b2;
  }

  void apply_touch(ds5_state_t &state, const gamepad_touch_t &touch) {
    constexpr std::array<std::size_t, 2> offsets {IN_TOUCH0, IN_TOUCH1};
    auto &r = state.report;

    const auto release = [&](int slot) {
      r[offsets[slot]] |= 0x80;
      state.touch_ids[slot] = UINT32_MAX;
    };

    if (touch.eventType == LI_TOUCH_EVENT_CANCEL_ALL) {
      release(0);
      release(1);
      return;
    }

    int slot = -1;
    for (int i = 0; i < 2; ++i) {
      if (state.touch_ids[i] == touch.pointerId) {
        slot = i;
      }
    }

    if (touch.eventType == LI_TOUCH_EVENT_UP || touch.eventType == LI_TOUCH_EVENT_CANCEL) {
      if (slot >= 0) {
        release(slot);
      }
      return;
    }

    if (touch.eventType != LI_TOUCH_EVENT_DOWN && touch.eventType != LI_TOUCH_EVENT_MOVE) {
      return;
    }

    if (slot < 0) {
      for (int i = 0; i < 2 && slot < 0; ++i) {
        if (state.touch_ids[i] == UINT32_MAX) {
          slot = i;
        }
      }
      if (slot < 0) {
        return;  // only two contacts are supported
      }
      state.touch_ids[slot] = touch.pointerId;
      r[offsets[slot]] = state.touch_counter++ & 0x7F;
    }

    const auto x = static_cast<std::uint16_t>(std::clamp(touch.x, 0.0f, 1.0f) * 1919.0f);
    const auto y = static_cast<std::uint16_t>(std::clamp(touch.y, 0.0f, 1.0f) * 1079.0f);
    auto *p = &r[offsets[slot]];
    p[0] &= 0x7F;
    p[1] = x & 0xFF;
    p[2] = ((x >> 8) & 0x0F) | ((y & 0x0F) << 4);
    p[3] = (y >> 4) & 0xFF;
  }

  void apply_motion(ds5_state_t &state, const gamepad_motion_t &motion) {
    auto &r = state.report;
    if (motion.motionType == LI_MOTION_TYPE_ACCEL) {
      put_le16(&r[IN_ACCEL + 0], static_cast<std::int32_t>(motion.x / EARTH_G * DS5_ACC_RES_PER_G));
      put_le16(&r[IN_ACCEL + 2], static_cast<std::int32_t>(motion.y / EARTH_G * DS5_ACC_RES_PER_G));
      put_le16(&r[IN_ACCEL + 4], static_cast<std::int32_t>(motion.z / EARTH_G * DS5_ACC_RES_PER_G));
    } else if (motion.motionType == LI_MOTION_TYPE_GYRO) {
      put_le16(&r[IN_GYRO + 0], static_cast<std::int32_t>(motion.x * DS5_GYRO_RAW_PER_DEG_S));
      put_le16(&r[IN_GYRO + 2], static_cast<std::int32_t>(motion.y * DS5_GYRO_RAW_PER_DEG_S));
      put_le16(&r[IN_GYRO + 4], static_cast<std::int32_t>(motion.z * DS5_GYRO_RAW_PER_DEG_S));
    }
  }

  void apply_battery(ds5_state_t &state, const gamepad_battery_t &battery) {
    auto &status = state.report[IN_STATUS];
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

  /**
   * @brief Pending interrupt IN request waiting for a report.
   */
  struct pending_urb_t {
    std::uint32_t seqnum;  ///< USB/IP sequence number.
    std::uint32_t length;  ///< Requested transfer size.
    std::uint32_t packets;  ///< number_of_packets field to echo.
  };

  /**
   * @brief One emulated DualSense device.
   */
  struct device_t {
    int nr = 0;  ///< Global gamepad index.
    std::string busid;  ///< USB/IP bus ID.
    std::uint8_t client_index = 0;  ///< Client relative index for feedback.
    feedback_queue_t feedback_queue;  ///< Feedback queue.

    std::mutex mtx;  ///< Guards state, pending and feedback.
    ds5_state_t state;  ///< Current input state.
    std::deque<pending_urb_t> pending;  ///< Pending IN URBs.
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();  ///< Creation time.

    std::mutex send_mtx;  ///< Serializes socket writes.
    SOCKET sock = INVALID_SOCKET;  ///< Imported connection.
    std::thread conn_thread;  ///< URB loop.
    std::thread attach_thread;  ///< Runs usbip attach.
    std::atomic<int> vhci_port {-1};  ///< usbip-win2 port, for detach.
    std::atomic<bool> closing {false};  ///< Set during teardown.

    std::uint16_t last_low = 0, last_high = 0;  ///< Last rumble.
    std::array<std::uint8_t, 3> last_rgb {0, 0, 0};  ///< Last lightbar color.
    std::uint8_t last_player = 0;  ///< Last player LEDs.
    std::array<std::uint8_t, 22> last_triggers {};  ///< Last trigger effects.
  };

  class server_t {
  public:
    ~server_t() {
      stop();
    }

    bool start() {
      if (running) {
        return true;
      }
      WSADATA wsa;
      if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
      }
      wsa_started = true;

      listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (listen_sock == INVALID_SOCKET) {
        return false;
      }
      BOOL exclusive = TRUE;
      setsockopt(listen_sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));

      sockaddr_in addr {};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(USBIP_PORT);
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (bind(listen_sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR || listen(listen_sock, 4) == SOCKET_ERROR) {
        BOOST_LOG(error) << "usbip_ds5: unable to listen on 127.0.0.1:"sv << USBIP_PORT << " (error "sv << WSAGetLastError()
                         << "). Is another USB/IP server such as usbipd-win running?"sv;
        closesocket(listen_sock);
        listen_sock = INVALID_SOCKET;
        return false;
      }

      running = true;
      accept_thread = std::thread {&server_t::accept_loop, this};
      pump_thread = std::thread {&server_t::pump_loop, this};
      BOOST_LOG(info) << "usbip_ds5: USB/IP server listening on 127.0.0.1:"sv << USBIP_PORT;
      return true;
    }

    void stop() {
      if (running.exchange(false)) {
        closesocket(listen_sock);
        listen_sock = INVALID_SOCKET;
        pump_cv.notify_all();
        accept_thread.join();
        pump_thread.join();
      }
      for (int i = 0; i < MAX_GAMEPADS; ++i) {
        remove(i);
      }
      if (wsa_started) {
        WSACleanup();
        wsa_started = false;
      }
    }

    std::shared_ptr<device_t> get(int nr) {
      if (nr < 0 || nr >= MAX_GAMEPADS) {
        return nullptr;
      }
      std::lock_guard lg {devices_mtx};
      return devices[nr];
    }

    int add(const gamepad_id_t &id, feedback_queue_t queue) {
      const auto usbip = find_usbip_exe();
      if (usbip.empty()) {
        BOOST_LOG(error) << "usbip_ds5: usbip-win2 is not installed (usbip.exe not found)"sv;
        return -1;
      }
      if (!start()) {
        return -1;
      }
      if (id.globalIndex < 0 || id.globalIndex >= MAX_GAMEPADS) {
        return -1;
      }
      remove(id.globalIndex);

      auto dev = std::make_shared<device_t>();
      dev->nr = id.globalIndex;
      dev->busid = "1-" + std::to_string(id.globalIndex + 1);
      dev->client_index = id.clientRelativeIndex;
      dev->feedback_queue = std::move(queue);
      reset_state(dev->state);
      {
        std::lock_guard lg {devices_mtx};
        devices[id.globalIndex] = dev;
      }

      if (dev->feedback_queue) {
        dev->feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(dev->client_index, LI_MOTION_TYPE_ACCEL, 100));
        dev->feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(dev->client_index, LI_MOTION_TYPE_GYRO, 100));
      }

      std::weak_ptr<device_t> weak = dev;
      dev->attach_thread = std::thread {[weak, usbip]() {
        auto dev = weak.lock();
        if (!dev) {
          return;
        }
        std::string output;
        const auto cmd = L"\"" + widen(usbip) + L"\" attach -r 127.0.0.1 -b " + widen(dev->busid);
        const auto rc = run_process(cmd, output, 20000);
        if (rc != 0) {
          BOOST_LOG(error) << "usbip_ds5: usbip attach failed for "sv << dev->busid << " (exit "sv << rc << "): "sv << output;
          return;
        }
        dev->vhci_port = parse_attached_port(output);
        BOOST_LOG(info) << "usbip_ds5: DualSense "sv << dev->busid << " attached to usbip port "sv << dev->vhci_port.load();
      }};

      BOOST_LOG(info) << "Gamepad "sv << id.globalIndex << " will be DualSense controller (usbip-win2)"sv;
      return 0;
    }

    void remove(int nr) {
      std::shared_ptr<device_t> dev;
      {
        std::lock_guard lg {devices_mtx};
        if (nr < 0 || nr >= MAX_GAMEPADS) {
          return;
        }
        dev = std::move(devices[nr]);
      }
      if (!dev) {
        return;
      }
      dev->closing = true;
      if (dev->attach_thread.joinable()) {
        dev->attach_thread.join();
      }
      if (const auto port = dev->vhci_port.load(); port > 0) {
        std::string output;
        const auto usbip = find_usbip_exe();
        if (!usbip.empty()) {
          run_process(L"\"" + widen(usbip) + L"\" detach -p " + std::to_wstring(port), output, 10000);
        }
      }
      close_connection(*dev);
      BOOST_LOG(info) << "usbip_ds5: DualSense "sv << dev->busid << " removed"sv;
    }

    void notify() {
      pump_cv.notify_one();
    }

  private:
    static void close_connection(device_t &dev) {
      SOCKET s;
      {
        std::lock_guard lg {dev.send_mtx};
        s = dev.sock;
        dev.sock = INVALID_SOCKET;
      }
      if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
      }
      if (dev.conn_thread.joinable() && dev.conn_thread.get_id() != std::this_thread::get_id()) {
        dev.conn_thread.join();
      }
    }

    void accept_loop() {
      while (running) {
        const auto s = accept(listen_sock, nullptr, nullptr);
        if (s == INVALID_SOCKET) {
          if (!running) {
            break;
          }
          std::this_thread::sleep_for(50ms);
          continue;
        }
        BOOL nodelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));
        DWORD timeout = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        if (!handle_op(s)) {
          closesocket(s);
        }
      }
    }

    static void fill_usb_device(std::uint8_t *p, const device_t &dev) {
      // struct usbip_usb_device, 312 bytes
      std::memset(p, 0, 312);
      const auto path = "/sys/devices/platform/sunshine/ds5-" + std::to_string(dev.nr + 1);
      std::memcpy(p, path.data(), std::min<std::size_t>(path.size(), 255));
      std::memcpy(p + 256, dev.busid.data(), std::min<std::size_t>(dev.busid.size(), 31));
      put_be32(p + 288, 1);  // busnum
      put_be32(p + 292, dev.nr + 1);  // devnum
      put_be32(p + 296, USB_SPEED_HIGH);
      put_be16(p + 300, DS5_VID);
      put_be16(p + 302, DS5_PID);
      put_be16(p + 304, 0x0100);
      p[306] = 0;  // class
      p[307] = 0;
      p[308] = 0;
      p[309] = 1;  // bConfigurationValue
      p[310] = 1;  // bNumConfigurations
      p[311] = 1;  // bNumInterfaces
    }

    /**
     * @brief Handle the operation phase of a new connection.
     * @return True when the socket was handed over to a device.
     */
    bool handle_op(SOCKET s) {
      std::uint8_t hdr[8];
      if (!recv_all(s, hdr, sizeof(hdr))) {
        return false;
      }
      const auto code = get_be16(hdr + 2);

      if (code == OP_REQ_DEVLIST) {
        std::vector<std::shared_ptr<device_t>> list;
        {
          std::lock_guard lg {devices_mtx};
          for (auto &d : devices) {
            if (d) {
              list.push_back(d);
            }
          }
        }
        std::vector<std::uint8_t> rep(12 + list.size() * (312 + 4));
        put_be16(rep.data(), USBIP_VERSION);
        put_be16(rep.data() + 2, OP_REP_DEVLIST);
        put_be32(rep.data() + 8, static_cast<std::uint32_t>(list.size()));
        auto *p = rep.data() + 12;
        for (auto &d : list) {
          fill_usb_device(p, *d);
          p[312] = 0x03;  // HID interface
          p += 316;
        }
        send_all(s, rep.data(), rep.size());
        return false;
      }

      if (code != OP_REQ_IMPORT) {
        return false;
      }

      char busid[33] {};
      if (!recv_all(s, reinterpret_cast<std::uint8_t *>(busid), 32)) {
        return false;
      }

      std::shared_ptr<device_t> dev;
      {
        std::lock_guard lg {devices_mtx};
        for (auto &d : devices) {
          if (d && d->busid == busid && !d->closing) {
            dev = d;
          }
        }
      }

      std::uint8_t rep[8 + 312] {};
      put_be16(rep, USBIP_VERSION);
      put_be16(rep + 2, OP_REP_IMPORT);
      if (!dev) {
        put_be32(rep + 4, 1);  // ST_NA
        send_all(s, rep, 8);
        return false;
      }
      fill_usb_device(rep + 8, *dev);
      if (!send_all(s, rep, sizeof(rep))) {
        return false;
      }

      // A reattach replaces the previous connection.
      close_connection(*dev);
      DWORD no_timeout = 0;
      setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&no_timeout), sizeof(no_timeout));
      {
        std::lock_guard lg {dev->send_mtx};
        dev->sock = s;
      }
      {
        std::lock_guard lg {dev->mtx};
        dev->pending.clear();
      }
      dev->conn_thread = std::thread {&server_t::urb_loop, dev, s};
      BOOST_LOG(info) << "usbip_ds5: usbip-win2 imported "sv << dev->busid;
      return true;
    }

    static bool send_ret_submit(device_t &dev, SOCKET s, std::uint32_t seqnum, std::int32_t status, const std::uint8_t *data, std::uint32_t len, std::uint32_t packets) {
      std::uint8_t hdr[48] {};
      put_be32(hdr + 0, USBIP_RET_SUBMIT);
      put_be32(hdr + 4, seqnum);
      put_be32(hdr + 20, static_cast<std::uint32_t>(status));
      put_be32(hdr + 24, len);
      put_be32(hdr + 32, packets);
      std::lock_guard lg {dev.send_mtx};
      if (dev.sock != s) {
        return false;
      }
      return send_all(s, hdr, sizeof(hdr)) && (len == 0 || send_all(s, data, len));
    }

    static void send_ret_unlink(device_t &dev, SOCKET s, std::uint32_t seqnum, std::int32_t status) {
      std::uint8_t hdr[48] {};
      put_be32(hdr + 0, USBIP_RET_UNLINK);
      put_be32(hdr + 4, seqnum);
      put_be32(hdr + 20, static_cast<std::uint32_t>(status));
      std::lock_guard lg {dev.send_mtx};
      if (dev.sock == s) {
        send_all(s, hdr, sizeof(hdr));
      }
    }

    static std::string feature_report(device_t &dev, std::uint8_t id) {
      std::uint8_t size = 63;
      for (const auto &[fid, fsize] : FEATURE_REPORTS) {
        if (fid == id) {
          size = fsize;
        }
      }
      std::string r(size + 1, '\0');
      auto *p = reinterpret_cast<std::uint8_t *>(r.data());
      p[0] = id;
      if (id == 0x05) {  // IMU calibration
        put_le16(p + 7, DS5_GYRO_CAL_PLUS);
        put_le16(p + 9, -DS5_GYRO_CAL_PLUS);
        put_le16(p + 11, DS5_GYRO_CAL_PLUS);
        put_le16(p + 13, -DS5_GYRO_CAL_PLUS);
        put_le16(p + 15, DS5_GYRO_CAL_PLUS);
        put_le16(p + 17, -DS5_GYRO_CAL_PLUS);
        put_le16(p + 19, DS5_GYRO_CAL_SPEED);
        put_le16(p + 21, DS5_GYRO_CAL_SPEED);
        for (int axis = 0; axis < 3; ++axis) {
          put_le16(p + 23 + axis * 4, DS5_ACC_RES_PER_G);
          put_le16(p + 25 + axis * 4, -DS5_ACC_RES_PER_G);
        }
      } else if (id == 0x09) {  // pairing info: MAC address
        const std::uint8_t mac[6] {static_cast<std::uint8_t>(0x10 + dev.nr), 0x53, 0x4E, 0x55, 0x53, 0x02};
        std::memcpy(p + 1, mac, sizeof(mac));
      } else if (id == 0x20) {  // firmware info
        std::memcpy(p + 1, "Jun 15 2024", 11);
        std::memcpy(p + 12, "12:00:00", 8);
        put_le32(p + 24, 0x00000617);  // hardware version
        put_le32(p + 28, 0x0110002A);  // firmware version
        p[44] = 0x30;  // update version 0x0630
        p[45] = 0x06;
      }
      return r;
    }

    /**
     * @brief Handle a control transfer.
     * @return Status, data written to out.
     */
    static std::int32_t handle_control(device_t &dev, const std::uint8_t *setup, const std::vector<std::uint8_t> &out_data, std::string &in) {
      const auto bm = setup[0];
      const auto req = setup[1];
      const auto value = static_cast<std::uint16_t>(setup[2] | (setup[3] << 8));
      const auto length = static_cast<std::uint16_t>(setup[6] | (setup[7] << 8));
      const auto type = value >> 8;
      const auto index = value & 0xFF;

      std::int32_t status = 0;
      if ((bm & 0x60) == 0x00) {  // standard
        switch (req) {
          case 0x06:  // GET_DESCRIPTOR
            if (type == 0x01) {
              in = device_descriptor();
            } else if (type == 0x02) {
              in = config_descriptor();
            } else if (type == 0x03) {
              if (index == 0) {
                in = std::string {"\x04\x03\x09\x04", 4};
              } else if (index == 1) {
                in = usb_string(L"Sony Interactive Entertainment");
              } else if (index == 2) {
                in = usb_string(L"DualSense Wireless Controller");
              } else {
                status = ERR_PIPE;
              }
            } else if (type == 0x06) {
              in = device_qualifier();
            } else if (type == 0x22) {
              in = hid_report_descriptor();
            } else if (type == 0x21) {
              in = config_descriptor().substr(18, 9);
            } else {
              status = ERR_PIPE;
            }
            break;
          case 0x00:  // GET_STATUS
            in = std::string(2, '\0');
            break;
          case 0x08:  // GET_CONFIGURATION
            in = std::string(1, '\x01');
            break;
          case 0x0A:  // GET_INTERFACE
            in = std::string(1, '\0');
            break;
          default:  // SET_CONFIGURATION, SET_INTERFACE, CLEAR_FEATURE, SET_FEATURE...
            break;
        }
      } else if ((bm & 0x60) == 0x20) {  // HID class
        switch (req) {
          case 0x01:  // GET_REPORT
            if (type == 0x01) {
              std::lock_guard lg {dev.mtx};
              in.assign(reinterpret_cast<const char *>(dev.state.report.data()), dev.state.report.size());
            } else if (type == 0x03) {
              in = feature_report(dev, index);
            } else {
              status = ERR_PIPE;
            }
            break;
          case 0x09:  // SET_REPORT
            if (type == 0x02) {
              handle_output(dev, out_data.data(), out_data.size());
            }
            break;
          case 0x02:  // GET_IDLE
            in = std::string(1, '\0');
            break;
          case 0x03:  // GET_PROTOCOL
            in = std::string(1, '\x01');
            break;
          default:  // SET_IDLE, SET_PROTOCOL
            break;
        }
      } else {
        status = ERR_PIPE;
      }

      if (in.size() > length) {
        in.resize(length);
      }
      return status;
    }

    static void handle_output(device_t &dev, const std::uint8_t *d, std::size_t n) {
      if (n < OUTPUT_REPORT_SIZE || d[0] != 0x02) {
        return;
      }
      const auto flags0 = d[1];
      const auto flags1 = d[2];

      std::lock_guard lg {dev.mtx};
      if (!dev.feedback_queue) {
        return;
      }

      if (flags0 & 0x03) {
        const std::uint16_t low = d[4] * 257;  // left motor
        const std::uint16_t high = d[3] * 257;  // right motor
        if (low != dev.last_low || high != dev.last_high) {
          dev.last_low = low;
          dev.last_high = high;
          dev.feedback_queue->raise(gamepad_feedback_msg_t::make_rumble(dev.client_index, low, high));
        }
      }

      if (flags0 & 0x0C) {
        std::array<std::uint8_t, 22> triggers {};
        std::memcpy(triggers.data(), d + 11, 22);
        if (triggers != dev.last_triggers) {
          dev.last_triggers = triggers;
          std::array<std::uint8_t, 10> right {};
          std::array<std::uint8_t, 10> left {};
          std::memcpy(right.data(), d + 12, 10);
          std::memcpy(left.data(), d + 23, 10);
          dev.feedback_queue->raise(gamepad_feedback_msg_t::make_adaptive_triggers(dev.client_index, flags0 & 0x0C, d[22], d[11], left, right));
        }
      }

      if (flags1 & 0x04) {
        const std::array<std::uint8_t, 3> rgb {d[45], d[46], d[47]};
        if (rgb != dev.last_rgb) {
          dev.last_rgb = rgb;
          dev.feedback_queue->raise(gamepad_feedback_msg_t::make_rgb_led(dev.client_index, rgb[0], rgb[1], rgb[2]));
        }
      }

      if (flags1 & 0x10) {
        const std::uint8_t player = d[44] & 0x1F;
        if (player != dev.last_player) {
          dev.last_player = player;
          dev.feedback_queue->raise(gamepad_feedback_msg_t::make_player_leds(dev.client_index, player, 0));
        }
      }
    }

    static void urb_loop(std::shared_ptr<device_t> dev, SOCKET s) {
      std::uint8_t hdr[48];
      std::vector<std::uint8_t> out;
      while (recv_all(s, hdr, sizeof(hdr))) {
        const auto command = get_be32(hdr);
        const auto seqnum = get_be32(hdr + 4);
        const auto direction = get_be32(hdr + 12);
        const auto ep = get_be32(hdr + 16);

        if (command == USBIP_CMD_UNLINK) {
          const auto victim = get_be32(hdr + 20);
          bool found = false;
          {
            std::lock_guard lg {dev->mtx};
            const auto it = std::find_if(dev->pending.begin(), dev->pending.end(), [victim](const auto &p) {
              return p.seqnum == victim;
            });
            if (it != dev->pending.end()) {
              dev->pending.erase(it);
              found = true;
            }
          }
          send_ret_unlink(*dev, s, seqnum, found ? ERR_CONNRESET : 0);
          continue;
        }

        if (command != USBIP_CMD_SUBMIT) {
          break;
        }

        const auto length = get_be32(hdr + 24);
        const auto packets = get_be32(hdr + 32);
        const std::uint8_t *setup = hdr + 40;
        if (length > 65536) {
          break;
        }

        out.clear();
        if (direction == USBIP_DIR_OUT && length > 0) {
          out.resize(length);
          if (!recv_all(s, out.data(), length)) {
            break;
          }
        }

        bool ok = true;
        if (ep == 0) {
          std::string in;
          const auto status = handle_control(*dev, setup, out, in);
          if (direction == USBIP_DIR_IN) {
            ok = send_ret_submit(*dev, s, seqnum, status, reinterpret_cast<const std::uint8_t *>(in.data()), status == 0 ? static_cast<std::uint32_t>(in.size()) : 0, packets);
          } else if (status == 0) {
            ok = send_ret_submit_out(*dev, s, seqnum, length, packets);
          } else {
            ok = send_ret_submit(*dev, s, seqnum, status, nullptr, 0, packets);
          }
        } else if (ep == EP_IN && direction == USBIP_DIR_IN) {
          std::lock_guard lg {dev->mtx};
          dev->pending.push_back({seqnum, length, packets});
        } else if (ep == EP_OUT && direction == USBIP_DIR_OUT) {
          handle_output(*dev, out.data(), out.size());
          ok = send_ret_submit_out(*dev, s, seqnum, length, packets);
        } else {
          ok = send_ret_submit(*dev, s, seqnum, ERR_PIPE, nullptr, 0, packets);
        }
        if (!ok) {
          break;
        }
      }

      if (!dev->closing) {
        BOOST_LOG(info) << "usbip_ds5: connection for "sv << dev->busid << " closed by usbip-win2"sv;
      }
      std::lock_guard lg {dev->send_mtx};
      if (dev->sock == s) {
        closesocket(s);
        dev->sock = INVALID_SOCKET;
      }
    }

    /**
     * @brief Complete an OUT transfer. The header carries actual_length but no payload.
     */
    static bool send_ret_submit_out(device_t &dev, SOCKET s, std::uint32_t seqnum, std::uint32_t length, std::uint32_t packets) {
      std::uint8_t hdr[48] {};
      put_be32(hdr + 0, USBIP_RET_SUBMIT);
      put_be32(hdr + 4, seqnum);
      put_be32(hdr + 24, length);
      put_be32(hdr + 32, packets);
      std::lock_guard lg {dev.send_mtx};
      return dev.sock == s && send_all(s, hdr, sizeof(hdr));
    }

    void pump_loop() {
      while (running) {
        {
          std::unique_lock ul {pump_mtx};
          pump_cv.wait_for(ul, 4ms);
        }
        std::array<std::shared_ptr<device_t>, MAX_GAMEPADS> snapshot;
        {
          std::lock_guard lg {devices_mtx};
          snapshot = devices;
        }
        for (auto &dev : snapshot) {
          if (!dev) {
            continue;
          }
          pending_urb_t urb;
          std::array<std::uint8_t, INPUT_REPORT_SIZE> report;
          SOCKET s;
          {
            std::lock_guard lg {dev->send_mtx};
            s = dev->sock;
          }
          if (s == INVALID_SOCKET) {
            continue;
          }
          {
            std::lock_guard lg {dev->mtx};
            if (dev->pending.empty()) {
              continue;
            }
            urb = dev->pending.front();
            dev->pending.pop_front();
            dev->state.report[IN_SEQ] = dev->state.seq++;
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - dev->created).count();
            put_le32(&dev->state.report[IN_TIMESTAMP], static_cast<std::uint32_t>(us * 3));
            report = dev->state.report;
          }
          const auto len = std::min<std::uint32_t>(urb.length, INPUT_REPORT_SIZE);
          send_ret_submit(*dev, s, urb.seqnum, 0, report.data(), len, urb.packets);
        }
      }
    }

    std::atomic<bool> running {false};
    bool wsa_started = false;
    SOCKET listen_sock = INVALID_SOCKET;
    std::thread accept_thread;
    std::thread pump_thread;
    std::mutex pump_mtx;
    std::condition_variable pump_cv;
    std::mutex devices_mtx;
    std::array<std::shared_ptr<device_t>, MAX_GAMEPADS> devices;
  };

  context_t::context_t() = default;

  context_t::~context_t() = default;

  std::string find_usbip_exe() {
    std::vector<std::string> candidates;
    for (const auto *var : {"ProgramW6432", "ProgramFiles"}) {
      char buf[MAX_PATH];
      if (const auto n = GetEnvironmentVariableA(var, buf, sizeof(buf)); n > 0 && n < sizeof(buf)) {
        candidates.emplace_back(std::string {buf} + "\\USBip\\usbip.exe");
      }
    }
    candidates.emplace_back("C:\\Program Files\\USBip\\usbip.exe");
    for (const auto &path : candidates) {
      if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return path;
      }
    }
    char found[MAX_PATH];
    if (SearchPathA(nullptr, "usbip.exe", nullptr, sizeof(found), found, nullptr) > 0) {
      return found;
    }
    return {};
  }

  bool available() {
    return !find_usbip_exe().empty();
  }

  int alloc_gamepad(context_t &context, const gamepad_id_t &id, feedback_queue_t feedback_queue) {
    if (!context.server) {
      context.server = std::make_unique<server_t>();
    }
    return context.server->add(id, std::move(feedback_queue));
  }

  bool has_gamepad(context_t &context, int nr) {
    return context.server && context.server->get(nr) != nullptr;
  }

  int rebind_gamepad(context_t &context, const gamepad_id_t &id, feedback_queue_t feedback_queue) {
    auto dev = context.server ? context.server->get(id.globalIndex) : nullptr;
    if (!dev) {
      return -1;
    }
    std::lock_guard lg {dev->mtx};
    dev->client_index = id.clientRelativeIndex;
    dev->feedback_queue = std::move(feedback_queue);
    if (dev->feedback_queue) {
      dev->feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(dev->client_index, LI_MOTION_TYPE_ACCEL, 100));
      dev->feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(dev->client_index, LI_MOTION_TYPE_GYRO, 100));
    }
    return 0;
  }

  void free_gamepad(context_t &context, int nr) {
    if (context.server) {
      context.server->remove(nr);
    }
  }

  void gamepad_update(context_t &context, int nr, const gamepad_state_t &state) {
    auto dev = context.server ? context.server->get(nr) : nullptr;
    if (!dev) {
      return;
    }
    {
      std::lock_guard lg {dev->mtx};
      apply_gamepad_state(dev->state, state);
    }
    context.server->notify();
  }

  void gamepad_touch(context_t &context, const gamepad_touch_t &touch) {
    auto dev = context.server ? context.server->get(touch.id.globalIndex) : nullptr;
    if (!dev) {
      return;
    }
    {
      std::lock_guard lg {dev->mtx};
      apply_touch(dev->state, touch);
    }
    context.server->notify();
  }

  void gamepad_motion(context_t &context, const gamepad_motion_t &motion) {
    auto dev = context.server ? context.server->get(motion.id.globalIndex) : nullptr;
    if (!dev) {
      return;
    }
    std::lock_guard lg {dev->mtx};
    apply_motion(dev->state, motion);
  }

  void gamepad_battery(context_t &context, const gamepad_battery_t &battery) {
    auto dev = context.server ? context.server->get(battery.id.globalIndex) : nullptr;
    if (!dev) {
      return;
    }
    std::lock_guard lg {dev->mtx};
    apply_battery(dev->state, battery);
  }
}  // namespace platf::usbip_ds5
