// Headless widget-rendering smoke test.
//
// The gui smoke proves a raw SolidColor surface composites; this proves the
// real xkit -> xrender pipeline: a forked child runs an actual XApplication
// with a widget tree (a green ContainerWidget stacked above a blue one in a
// FlexColumn), and the parent captures the composited frame and asserts both
// widget colours rendered *and* that green sits above blue — i.e. widgets
// rasterised and column layout ran. No display, no seat.
//
// Exit 0 = PASS, 1 = FAIL.

#include "xkit/widget.h"
#include "xkit/application.h"
#include "xkit/xbus_client.h"
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <memory>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>

using namespace fbx::xkit;
using namespace fbx::xserver;
using namespace fbx::shared;

static void nap(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ── The widget app (child process) ─────────────────────────────────────────────
// A green box stacked above a blue box: exercises real widget rasterisation and
// FlexColumn layout. Blocks in the event loop until the parent kills it.
[[noreturn]] static void run_widget_app() {
    auto green = std::make_shared<ContainerWidget>(320.0f, 140.0f);
    green->setBackgroundColor(Color{0.10f, 0.80f, 0.20f, 1.0f});
    auto blue = std::make_shared<ContainerWidget>(320.0f, 140.0f);
    blue->setBackgroundColor(Color{0.10f, 0.20f, 0.85f, 1.0f});
    auto root = std::make_shared<FlexColumn>(
        std::vector<std::shared_ptr<Widget>>{green, blue});
    XApplication::Run(root, "/tmp/freebsdx.sock", false, false, nullptr);
    _exit(0);
}

// ── Frame capture (parent process) ─────────────────────────────────────────────
static bool capture_frame(std::shared_ptr<IpcSocket>& sock, XBusClient& xbus,
                          ScreencopyFrameInfo& info, UniqueFd& shm) {
    xbus.Call("com.freebsdx.Headless", static_cast<uint32_t>(HeadlessOpcode::CaptureFrame),
              {}, nullptr);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        auto r = sock->receiveMessage();
        if (r) {
            auto& [data, fd] = r.value();
            constexpr size_t kFrame = sizeof(XBusMessage);
            if (fd.isValid() && data.size() >= kFrame) {
                const auto* xm = reinterpret_cast<const XBusMessage*>(data.data());
                if (std::strcmp(xm->service_name_, "com.freebsdx.Headless") == 0 &&
                    xm->opcode_ == static_cast<uint32_t>(HeadlessOpcode::CaptureFrame)) {
                    std::memcpy(&info, xm->payload_, sizeof(info));
                    shm = std::move(fd);
                    return info.width_ != 0 && shm.isValid();
                }
            }
            for (size_t off = 0; off + kFrame <= data.size(); off += kFrame)
                xbus.HandleIncoming(std::span<const uint8_t>(data.data() + off, kFrame));
        }
        nap(3);
    }
    return false;
}

// Scan the whole frame: count green/blue widget pixels and their mean Y.
struct Scan { uint64_t green = 0, blue = 0; double green_y = 0, blue_y = 0; };
static Scan scan_frame(const void* map, const ScreencopyFrameInfo& info) {
    Scan s;
    for (uint32_t y = 0; y < info.height_; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(map) + static_cast<size_t>(y) * info.stride_;
        for (uint32_t x = 0; x < info.width_; ++x) {
            uint32_t p; std::memcpy(&p, row + x * 4, 4);
            uint8_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            if (g > 150 && r < 110 && b < 110) { s.green++; s.green_y += y; }
            else if (b > 150 && r < 110 && g < 130) { s.blue++; s.blue_y += y; }
        }
    }
    if (s.green) s.green_y /= s.green;
    if (s.blue)  s.blue_y  /= s.blue;
    return s;
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    std::cout << "\033[1;36m=== Headless widget-rendering smoke test ===\033[0m\n" << std::flush;

    // Fork the widget app before the parent touches any Vulkan/socket state.
    // (stdout is flushed above so the child doesn't inherit a dirty buffer.)
    pid_t child = fork();
    if (child == 0) run_widget_app();
    if (child < 0) { std::cout << "\033[1;31mFAIL\033[0m: fork failed\n"; return 1; }

    auto fail = [&](const char* why) -> int {
        std::cout << "\033[1;31mFAIL\033[0m: " << why << "\n";
        kill(child, SIGTERM); waitpid(child, nullptr, 0);
        return 1;
    };

    // Parent: connect to the compositor.
    int sock = -1; bool connected = false;
    for (int i = 0; i < 30 && !connected; ++i) {
        sock = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (sock < 0) break;
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, "/tmp/freebsdx.sock", sizeof(addr.sun_path) - 1);
        if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) { connected = true; break; }
        ::close(sock);
        nap(300);
    }
    if (!connected) return fail("no compositor socket");
    { int fl = fcntl(sock, F_GETFL, 0); fcntl(sock, F_SETFL, fl | O_NONBLOCK); }
    auto cs = std::make_shared<IpcSocket>(UniqueFd(sock));
    XBusClient xbus(cs);

    // Poll: the app needs time to init Vulkan+fonts, connect, mount and render
    // (opacity also animates up). Capture until both widget colours appear.
    Scan best;
    ScreencopyFrameInfo info{};
    bool found = false;
    auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < give_up && !found) {
        nap(400);
        UniqueFd shm;
        if (!capture_frame(cs, xbus, info, shm)) continue;
        size_t map_len = static_cast<size_t>(info.height_) * info.stride_;
        void* map = mmap(nullptr, map_len, PROT_READ, MAP_SHARED, shm.get(), 0);
        if (map == MAP_FAILED) continue;
        Scan s = scan_frame(map, info);
        munmap(map, map_len);
        if (s.green > best.green) best = s;  // keep the strongest observation
        // Require a solid presence of both colours (not stray AA pixels).
        if (s.green > 2000 && s.blue > 2000) { best = s; found = true; }
    }

    std::cout << "  green px=" << best.green << " meanY=" << best.green_y
              << " | blue px=" << best.blue << " meanY=" << best.blue_y << "\n";

    if (best.green < 2000) return fail("green widget not rendered");
    if (best.blue  < 2000) return fail("blue widget not rendered");
    if (!(best.green_y < best.blue_y))
        return fail("layout wrong: green not above blue");

    std::cout << "  \033[1;32mgreen + blue widgets rendered; green above blue (column layout)\033[0m\n";
    std::cout << "\033[1;32m=== widget smoke: PASS ===\033[0m\n";
    kill(child, SIGTERM); waitpid(child, nullptr, 0);
    return 0;
}
