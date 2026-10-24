// OptStorage backend smoke test.
//
// Exercises com.freebsdx.OptStorage end-to-end against a running freebsdx
// compositor: GetState → SetState → verify the StateChanged broadcast fires
// AND a follow-up GetState reflects the mutation. Also checks that the
// display-only reclaimable_mb_ field stays populated and is NOT clobbered by
// SetState (which only carries enabled_/target_pct_).
//
// No surface handshake: xbus RPC dispatches by service name and does not need a
// registered render surface (headless bypasses the SurfaceReady ack anyway).
//
// Exit 0 = PASS, 1 = FAIL. Restores the original state before exiting.

#include "xkit/xbus_client.h"
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <iostream>
#include <span>
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
            // One recv can coalesce several 256-byte XBusMessages (a SetState
            // response plus its trailing StateChanged broadcast). HandleIncoming
            // only parses the first frame, so split into message-sized chunks.
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

static bool os_get(std::shared_ptr<IpcSocket>& s, XBusClient& x, OptStorageState& out) {
    bool got = false;
    x.Call("com.freebsdx.OptStorage",
           static_cast<uint32_t>(OptStorageOpcode::GetState), {},
           [&](const XBusMessage& r) {
               std::memcpy(&out, r.payload_, sizeof(out));
               got = true;
           });
    return drain_until(s, x, got, 2000);
}

static bool os_set(std::shared_ptr<IpcSocket>& s, XBusClient& x,
                   uint32_t enabled, uint32_t target_pct) {
    OptStorageRequest req{};
    req.enabled_ = enabled;
    req.target_pct_ = target_pct;
    bool ack = false;
    x.Call("com.freebsdx.OptStorage",
           static_cast<uint32_t>(OptStorageOpcode::SetState),
           {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
           [&](const XBusMessage&) { ack = true; });
    return drain_until(s, x, ack, 2000);
}

int main() {
    std::cout << "\033[1;36m=== OptStorage backend smoke test ===\033[0m\n";

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
        if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            connected = true;
            break;
        }
        ::close(sock);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!connected) {
        std::cout << "\033[1;31mFAIL\033[0m: no socket at " << path << "\n";
        return 1;
    }
    {
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
    auto cs = std::make_shared<IpcSocket>(UniqueFd(sock));
    XBusClient xbus(cs);

    // Capture StateChanged broadcasts.
    bool bcast_ok = false;
    OptStorageState bcast_state{};
    xbus.OnBroadcast([&](const XBusMessage& msg) {
        if (std::strcmp(msg.service_name_, "com.freebsdx.OptStorage") == 0 &&
            msg.opcode_ == static_cast<uint32_t>(OptStorageOpcode::StateChanged)) {
            std::memcpy(&bcast_state, msg.payload_, sizeof(bcast_state));
            bcast_ok = true;
        }
    });

    // 1. Initial GetState.
    OptStorageState initial{};
    if (!os_get(cs, xbus, initial)) {
        std::cout << "\033[1;31mFAIL\033[0m: GetState timed out (service unbacked?)\n";
        return 1;
    }
    std::cout << "  initial: enabled=" << initial.enabled_
              << " target_pct=" << initial.target_pct_
              << " reclaimableMB=" << initial.reclaimable_mb_ << "\n";

    // 2. Mutate to a distinctive value.
    const uint32_t want_enabled = 1;
    const uint32_t want_target = (initial.target_pct_ == 40) ? 25 : 40;
    if (!os_set(cs, xbus, want_enabled, want_target)) {
        std::cout << "\033[1;31mFAIL\033[0m: SetState not acked\n";
        return 1;
    }
    drain_until(cs, xbus, bcast_ok, 1000);

    // 3. Read back.
    OptStorageState after{};
    if (!os_get(cs, xbus, after)) {
        std::cout << "\033[1;31mFAIL\033[0m: read-back GetState timed out\n";
        return 1;
    }
    std::cout << "  after:   enabled=" << after.enabled_
              << " target_pct=" << after.target_pct_
              << " reclaimableMB=" << after.reclaimable_mb_ << "\n";

    bool persist_ok = (after.enabled_ == want_enabled && after.target_pct_ == want_target);
    bool broadcast_ok = (bcast_ok && bcast_state.enabled_ == want_enabled &&
                         bcast_state.target_pct_ == want_target);
    // reclaimable_mb_ is display-only: must be populated and survive a SetState.
    bool reclaim_ok = (after.reclaimable_mb_ != 0 &&
                       after.reclaimable_mb_ == initial.reclaimable_mb_);

    std::cout << "  persistence:         " << (persist_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m") << "\n";
    std::cout << "  broadcast echo:      " << (broadcast_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m")
              << " (enabled=" << bcast_state.enabled_ << " target_pct=" << bcast_state.target_pct_ << ")\n";
    std::cout << "  reclaimable_mb kept: " << (reclaim_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m")
              << " (" << after.reclaimable_mb_ << " MB)\n";

    // Restore original state.
    os_set(cs, xbus, initial.enabled_, initial.target_pct_);

    const bool all_ok = persist_ok && broadcast_ok && reclaim_ok;
    std::cout << (all_ok ? "\033[1;32m=== OptStorage smoke: PASS ===\033[0m\n"
                         : "\033[1;31m=== OptStorage smoke: FAIL ===\033[0m\n");
    return all_ok ? 0 : 1;
}
