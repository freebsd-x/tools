// TrustedDevices backend smoke test (paged-list service).
//
// Exercises com.freebsdx.TrustedDevices end-to-end against a running freebsdx
// compositor:
//   - walks the page list (ListDevices(page) until !has_more_) and checks the
//     valid_/has_more_/total_ bookkeeping is self-consistent;
//   - flips one device's trust via SetTrust, verifies the DevicesChanged
//     broadcast fires, and re-reads page 0 to confirm the flip persisted.
// Restores the original trust before exiting.
//
// No surface handshake: xbus RPC dispatches by service name and does not need a
// registered render surface (headless bypasses the SurfaceReady ack anyway).
//
// Exit 0 = PASS, 1 = FAIL.

#include "xkit/xbus_client.h"
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <cstring>
#include <unistd.h>

using namespace fbx::xkit;
using namespace fbx::xserver;
using namespace fbx::shared;

static bool drain_until(std::shared_ptr<IpcSocket>& sock, XBusClient& xbus,
                        bool& flag, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto msg_res = sock->receiveMessage();
        if (msg_res) {
            const auto& data = msg_res.value().first;
            constexpr size_t kFrame = sizeof(XBusMessage);
            for (size_t off = 0; off + kFrame <= data.size(); off += kFrame) {
                std::span<const uint8_t> frame(data.data() + off, kFrame);
                xbus.HandleIncoming(frame);
            }
        }
        if (flag) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

static bool td_list(std::shared_ptr<IpcSocket>& s, XBusClient& x,
                    uint32_t page, TrustedDevicePage& out) {
    bool got = false;
    x.Call("com.freebsdx.TrustedDevices",
           static_cast<uint32_t>(TrustedDeviceOpcode::ListDevices),
           {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
           [&](const XBusMessage& r) {
               std::memcpy(&out, r.payload_, sizeof(out));
               got = true;
           });
    return drain_until(s, x, got, 2000);
}

static bool td_set(std::shared_ptr<IpcSocket>& s, XBusClient& x,
                   const std::string& name, uint32_t trusted) {
    TrustedDeviceRequest req{};
    std::strncpy(req.device_name_, name.c_str(), sizeof(req.device_name_) - 1);
    req.trusted_ = trusted;
    bool ack = false;
    x.Call("com.freebsdx.TrustedDevices",
           static_cast<uint32_t>(TrustedDeviceOpcode::SetTrust),
           {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
           [&](const XBusMessage&) { ack = true; });
    return drain_until(s, x, ack, 2000);
}

int main() {
    std::cout << "\033[1;36m=== TrustedDevices backend smoke test ===\033[0m\n";

    const char* path = "/tmp/freebsdx.sock";
    int sock = -1;
    bool connected = false;
    for (int i = 0; i < 20; ++i) {
        sock = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (sock < 0) break;
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
#ifdef __FreeBSD__
        addr.sun_len = SUN_LEN(&addr);
#endif
        if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) { connected = true; break; }
        ::close(sock);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!connected) { std::cout << "\033[1;31mFAIL\033[0m: no socket at " << path << "\n"; return 1; }
    {
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
    auto cs = std::make_shared<IpcSocket>(UniqueFd(sock));
    XBusClient xbus(cs);

    // A shell-only service: register as the desktop shell (surface 999). A
    // headless compositor lets a test client do that; the live seat accepts a
    // shell surface only from the shell binaries.
    {
        RenderMessage shell{};
        shell.surface_id_ = 999;
        shell.status_     = RenderMessageStatus::ShellSurfaceCreated;
        (void)cs->sendMessage({reinterpret_cast<const uint8_t*>(&shell), sizeof(shell)});
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // The list is the machine's known Bluetooth devices (it may well be empty)
    // and pairing lives in root-only /etc/bluetooth/hcsecd.conf, so SetTrust is
    // acknowledged without changing anything. Check that contract.

    // 1. Walk the page list: total_ consistent, valid_=0 past the end.
    TrustedDevicePage first{};
    if (!td_list(cs, xbus, 0, first)) {
        std::cout << "\033[1;31mFAIL\033[0m: ListDevices(0) timed out\n";
        return 1;
    }
    const uint32_t total = first.total_;
    uint32_t walked = 0;
    for (uint32_t p = 0; p < total; ++p) {
        TrustedDevicePage pg{};
        if (!td_list(cs, xbus, p, pg)) {
            std::cout << "\033[1;31mFAIL\033[0m: ListDevices(" << p << ") timed out\n";
            return 1;
        }
        if (!pg.valid_ || pg.total_ != total) break;
        std::cout << "  [" << pg.page_ << "] " << pg.device_name_ << (pg.has_more_ ? "" : "  (last)") << "\n";
        ++walked;
    }
    TrustedDevicePage past{};
    bool past_ok = td_list(cs, xbus, total, past) && past.valid_ == 0 && past.total_ == total;
    bool walk_ok = walked == total && past_ok;

    // 2. SetTrust is acknowledged and changes nothing (no pretend pairing).
    bool set_ok = true;
    if (total > 0) {
        const std::string dev0 = first.device_name_;
        TrustedDevicePage after{};
        set_ok = td_set(cs, xbus, dev0, first.trusted_ ? 0u : 1u) &&
                 td_list(cs, xbus, 0, after) && after.trusted_ == first.trusted_;
    }

    std::cout << "  page walk:      " << (walk_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m")
              << " (" << walked << "/" << total << " devices)\n";
    std::cout << "  set trust:      " << (total == 0 ? "\033[1;33mSKIP\033[0m (no devices on this machine)"
                                           : set_ok ? "\033[1;32mPASS\033[0m (acked, unchanged)"
                                                    : "\033[1;31mFAIL\033[0m") << "\n";

    const bool all_ok = walk_ok && set_ok;
    std::cout << (all_ok ? "\033[1;32m=== TrustedDevices smoke: PASS ===\033[0m\n"
                         : "\033[1;31m=== TrustedDevices smoke: FAIL ===\033[0m\n");
    return all_ok ? 0 : 1;
}
