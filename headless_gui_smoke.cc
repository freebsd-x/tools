// Headless GUI-automation smoke test.
//
// Proves the compositor's *rendered output* can be asserted with no display and
// no seat: create a solid-colour window, capture the composited frame via
// Headless/CaptureFrame (shared-memory XRGB8888), and check the window's pixels
// are the expected colour while the background isn't. This closes the "I can't
// see the screen" gap for everything composited (windows, widgets, app content).
//
// Modelled on headless_input_smoke: single-threaded, XBusClient for XBus calls
// (which stamp the XBUS magic + transaction id the dispatcher requires), plus a
// fd-preserving drain for the CaptureFrame reply (XBusClient::HandleIncoming
// drops the shm fd, so the frame reply is read directly).
//
// Exit 0 = PASS, 1 = FAIL.

#include <algorithm>
#include "xkit/xbus_client.h"
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>

using namespace fbx::xserver;
using namespace fbx::shared;
using fbx::xkit::XBusClient;

static void nap(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Drain inbound messages for `ms`, feeding XBus replies to the client (so acks
// like SurfaceReady are consumed) and catching the CaptureFrame reply's shm fd
// (which XBusClient::HandleIncoming would otherwise drop). Returns when the fd
// arrives or the window elapses.
// The size the compositor gave the window (SurfaceReady): it sizes app windows
// itself and opens them centred for that size.
static uint32_t g_ready_w = 0, g_ready_h = 0;

static bool drain(std::shared_ptr<IpcSocket>& sock, XBusClient& xbus, int ms,
                  ScreencopyFrameInfo* out_info, UniqueFd* out_fd) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto r = sock->receiveMessage();
        if (r) {
            auto& [data, fd] = r.value();
            if (data.size() == sizeof(RenderMessage)) {
                const auto* rm = reinterpret_cast<const RenderMessage*>(data.data());
                if (rm->status_ == RenderMessageStatus::SurfaceReady) {
                    g_ready_w = rm->width_;
                    g_ready_h = rm->height_;
                }
                continue;
            }
            constexpr size_t kFrame = sizeof(XBusMessage);
            if (out_info && fd.isValid() && data.size() >= kFrame) {
                const auto* xm = reinterpret_cast<const XBusMessage*>(data.data());
                if (std::strcmp(xm->service_name_, "com.freebsdx.Headless") == 0 &&
                    xm->opcode_ == static_cast<uint32_t>(HeadlessOpcode::CaptureFrame)) {
                    std::memcpy(out_info, xm->payload_, sizeof(*out_info));
                    *out_fd = std::move(fd);
                    return true;
                }
            }
            for (size_t off = 0; off + kFrame <= data.size(); off += kFrame)
                xbus.HandleIncoming(std::span<const uint8_t>(data.data() + off, kFrame));
        }
        nap(3);
    }
    return false;
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    std::cout << "\033[1;36m=== Headless GUI-automation smoke test ===\033[0m\n";

    const char* path = "/tmp/freebsdx.sock";
    int sock = -1; bool connected = false;
    for (int i = 0; i < 20 && !connected; ++i) {
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
        nap(500);
    }
    if (!connected) { std::cout << "\033[1;31mFAIL\033[0m: no socket at " << path << "\n"; return 1; }
    { int fl = fcntl(sock, F_GETFL, 0); fcntl(sock, F_SETFL, fl | O_NONBLOCK); }
    auto cs = std::make_shared<IpcSocket>(UniqueFd(sock));
    XBusClient xbus(cs);

    constexpr uint32_t kId = 1, kW = 400, kH = 300;
    constexpr uint32_t kRed = 0xFFFF0000;  // 0xAARRGGBB

    // Handshake: an XBus round-trip establishes the connection the same way the
    // proven input smoke does before driving surfaces.
    HeadlessInfo hinfo{};
    { bool got = false;
      xbus.Call("com.freebsdx.Headless", static_cast<uint32_t>(HeadlessOpcode::GetInfo), {},
                [&](const XBusMessage& r){ std::memcpy(&hinfo, r.payload_, sizeof(hinfo)); got = true; });
      drain(cs, xbus, 2000, nullptr, nullptr);
      if (!got) { std::cout << "\033[1;31mFAIL\033[0m: no GetInfo reply (compositor not ready)\n"; return 1; }
    }

    // Create the window; drain so the compositor processes it (and its
    // SurfaceReady ack) as its own message, then paint it red.
    RenderMessage create{};
    create.surface_id_ = kId; create.status_ = RenderMessageStatus::SurfaceCreated;
    create.width_ = kW; create.height_ = kH;
    (void)cs->sendMessage({reinterpret_cast<const uint8_t*>(&create), sizeof(create)});
    drain(cs, xbus, 400, nullptr, nullptr);
    if (g_ready_w == 0) { std::cout << "\033[1;31mFAIL\033[0m: no SurfaceReady for the window\n"; return 1; }
    // Placed centred for the compositor's size (WindowStack::addWindow); the
    // red kW x kH content then shrinks into place from that top-left corner.
    const uint32_t win_x = static_cast<uint32_t>(std::max(24.0f, (static_cast<float>(hinfo.width_)  - g_ready_w) * 0.5f));
    const uint32_t win_y = static_cast<uint32_t>(std::max(48.0f, (static_cast<float>(hinfo.height_) - g_ready_h) * 0.5f));
    const uint32_t in_x = win_x + kW / 2, in_y = win_y + kH / 2;

    RenderMessage solid{};
    solid.surface_id_ = kId; solid.status_ = RenderMessageStatus::SolidColor;
    solid.stride_ = kRed;
    solid.width_ = kW; solid.height_ = kH;  // updateSurface sizes the surface from these
    (void)cs->sendMessage({reinterpret_cast<const uint8_t*>(&solid), sizeof(solid)});
    drain(cs, xbus, 800, nullptr, nullptr);  // let it composite (opacity animates up)

    // Capture the composited frame via XBus, keeping the shm fd.
    ScreencopyFrameInfo info{};
    UniqueFd shm;
    xbus.Call("com.freebsdx.Headless", static_cast<uint32_t>(HeadlessOpcode::CaptureFrame),
              {}, nullptr);
    if (!drain(cs, xbus, 3000, &info, &shm) || !shm.isValid() || info.width_ == 0) {
        std::cout << "\033[1;31mFAIL\033[0m: no captured frame received\n"; return 1;
    }
    std::cout << "  captured frame: " << info.width_ << "x" << info.height_
              << " stride=" << info.stride_ << " format=" << info.format_ << "\n";

    size_t map_len = static_cast<size_t>(info.height_) * info.stride_;
    void* map = mmap(nullptr, map_len, PROT_READ, MAP_SHARED, shm.get(), 0);
    if (map == MAP_FAILED) { std::cout << "\033[1;31mFAIL\033[0m: mmap failed\n"; return 1; }
    auto pixel = [&](uint32_t x, uint32_t y) -> uint32_t {
        const uint8_t* row = static_cast<const uint8_t*>(map) + static_cast<size_t>(y) * info.stride_;
        uint32_t p; std::memcpy(&p, row + x * 4, 4); return p;
    };
    auto is_red = [](uint32_t p) {
        uint8_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        return r > 200 && g < 60 && b < 60;
    };

    uint32_t inside  = pixel(in_x, in_y);              // the middle of the red window
    uint32_t outside = pixel(10, info.height_ - 120);  // bare desktop, clear of window and taskbar
    bool in_ok  = is_red(inside);
    bool out_ok = !is_red(outside);
    munmap(map, map_len);

    std::cout << "  window pixel (" << in_x << "," << in_y << ") = 0x" << std::hex << (inside & 0xFFFFFF)
              << (in_ok ? "  \033[1;32mred PASS\033[0m" : "  \033[1;31mnot red FAIL\033[0m")
              << std::dec << "\n";
    std::cout << "  background (10," << info.height_ - 120 << ") = 0x" << std::hex << (outside & 0xFFFFFF)
              << (out_ok ? "  \033[1;32mnot red PASS\033[0m" : "  \033[1;31mred FAIL\033[0m")
              << std::dec << "\n";

    bool ok = in_ok && out_ok;
    std::cout << (ok ? "\033[1;32m=== gui smoke: PASS ===\033[0m\n"
                     : "\033[1;31m=== gui smoke: FAIL ===\033[0m\n");
    return ok ? 0 : 1;
}
