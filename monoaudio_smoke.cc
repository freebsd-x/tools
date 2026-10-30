// MonoAudio backend smoke test.
//
// Exercises com.freebsdx.MonoAudio end-to-end against a running freebsdx
// compositor: GetState → SetState → verify the StateChanged broadcast fires
// AND a follow-up GetState reflects the mutation. A hollow (unbacked) service
// cannot pass this — it either never responds or fails to persist state.
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
            // A single recv can coalesce several 256-byte XBusMessages (e.g. a
            // SetState response immediately followed by its StateChanged
            // broadcast). HandleIncoming only parses the first frame, so split
            // the buffer into message-sized chunks and feed each one.
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

static bool ma_get(std::shared_ptr<IpcSocket>& s, XBusClient& x, MonoAudioState& out) {
    bool got = false;
    x.Call("com.freebsdx.MonoAudio",
           static_cast<uint32_t>(MonoAudioOpcode::GetState), {},
           [&](const XBusMessage& r) {
               std::memcpy(&out, r.payload_, sizeof(out));
               got = true;
           });
    return drain_until(s, x, got, 2000);
}

static bool ma_set(std::shared_ptr<IpcSocket>& s, XBusClient& x,
                   uint32_t enabled, uint32_t balance) {
    MonoAudioRequest req{};
    req.enabled_ = enabled;
    req.balance_ = balance;
    bool ack = false;
    x.Call("com.freebsdx.MonoAudio",
           static_cast<uint32_t>(MonoAudioOpcode::SetState),
           {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
           [&](const XBusMessage&) { ack = true; });
    return drain_until(s, x, ack, 2000);
}

int main() {
    std::cout << "\033[1;36m=== MonoAudio backend smoke test ===\033[0m\n";

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

    // No surface handshake: xbus RPC is dispatched by service name and does not
    // require a registered render surface. (Headless mode bypasses the
    // presentation path that would ack SurfaceReady, so a handshake here would
    // hang.) We go straight to the MonoAudio round-trip.

    // Capture StateChanged broadcasts.
    bool bcast_ok = false;
    MonoAudioState bcast_state{};
    xbus.OnBroadcast([&](const XBusMessage& msg) {
        if (std::strcmp(msg.service_name_, "com.freebsdx.MonoAudio") == 0 &&
            msg.opcode_ == static_cast<uint32_t>(MonoAudioOpcode::StateChanged)) {
            std::memcpy(&bcast_state, msg.payload_, sizeof(bcast_state));
            bcast_ok = true;
        }
    });

    // 1. Initial GetState.
    MonoAudioState initial{};
    if (!ma_get(cs, xbus, initial)) {
        std::cout << "\033[1;31mFAIL\033[0m: GetState timed out (service unbacked?)\n";
        return 1;
    }
    std::cout << "  initial: enabled=" << initial.enabled_
              << " balance=" << initial.balance_ << "\n";

    // 2. Mutate to a distinctive value.
    const uint32_t want_enabled = 1;
    const uint32_t want_balance = (initial.balance_ == 25) ? 75 : 25;
    if (!ma_set(cs, xbus, want_enabled, want_balance)) {
        std::cout << "\033[1;31mFAIL\033[0m: SetState not acked\n";
        return 1;
    }
    // Broadcast may have arrived during the ack drain; give it a moment more.
    drain_until(cs, xbus, bcast_ok, 1000);

    // 3. Read back.
    MonoAudioState after{};
    if (!ma_get(cs, xbus, after)) {
        std::cout << "\033[1;31mFAIL\033[0m: read-back GetState timed out\n";
        return 1;
    }
    std::cout << "  after:   enabled=" << after.enabled_
              << " balance=" << after.balance_ << "\n";

    bool persist_ok = (after.enabled_ == want_enabled && after.balance_ == want_balance);
    bool broadcast_ok = (bcast_ok && bcast_state.enabled_ == want_enabled &&
                         bcast_state.balance_ == want_balance);

    std::cout << "  persistence:    " << (persist_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m") << "\n";
    std::cout << "  broadcast echo: " << (broadcast_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m")
              << " (enabled=" << bcast_state.enabled_ << " balance=" << bcast_state.balance_ << ")\n";

    // Restore original state.
    ma_set(cs, xbus, initial.enabled_, initial.balance_);

    const bool all_ok = persist_ok && broadcast_ok;
    std::cout << (all_ok ? "\033[1;32m=== MonoAudio smoke: PASS ===\033[0m\n"
                         : "\033[1;31m=== MonoAudio smoke: FAIL ===\033[0m\n");
    return all_ok ? 0 : 1;
}
