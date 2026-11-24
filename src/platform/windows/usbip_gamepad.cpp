/**
 * @file src/platform/windows/usbip_gamepad.cpp
 * @brief USB/IP transport for the free usbip-win2 gamepad backend.
 *
 * Sunshine acts as a tiny USB/IP server on 127.0.0.1 (port 3240 by default) and exports
 * one emulated USB controller per gamepad. usbip-win2 (BSD-2-Clause,
 * https://github.com/vadimgrn/usbip-win2) is the client driver that plugs the device
 * into Windows. The controllers themselves are described in usbip_profiles.cpp.
 */
// platform includes
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// local includes
#include "src/logging.h"
#include "src/utility.h"
#include "usbip_gamepad.h"

using namespace std::literals;

namespace platf::usbip_gamepad {
  namespace {
    constexpr std::uint16_t DEFAULT_USBIP_PORT = 3240;
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
    constexpr std::uint32_t USB_SPEED_FULL = 2;
    constexpr std::uint32_t USB_SPEED_HIGH = 3;
    constexpr std::int32_t ERR_CONNRESET = -104;  ///< -ECONNRESET, URB unlinked.

    /**
     * @brief TCP port of the loopback USB/IP server.
     *
     * Defaults to 3240 and can be changed with the SUNSHINE_USBIP_PORT environment variable,
     * for example when another USB/IP server already uses the default port.
     */
    std::uint16_t usbip_port() {
      static const std::uint16_t port = []() -> std::uint16_t {
        char buf[16];
        if (const auto n = GetEnvironmentVariableA("SUNSHINE_USBIP_PORT", buf, sizeof(buf)); n > 0 && n < sizeof(buf)) {
          const auto v = std::atoi(buf);
          if (v >= 1024 && v <= 65535) {
            return static_cast<std::uint16_t>(v);
          }
        }
        return DEFAULT_USBIP_PORT;
      }();
      return port;
    }

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

    /**
     * @brief Find the first sub-descriptor of a type inside a configuration descriptor.
     */
    std::string find_descriptor(const std::string &config, std::uint8_t type) {
      for (std::size_t i = 0; i + 1 < config.size() && config[i] > 0;) {
        const auto len = static_cast<std::uint8_t>(config[i]);
        if (static_cast<std::uint8_t>(config[i + 1]) == type) {
          return config.substr(i, len);
        }
        i += len;
      }
      return {};
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
     * @brief Find the virtual hub port number printed by "usbip attach --terse".
     */
    int parse_attached_port(const std::string &output) {
      int port = -1;
      int current = -1;
      for (const auto c : output) {
        if (c >= '0' && c <= '9') {
          current = (current < 0 ? 0 : current * 10) + (c - '0');
        } else if (current >= 0) {
          port = current;
          current = -1;
        }
      }
      return current >= 0 ? current : port;
    }
  }  // namespace

  /**
   * @brief Pending interrupt IN request waiting for a report.
   */
  struct pending_urb_t {
    std::uint32_t seqnum;  ///< USB/IP sequence number.
    std::uint32_t length;  ///< Requested transfer size.
    std::uint32_t packets;  ///< number_of_packets field to echo.
  };

  /**
   * @brief One emulated USB controller.
   */
  struct device_t {
    int nr = 0;  ///< Global gamepad index.
    std::string busid;  ///< USB/IP bus ID.
    std::uint8_t client_index = 0;  ///< Client relative index for feedback.
    feedback_queue_t feedback_queue;  ///< Feedback queue.

    std::mutex mtx;  ///< Guards the profile, pending URBs and feedback.
    std::unique_ptr<profile_t> profile;  ///< Emulated controller.
    std::deque<pending_urb_t> pending;  ///< Pending IN URBs.

    std::mutex send_mtx;  ///< Serializes socket writes.
    SOCKET sock = INVALID_SOCKET;  ///< Imported connection.
    std::thread conn_thread;  ///< URB loop.
    std::thread attach_thread;  ///< Runs usbip attach.
    std::atomic<int> vhci_port {-1};  ///< usbip-win2 port, for detach.
    std::atomic<bool> closing {false};  ///< Set during teardown.
    bool logged_gyro = false;  ///< First gyro sample was logged.
    bool logged_accel = false;  ///< First accelerometer sample was logged.

    /**
     * @brief Send feedback to the client. Caller holds mtx.
     */
    void raise(gamepad_feedback_msg_t msg) {
      if (feedback_queue) {
        msg.id = client_index;
        feedback_queue->raise(msg);
      }
    }

    /**
     * @brief Ask the client to stream motion data when the controller has sensors. Caller holds mtx.
     */
    void request_motion() {
      if (profile && profile->has_motion()) {
        raise(gamepad_feedback_msg_t::make_motion_event_state(0, LI_MOTION_TYPE_ACCEL, 100));
        raise(gamepad_feedback_msg_t::make_motion_event_state(0, LI_MOTION_TYPE_GYRO, 100));
      }
    }
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
      addr.sin_port = htons(usbip_port());
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (bind(listen_sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR || listen(listen_sock, 4) == SOCKET_ERROR) {
        BOOST_LOG(error) << "usbip: unable to listen on 127.0.0.1:"sv << usbip_port() << " (error "sv << WSAGetLastError()
                         << "). Is another USB/IP server such as usbipd-win running? Set SUNSHINE_USBIP_PORT to use another port."sv;
        closesocket(listen_sock);
        listen_sock = INVALID_SOCKET;
        return false;
      }

      running = true;
      accept_thread = std::thread {&server_t::accept_loop, this};
      pump_thread = std::thread {&server_t::pump_loop, this};
      BOOST_LOG(info) << "usbip: USB/IP server listening on 127.0.0.1:"sv << usbip_port();
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

    int add(const gamepad_id_t &id, profile_e type, feedback_queue_t queue) {
      const auto usbip = find_usbip_exe();
      if (usbip.empty()) {
        BOOST_LOG(error) << "usbip: usbip-win2 is not installed (usbip.exe not found)"sv;
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
      dev->profile = make_profile(type, id.globalIndex);
      if (!dev->profile) {
        return -1;
      }
      std::weak_ptr<device_t> weak_dev = dev;
      dev->profile->emit = [weak_dev](gamepad_feedback_msg_t msg) {
        // Called from profile code while the device mutex is held.
        if (auto d = weak_dev.lock()) {
          d->raise(msg);
        }
      };
      {
        std::lock_guard lg {dev->mtx};
        dev->request_motion();
      }
      {
        std::lock_guard lg {devices_mtx};
        devices[id.globalIndex] = dev;
      }

      dev->attach_thread = std::thread {[weak_dev, usbip]() {
        auto dev = weak_dev.lock();
        if (!dev) {
          return;
        }
        std::string output;
        const auto cmd = L"\"" + widen(usbip) + L"\" -t " + std::to_wstring(usbip_port()) + L" attach -r 127.0.0.1 -b " + widen(dev->busid) + L" --once --terse --receive-mode low-latency";
        const auto rc = run_process(cmd, output, 20000);
        if (rc != 0) {
          BOOST_LOG(error) << "usbip: usbip attach failed for "sv << dev->busid << " (exit "sv << rc << "): "sv << output;
          return;
        }
        dev->vhci_port = parse_attached_port(output);
        BOOST_LOG(info) << "usbip: "sv << dev->busid << " attached to usbip port "sv << dev->vhci_port.load();
      }};

      BOOST_LOG(info) << "Gamepad "sv << id.globalIndex << " will be "sv << dev->profile->display_name() << " (usbip-win2)"sv;
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
          run_process(L"\"" + widen(usbip) + L"\" -t " + std::to_wstring(usbip_port()) + L" detach -p " + std::to_wstring(port), output, 10000);
        }
      }
      close_connection(*dev);
      BOOST_LOG(info) << "usbip: "sv << dev->busid << " removed"sv;
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
      const auto path = "/sys/devices/platform/sunshine/gamepad-" + std::to_string(dev.nr + 1);
      std::memcpy(p, path.data(), std::min<std::size_t>(path.size(), 255));
      std::memcpy(p + 256, dev.busid.data(), std::min<std::size_t>(dev.busid.size(), 31));
      const auto device = dev.profile->device_descriptor();
      const bool full_speed = static_cast<std::uint8_t>(device[7]) <= 8;  // 8 byte EP0 only exists on full speed devices
      put_be32(p + 288, 1);  // busnum
      put_be32(p + 292, dev.nr + 1);  // devnum
      put_be32(p + 296, full_speed ? USB_SPEED_FULL : USB_SPEED_HIGH);
      put_be16(p + 300, dev.profile->vid());
      put_be16(p + 302, dev.profile->pid());
      put_be16(p + 304, dev.profile->bcd_device());
      p[306] = static_cast<std::uint8_t>(device[4]);  // class
      p[307] = static_cast<std::uint8_t>(device[5]);
      p[308] = static_cast<std::uint8_t>(device[6]);
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
          p[312] = d->profile->interface_class();
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
      BOOST_LOG(info) << "usbip: usbip-win2 imported "sv << dev->busid;
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
      return send_all(s, hdr, sizeof(hdr)) && (len == 0 || data == nullptr || send_all(s, data, len));
    }

    /**
     * @brief Complete an OUT transfer. The header carries actual_length but no payload.
     */
    static bool send_ret_submit_out(device_t &dev, SOCKET s, std::uint32_t seqnum, std::uint32_t length, std::uint32_t packets) {
      return send_ret_submit(dev, s, seqnum, 0, nullptr, length, packets);
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

    /**
     * @brief Handle a control transfer on endpoint 0. Caller holds dev.mtx.
     * @return Status; IN data is written to in.
     */
    static std::int32_t handle_control(device_t &dev, const std::uint8_t *setup, const std::vector<std::uint8_t> &out_data, std::string &in) {
      auto &profile = *dev.profile;
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
              in = profile.device_descriptor();
            } else if (type == 0x02) {
              in = profile.config_descriptor();
            } else if (type == 0x03) {
              if (index == 0) {
                in = std::string {"\x04\x03\x09\x04", 4};
              } else if (index == 1) {
                in = usb_string(profile.manufacturer());
              } else if (index == 2) {
                in = usb_string(profile.product());
              } else if (index == 3 && !profile.serial().empty()) {
                in = usb_string(profile.serial());
              } else {
                status = ERR_PIPE;
              }
            } else if (type == 0x06) {
              const auto device = profile.device_descriptor();
              if (static_cast<std::uint8_t>(device[7]) <= 8) {
                status = ERR_PIPE;  // full speed only device
              } else {
                in = std::string {"\x0A\x06\x00\x02\x00\x00\x00\x40\x01\x00", 10};
              }
            } else if (type == 0x22) {
              in = profile.report_descriptor();
              if (in.empty()) {
                status = ERR_PIPE;
              }
            } else if (type == 0x21) {
              in = find_descriptor(profile.config_descriptor(), 0x21);
              if (in.empty()) {
                status = ERR_PIPE;
              }
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
      } else {
        status = profile.control(setup, out_data, in);
      }

      if (in.size() > length) {
        in.resize(length);
      }
      BOOST_LOG(debug) << "usbip: "sv << dev.busid << " control bm="sv << util::hex(bm).to_string_view() << " req="sv << util::hex(req).to_string_view()
                       << " value="sv << util::hex(value).to_string_view() << " index="sv << (setup[4] | (setup[5] << 8)) << " length="sv << length
                       << " -> status="sv << status << " size="sv << in.size();
      return status;
    }

    static void urb_loop(std::shared_ptr<device_t> dev, SOCKET s) {
      std::uint8_t hdr[48];
      std::vector<std::uint8_t> out;
      const auto in_ep = dev->profile->in_endpoint();
      const auto out_ep = dev->profile->out_endpoint();
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
          std::int32_t status;
          {
            std::lock_guard lg {dev->mtx};
            status = handle_control(*dev, setup, out, in);
          }
          if (status != 0) {
            ok = send_ret_submit(*dev, s, seqnum, status, nullptr, 0, packets);
          } else if (direction == USBIP_DIR_IN) {
            ok = send_ret_submit(*dev, s, seqnum, 0, reinterpret_cast<const std::uint8_t *>(in.data()), static_cast<std::uint32_t>(in.size()), packets);
          } else {
            ok = send_ret_submit_out(*dev, s, seqnum, length, packets);
          }
        } else if (ep == in_ep && direction == USBIP_DIR_IN) {
          {
            std::lock_guard lg {dev->mtx};
            dev->pending.push_back({seqnum, length, packets});
          }
          pump_now();
        } else if (out_ep && ep == out_ep && direction == USBIP_DIR_OUT) {
          {
            std::lock_guard lg {dev->mtx};
            dev->profile->output(out.data(), out.size());
          }
          BOOST_LOG(debug) << "usbip: "sv << dev->busid << " output report "sv << (out.empty() ? 0 : (int) out[0]) << ", "sv << out.size() << " bytes"sv;
          ok = send_ret_submit_out(*dev, s, seqnum, length, packets);
          pump_now();
        } else {
          ok = send_ret_submit(*dev, s, seqnum, ERR_PIPE, nullptr, 0, packets);
        }
        if (!ok) {
          break;
        }
      }

      if (!dev->closing) {
        BOOST_LOG(info) << "usbip: connection for "sv << dev->busid << " closed by usbip-win2"sv;
      }
      std::lock_guard lg {dev->send_mtx};
      if (dev->sock == s) {
        closesocket(s);
        dev->sock = INVALID_SOCKET;
      }
    }

    /**
     * @brief Wake the pump of the active server, used when a reply is waiting.
     */
    static void pump_now() {
      if (auto *server = active.load()) {
        server->notify();
      }
    }

    void pump_loop() {
      active = this;
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
          SOCKET s;
          {
            std::lock_guard lg {dev->send_mtx};
            s = dev->sock;
          }
          if (s == INVALID_SOCKET) {
            continue;
          }
          pending_urb_t urb;
          std::string report;
          {
            std::lock_guard lg {dev->mtx};
            if (dev->pending.empty() || !dev->profile->input(report)) {
              continue;
            }
            urb = dev->pending.front();
            dev->pending.pop_front();
          }
          const auto len = std::min<std::uint32_t>(urb.length, static_cast<std::uint32_t>(report.size()));
          send_ret_submit(*dev, s, urb.seqnum, 0, reinterpret_cast<const std::uint8_t *>(report.data()), len, urb.packets);
        }
      }
      active = nullptr;
    }

    static inline std::atomic<server_t *> active {nullptr};  ///< Server whose pump can be woken up.
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

  int alloc_gamepad(context_t &context, const gamepad_id_t &id, profile_e profile, feedback_queue_t feedback_queue) {
    if (!context.server) {
      context.server = std::make_unique<server_t>();
    }
    return context.server->add(id, profile, std::move(feedback_queue));
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
    dev->request_motion();
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
      dev->profile->update(state);
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
      dev->profile->touch(touch);
    }
    context.server->notify();
  }

  void gamepad_motion(context_t &context, const gamepad_motion_t &motion) {
    auto dev = context.server ? context.server->get(motion.id.globalIndex) : nullptr;
    if (!dev) {
      return;
    }
    std::lock_guard lg {dev->mtx};
    dev->profile->motion(motion);
    auto &logged = motion.motionType == LI_MOTION_TYPE_GYRO ? dev->logged_gyro : dev->logged_accel;
    if (!logged) {
      logged = true;
      BOOST_LOG(info) << "usbip: first "sv << (motion.motionType == LI_MOTION_TYPE_GYRO ? "gyro"sv : "accelerometer"sv)
                      << " sample for "sv << dev->busid << ": "sv << motion.x << ", "sv << motion.y << ", "sv << motion.z;
    }
  }

  void gamepad_battery(context_t &context, const gamepad_battery_t &battery) {
    auto dev = context.server ? context.server->get(battery.id.globalIndex) : nullptr;
    if (!dev) {
      return;
    }
    std::lock_guard lg {dev->mtx};
    dev->profile->battery(battery);
  }
}  // namespace platf::usbip_gamepad
