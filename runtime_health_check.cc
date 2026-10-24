#include "xkit/xbus_client.h"
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <iostream>
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

// Drain incoming IPC messages until predicate returns true or timeout expires.
// The socket must be in non-blocking mode before calling this.
static bool drain_until(std::shared_ptr<IpcSocket>& sock, XBusClient& xbus,
                        bool& flag, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto msg_res = sock->receiveMessage();
        if (msg_res) {
            xbus.HandleIncoming(msg_res.value().first);
        }
        if (flag) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

int main() {
    std::cout << "\033[1;36m====================================================\n";
    std::cout << " FreeBSD-X 1.0 RUNTIME HEALTH DIAGNOSTICS\n";
    std::cout << "====================================================\033[0m\n\n";

    const char* path = "/tmp/freebsdx.sock";
    int sock = -1;
    bool connected = false;

    // 1. Connection with retry
    std::cout << "[1/6] Connecting to Master Server... ";
    std::cout.flush();
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
        std::cout << "\033[1;31mFAILED\033[0m (Socket not available at " << path << ")\n";
        return 1;
    }
    std::cout << "\033[1;32mOK\033[0m\n";

    // Switch to non-blocking so drain loops don't hang if server is slow
    {
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    auto client_socket = std::make_shared<IpcSocket>(UniqueFd(sock));
    XBusClient xbus(client_socket);

    // 2. Surface handshake
    std::cout << "[2/6] IPC Handshake (Surface Registration)... ";
    std::cout.flush();
    RenderMessage handshake{};
    handshake.surface_id_ = 8888;
    handshake.status_     = RenderMessageStatus::SurfaceCreated;
    handshake.width_      = 100;
    handshake.height_     = 100;
    handshake.stride_     = 400;
    (void)client_socket->sendMessage(
        {reinterpret_cast<const uint8_t*>(&handshake), sizeof(handshake)});

    bool ready = false;
    for (int i = 0; i < 100; ++i) {
        auto msg_res = client_socket->receiveMessage();
        if (msg_res) {
            auto& data = msg_res.value().first;
            if (data.size() >= sizeof(RenderMessage)) {
                const auto* res = reinterpret_cast<const RenderMessage*>(data.data());
                if (res->status_ == RenderMessageStatus::SurfaceReady) {
                    ready = true;
                    break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!ready) {
        std::cout << "\033[1;31mTIMEOUT\033[0m\n";
        return 1;
    }
    std::cout << "\033[1;32mOK\033[0m\n";

    // 3. Ping — round-trip latency
    std::cout << "[3/6] XBus Ping (latency)... ";
    std::cout.flush();
    bool ping_ok = false;
    auto t0 = std::chrono::high_resolution_clock::now();
    xbus.Call("com.freebsdx.Ping", 0x01, {}, [&](const XBusMessage& r) {
        if (std::strcmp(reinterpret_cast<const char*>(r.payload_), "PONG") == 0)
            ping_ok = true;
    });
    drain_until(client_socket, xbus, ping_ok, 2000);
    auto ping_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - t0).count();

    if (ping_ok)
        std::cout << "\033[1;32mPASSED\033[0m (" << ping_us << " µs RTT)\n";
    else
        std::cout << "\033[1;31mFAILED\033[0m (no PONG within 2 s)\n";

    // 4. System telemetry
    std::cout << "[4/6] Telemetry (SystemInfo)... ";
    std::cout.flush();
    bool sys_ok = false;
    xbus.Call("com.freebsdx.System", 0x01, {}, [&](const XBusMessage& r) {
        SystemInfo stats{};
        std::memcpy(&stats, r.payload_, sizeof(stats));
        std::cout << "\033[1;32mOK\033[0m  CPU: " << stats.cpu_usage_
                  << "%  MEM: " << stats.mem_used_mb_ << "/"
                  << stats.mem_total_mb_ << " MB  Frames: "
                  << stats.frame_count_ << "\n";
        sys_ok = true;
    });
    drain_until(client_socket, xbus, sys_ok, 2000);
    if (!sys_ok)
        std::cout << "\033[1;31mFAILED\033[0m (no response from System broker)\n";

    // 5. Notification round-trip (Post → receive ID → Dismiss)
    std::cout << "[5/6] Notification round-trip... ";
    std::cout.flush();
    bool notif_ok = false;
    uint32_t notif_id = 0;

    NotificationPayload np{};
    std::strncpy(np.title_,   "Health Check",        sizeof(np.title_) - 1);
    std::strncpy(np.message_, "Automated test ping", sizeof(np.message_) - 1);
    np.timeout_ms_ = 2000;

    xbus.Call("com.freebsdx.Notification",
              static_cast<uint32_t>(NotificationOpcode::Post),
              {reinterpret_cast<const uint8_t*>(&np), sizeof(np)},
              [&](const XBusMessage& r) {
                  std::memcpy(&notif_id, r.payload_, sizeof(notif_id));
                  notif_ok = (notif_id != 0);
              });
    drain_until(client_socket, xbus, notif_ok, 2000);

    if (notif_ok) {
        // Dismiss it immediately so the shell UI isn't cluttered
        xbus.Call("com.freebsdx.Notification",
                  static_cast<uint32_t>(NotificationOpcode::Dismiss),
                  {reinterpret_cast<const uint8_t*>(&notif_id), sizeof(notif_id)},
                  [](const XBusMessage&) {});
        bool dismissed = true; // fire-and-forget; server handles it
        drain_until(client_socket, xbus, dismissed, 500);
        std::cout << "\033[1;32mPASSED\033[0m (ID " << notif_id
                  << " posted and dismissed)\n";
    } else {
        std::cout << "\033[1;31mFAILED\033[0m (no ID assigned by Notification broker)\n";
    }

    // 6. Visual rendering check — compositor frame checksum
    std::cout << "[6/6] Vulkan Graphics Pipeline... ";
    std::cout.flush();
    bool visual_ok = false;
    xbus.Call("com.freebsdx.Automation", 0x03, {}, [&](const XBusMessage& r) {
        uint32_t checksum = 0;
        std::memcpy(&checksum, r.payload_, sizeof(checksum));
        if (checksum != 0) {
            std::cout << "\033[1;32mRENDERING\033[0m (frame checksum: 0x"
                      << std::hex << checksum << std::dec << ")\n";
            visual_ok = true;
        } else {
            std::cout << "\033[1;31mSTALLED\033[0m (zero checksum — black screen?)\n";
        }
    });
    if (!drain_until(client_socket, xbus, visual_ok, 4000)) {
        // Callback never fired at all — broker unresponsive
        std::cout << "\033[1;31mTIMEOUT\033[0m (Automation broker did not respond)\n";
    }
    // If callback fired but checksum was zero, message was already printed inside callback

    // Tear down the test surface
    RenderMessage close_msg{};
    close_msg.surface_id_ = 8888;
    close_msg.status_     = RenderMessageStatus::SurfaceClosed;
    (void)client_socket->sendMessage(
        {reinterpret_cast<const uint8_t*>(&close_msg), sizeof(close_msg)});

    // Summary
    const bool all_ok = ping_ok && sys_ok && notif_ok && visual_ok;
    std::cout << "\n\033[1;36m====================================================\033[0m\n";
    if (all_ok) {
        std::cout << "\033[1;32m SYSTEM VALIDATION SUCCESSFUL — OS IS HEALTHY\033[0m\n";
    } else {
        std::cout << "\033[1;31m SYSTEM VALIDATION FAILED\033[0m\n";
        if (!ping_ok)   std::cout << "   - XBus broker unresponsive\n";
        if (!sys_ok)    std::cout << "   - System telemetry unavailable\n";
        if (!notif_ok)  std::cout << "   - Notification broker broken\n";
        if (!visual_ok) std::cout << "   - GPU pipeline not rendering\n";
    }
    std::cout << "\033[1;36m====================================================\033[0m\n";

    return all_ok ? 0 : 1;
}
