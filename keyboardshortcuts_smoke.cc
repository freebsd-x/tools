// KeyboardShortcuts backend smoke test (paged-list service).
//
// Exercises com.freebsdx.KeyboardShortcuts end-to-end against a running
// freebsdx compositor:
//   - walks the page list (ListShortcuts(page) until !has_more_) and checks the
//     valid_/has_more_/total_ bookkeeping is self-consistent and that each row
//     carries a non-empty display-only binding_;
//   - flips one shortcut's enabled flag via SetShortcut, verifies the
//     ShortcutsChanged broadcast fires, and re-reads page 0 to confirm the flip
//     persisted (and that binding_ is untouched).
// Restores the original state before exiting.
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

static bool ks_list(std::shared_ptr<IpcSocket>& s, XBusClient& x,
                    uint32_t page, KeyShortcutPage& out) {
    bool got = false;
    x.Call("com.freebsdx.KeyboardShortcuts",
           static_cast<uint32_t>(KeyboardShortcutOpcode::ListShortcuts),
           {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
           [&](const XBusMessage& r) {
               std::memcpy(&out, r.payload_, sizeof(out));
               got = true;
           });
    return drain_until(s, x, got, 2000);
}

static bool ks_set(std::shared_ptr<IpcSocket>& s, XBusClient& x,
                   const std::string& action, uint32_t enabled) {
    KeyShortcutRequest req{};
    std::strncpy(req.action_, action.c_str(), sizeof(req.action_) - 1);
    req.enabled_ = enabled;
    bool ack = false;
    x.Call("com.freebsdx.KeyboardShortcuts",
           static_cast<uint32_t>(KeyboardShortcutOpcode::SetShortcut),
           {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
           [&](const XBusMessage&) { ack = true; });
    return drain_until(s, x, ack, 2000);
}

int main() {
    std::cout << "\033[1;36m=== KeyboardShortcuts backend smoke test ===\033[0m\n";

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

    // Capture ShortcutsChanged broadcasts.
    bool bcast_ok = false;
    xbus.OnBroadcast([&](const XBusMessage& msg) {
        if (std::strcmp(msg.service_name_, "com.freebsdx.KeyboardShortcuts") == 0 &&
            msg.opcode_ == static_cast<uint32_t>(KeyboardShortcutOpcode::ShortcutsChanged)) {
            bcast_ok = true;
        }
    });

    // 1. Walk the page list.
    KeyShortcutPage first{};
    if (!ks_list(cs, xbus, 0, first)) {
        std::cout << "\033[1;31mFAIL\033[0m: ListShortcuts(0) timed out (service unbacked?)\n";
        return 1;
    }
    if (!first.valid_ || first.total_ == 0) {
        std::cout << "\033[1;31mFAIL\033[0m: empty/invalid first page\n";
        return 1;
    }
    const std::string act0 = first.action_;
    const uint32_t    act0_enabled = first.enabled_;
    const uint32_t    total = first.total_;

    uint32_t walked = 0;
    bool bindings_ok = true;
    bool has_more = true;
    for (uint32_t p = 0; p < total + 2 && has_more; ++p) {
        KeyShortcutPage pg{};
        if (!ks_list(cs, xbus, p, pg)) {
            std::cout << "\033[1;31mFAIL\033[0m: ListShortcuts(" << p << ") timed out\n";
            return 1;
        }
        if (!pg.valid_) break;
        if (pg.binding_[0] == '\0') bindings_ok = false;
        std::cout << "  [" << pg.page_ << "] " << pg.action_
                  << " = " << pg.binding_
                  << " — " << (pg.enabled_ ? "On" : "Off")
                  << (pg.has_more_ ? "" : "  (last)") << "\n";
        ++walked;
        has_more = pg.has_more_;
    }
    KeyShortcutPage past{};
    bool past_ok = ks_list(cs, xbus, total, past) && (past.valid_ == 0);
    bool walk_ok = (walked == total) && past_ok && bindings_ok;

    // 2. Flip act0's enabled flag and confirm broadcast + persistence.
    const uint32_t flipped = act0_enabled ? 0u : 1u;
    if (!ks_set(cs, xbus, act0, flipped)) {
        std::cout << "\033[1;31mFAIL\033[0m: SetShortcut not acked\n";
        return 1;
    }
    drain_until(cs, xbus, bcast_ok, 1000);

    KeyShortcutPage after{};
    bool reread = ks_list(cs, xbus, 0, after);
    bool persist_ok = reread && (std::string(after.action_) == act0) &&
                      (after.enabled_ == flipped) && (after.binding_[0] != '\0');

    std::cout << "  page walk:      " << (walk_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m")
              << " (" << walked << "/" << total << " shortcuts, bindings "
              << (bindings_ok ? "present" : "MISSING") << ")\n";
    std::cout << "  broadcast:      " << (bcast_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m")
              << " (ShortcutsChanged)\n";
    std::cout << "  state persists: " << (persist_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m")
              << " ('" << act0 << "' " << act0_enabled << "->" << after.enabled_ << ")\n";

    // Restore original state.
    ks_set(cs, xbus, act0, act0_enabled);

    const bool all_ok = walk_ok && bcast_ok && persist_ok;
    std::cout << (all_ok ? "\033[1;32m=== KeyboardShortcuts smoke: PASS ===\033[0m\n"
                         : "\033[1;31m=== KeyboardShortcuts smoke: FAIL ===\033[0m\n");
    return all_ok ? 0 : 1;
}
