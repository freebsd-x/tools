// DefaultApps backend smoke test (paged-list, cycle variant).
//
// Exercises com.freebsdx.DefaultApps end-to-end against a running freebsdx
// compositor:
//   - walks the page list (ListDefaults(page) until !has_more_) and checks the
//     valid_/has_more_/total_ bookkeeping is self-consistent;
//   - cycles the first category's handler via CycleDefault, verifies the
//     handler actually advanced, that the DefaultsChanged broadcast fires, and
//     re-reads to confirm persistence;
//   - keeps cycling until the handler wraps back to the original (exercises
//     wrap-around AND restores the seeded state).
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

static bool da_list(std::shared_ptr<IpcSocket>& s, XBusClient& x,
                    uint32_t page, DefaultAppPage& out) {
    bool got = false;
    x.Call("com.freebsdx.DefaultApps",
           static_cast<uint32_t>(DefaultAppsOpcode::ListDefaults),
           {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
           [&](const XBusMessage& r) {
               std::memcpy(&out, r.payload_, sizeof(out));
               got = true;
           });
    return drain_until(s, x, got, 2000);
}

static bool da_cycle(std::shared_ptr<IpcSocket>& s, XBusClient& x,
                     const std::string& category) {
    DefaultAppRequest req{};
    std::strncpy(req.category_, category.c_str(), sizeof(req.category_) - 1);
    bool ack = false;
    x.Call("com.freebsdx.DefaultApps",
           static_cast<uint32_t>(DefaultAppsOpcode::CycleDefault),
           {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
           [&](const XBusMessage&) { ack = true; });
    return drain_until(s, x, ack, 2000);
}

int main() {
    std::cout << "\033[1;36m=== DefaultApps backend smoke test ===\033[0m\n";

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

    // Capture DefaultsChanged broadcasts.
    bool bcast_ok = false;
    xbus.OnBroadcast([&](const XBusMessage& msg) {
        if (std::strcmp(msg.service_name_, "com.freebsdx.DefaultApps") == 0 &&
            msg.opcode_ == static_cast<uint32_t>(DefaultAppsOpcode::DefaultsChanged)) {
            bcast_ok = true;
        }
    });

    // 1. Walk the page list.
    DefaultAppPage first{};
    if (!da_list(cs, xbus, 0, first)) {
        std::cout << "\033[1;31mFAIL\033[0m: ListDefaults(0) timed out (service unbacked?)\n";
        return 1;
    }
    if (!first.valid_ || first.total_ == 0) {
        std::cout << "\033[1;31mFAIL\033[0m: empty/invalid first page\n";
        return 1;
    }
    const std::string cat0 = first.category_;
    const std::string h0   = first.handler_;
    const uint32_t    total = first.total_;

    uint32_t walked = 0;
    bool has_more = true;
    for (uint32_t p = 0; p < total + 2 && has_more; ++p) {
        DefaultAppPage pg{};
        if (!da_list(cs, xbus, p, pg)) {
            std::cout << "\033[1;31mFAIL\033[0m: ListDefaults(" << p << ") timed out\n";
            return 1;
        }
        if (!pg.valid_) break;
        std::cout << "  [" << pg.page_ << "] " << pg.category_
                  << " -> " << pg.handler_
                  << (pg.has_more_ ? "" : "  (last)") << "\n";
        ++walked;
        has_more = pg.has_more_;
    }
    DefaultAppPage past{};
    bool past_ok = da_list(cs, xbus, total, past) && (past.valid_ == 0);
    bool walk_ok = (walked == total) && past_ok;

    // 2. Cycle cat0 once; expect the handler to advance.
    if (!da_cycle(cs, xbus, cat0)) {
        std::cout << "\033[1;31mFAIL\033[0m: CycleDefault not acked\n";
        return 1;
    }
    drain_until(cs, xbus, bcast_ok, 1000);

    DefaultAppPage after{};
    bool reread = da_list(cs, xbus, 0, after);
    const std::string h1 = reread ? after.handler_ : "";
    bool cycle_ok = reread && (std::string(after.category_) == cat0) && (h1 != h0) && !h1.empty();

    std::cout << "  page walk:     " << (walk_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m")
              << " (" << walked << "/" << total << " categories)\n";
    std::cout << "  broadcast:     " << (bcast_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m")
              << " (DefaultsChanged)\n";
    std::cout << "  handler cycles:" << (cycle_ok ? " \033[1;32mPASS\033[0m" : " \033[1;31mFAIL\033[0m")
              << " ('" << cat0 << "' " << h0 << " -> " << h1 << ")\n";

    // 3. Keep cycling until we wrap back to h0 (exercises wrap + restores state).
    bool wrap_ok = false;
    for (int i = 0; i < 16; ++i) {
        DefaultAppPage cur{};
        if (!da_list(cs, xbus, 0, cur)) break;
        if (std::string(cur.handler_) == h0) { wrap_ok = true; break; }
        if (!da_cycle(cs, xbus, cat0)) break;
        drain_until(cs, xbus, bcast_ok, 500);
    }
    std::cout << "  wrap/restore:  " << (wrap_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m")
              << " (back to " << h0 << ")\n";

    const bool all_ok = walk_ok && bcast_ok && cycle_ok && wrap_ok;
    std::cout << (all_ok ? "\033[1;32m=== DefaultApps smoke: PASS ===\033[0m\n"
                         : "\033[1;31m=== DefaultApps smoke: FAIL ===\033[0m\n");
    return all_ok ? 0 : 1;
}
