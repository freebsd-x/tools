// Headless input-injection smoke test — prototype of the automated
// verification harness.
//
// Proves the full inject → process → observe loop against a running headless
// freebsdx, with no hardware and no live seat:
//   1. read the cursor position (Headless/GetPointer),
//   2. inject a relative pointer motion (Headless/InjectInput),
//   3. re-read the position and assert it moved by the injected delta,
//   4. inject a large motion and assert it clamps to the screen bounds.
//
// This is the foundation the richer harness builds on: once input can be driven
// and state observed headlessly, cursor-shape resolution, hit-testing, pointer
// capture, and widget interaction all become CI-testable (see GetInfo for the
// virtual output size; a windowed test client is the next increment).
//
// Exit 0 = PASS, 1 = FAIL.

#include <algorithm>
#include "xkit/xbus_client.h"
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <iostream>
#include <span>
#include <thread>
#include <chrono>
#include <cmath>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <cstring>
#include <unistd.h>

using namespace fbx::xkit;
using namespace fbx::xserver;
using namespace fbx::shared;

// The size the compositor gave our window (SurfaceReady). It picks the size
// itself — apps open at a fixed fraction of the output, whatever they ask for.
static float g_ready_w = 0.0f, g_ready_h = 0.0f;

static bool drain_until(std::shared_ptr<IpcSocket>& sock, XBusClient& xbus,
                        bool& flag, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto msg_res = sock->receiveMessage();
        if (msg_res) {
            const auto& data = msg_res.value().first;
            if (data.size() == sizeof(RenderMessage)) {
                const auto* rm = reinterpret_cast<const RenderMessage*>(data.data());
                if (rm->status_ == RenderMessageStatus::SurfaceReady) {
                    g_ready_w = static_cast<float>(rm->width_);
                    g_ready_h = static_cast<float>(rm->height_);
                }
                continue;
            }
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

static bool get_info(std::shared_ptr<IpcSocket>& s, XBusClient& x, HeadlessInfo& out) {
    bool got = false;
    x.Call("com.freebsdx.Headless", static_cast<uint32_t>(HeadlessOpcode::GetInfo), {},
           [&](const XBusMessage& r) { std::memcpy(&out, r.payload_, sizeof(out)); got = true; });
    return drain_until(s, x, got, 2000);
}

static bool get_pointer(std::shared_ptr<IpcSocket>& s, XBusClient& x, float& px, float& py) {
    HeadlessPointer p{};
    bool got = false;
    x.Call("com.freebsdx.Headless", static_cast<uint32_t>(HeadlessOpcode::GetPointer), {},
           [&](const XBusMessage& r) { std::memcpy(&p, r.payload_, sizeof(p)); got = true; });
    if (!drain_until(s, x, got, 2000)) return false;
    px = p.x_; py = p.y_;
    return true;
}

static void inject_move(XBusClient& x, int dx, int dy, uint32_t mods = 0) {
    InputMessage m{};
    m.type_      = InputEventType::MouseMove;
    m.value_a_   = dx;   // injected motion is relative (the drain loop accumulates it)
    m.value_b_   = dy;
    m.modifiers_ = mods; // Shift=1, Ctrl=2, Alt=4, Super=8
    x.Call("com.freebsdx.Headless", static_cast<uint32_t>(HeadlessOpcode::InjectInput),
           {reinterpret_cast<const uint8_t*>(&m), sizeof(m)}, nullptr);
}

// Begin a drag-and-drop with the given source surface id (any id that isn't the
// drop target works for driving the cursor logic).
static void start_drag(XBusClient& x, uint32_t source_id) {
    DnDStartRequest req{};
    req.surface_id_ = source_id;
    std::strncpy(req.mime_type_, "text/plain", sizeof(req.mime_type_) - 1);
    x.Call("com.freebsdx.DnD", static_cast<uint32_t>(DnDOpcode::StartDrag),
           {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
}

// Request a cursor shape the way an xkit client does: com.freebsdx.Input op 0x03.
static void set_cursor_shape(XBusClient& x, uint32_t shape) {
    x.Call("com.freebsdx.Input", 0x03,
           {reinterpret_cast<const uint8_t*>(&shape), sizeof(shape)}, nullptr);
}

// Register an app surface (RenderMessage handshake). The compositor places a new
// window at (50, 78) with the given size and acks with SurfaceReady.
static void create_window(std::shared_ptr<IpcSocket>& s, uint32_t id, uint32_t w, uint32_t h) {
    RenderMessage m{};
    m.surface_id_ = id;
    m.status_     = RenderMessageStatus::SurfaceCreated;
    m.width_      = w;
    m.height_     = h;
    (void)s->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

static bool get_cursor_shape(std::shared_ptr<IpcSocket>& s, XBusClient& x, uint32_t& out) {
    uint32_t sh = 0;
    bool got = false;
    x.Call("com.freebsdx.Headless", static_cast<uint32_t>(HeadlessOpcode::GetCursorShape), {},
           [&](const XBusMessage& r) { std::memcpy(&sh, r.payload_, sizeof(sh)); got = true; });
    if (!drain_until(s, x, got, 2000)) return false;
    out = sh;
    return true;
}

int main() {
    std::cout << "\033[1;36m=== Headless input-injection smoke test ===\033[0m\n";

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
    { int fl = fcntl(sock, F_GETFL, 0); fcntl(sock, F_SETFL, fl | O_NONBLOCK); }
    auto cs = std::make_shared<IpcSocket>(UniqueFd(sock));
    XBusClient xbus(cs);

    HeadlessInfo info{};
    if (!get_info(cs, xbus, info) || !info.headless_) {
        std::cout << "\033[1;31mFAIL\033[0m: not a headless compositor (run with --headless)\n";
        return 1;
    }
    std::cout << "  virtual output: " << info.width_ << "x" << info.height_ << "\n";

    // Give the injected input a few 16ms frames to be drained before re-reading.
    auto settle = [] { std::this_thread::sleep_for(std::chrono::milliseconds(120)); };
    auto approx = [](float a, float b) { return std::fabs(a - b) < 2.0f; };

    // 1. Relative motion moves the cursor by the injected delta.
    float x0, y0;
    if (!get_pointer(cs, xbus, x0, y0)) { std::cout << "\033[1;31mFAIL\033[0m: GetPointer timed out\n"; return 1; }
    inject_move(xbus, 100, 50);
    settle();
    float x1, y1;
    if (!get_pointer(cs, xbus, x1, y1)) { std::cout << "\033[1;31mFAIL\033[0m: GetPointer timed out\n"; return 1; }
    bool move_ok = approx(x1, x0 + 100.0f) && approx(y1, y0 + 50.0f);
    std::cout << "  motion:  (" << x0 << "," << y0 << ") +[100,50] -> (" << x1 << "," << y1 << ")  "
              << (move_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m") << "\n";

    // 2. A large motion clamps to the screen bounds.
    inject_move(xbus, 100000, 100000);
    settle();
    float x2, y2;
    if (!get_pointer(cs, xbus, x2, y2)) { std::cout << "\033[1;31mFAIL\033[0m: GetPointer timed out\n"; return 1; }
    bool clamp_ok = approx(x2, static_cast<float>(info.width_)) &&
                    approx(y2, static_cast<float>(info.height_));
    std::cout << "  clamp:   +[big] -> (" << x2 << "," << y2 << ")  expected ("
              << info.width_ << "," << info.height_ << ")  "
              << (clamp_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m") << "\n";

    // 3. A client cursor-shape request is recorded and observable (the exact
    //    com.freebsdx.Input path xkit uses), for several shapes across the enum.
    struct { uint32_t shape; const char* name; } cases[] = {
        {9, "text"}, {17, "grabbing"}, {23, "se_resize"}, {1, "default"},
    };
    bool shape_ok = true;
    for (auto& c : cases) {
        set_cursor_shape(xbus, c.shape);
        settle();
        uint32_t got = 0;
        if (!get_cursor_shape(cs, xbus, got)) {
            std::cout << "\033[1;31mFAIL\033[0m: GetCursorShape timed out\n"; return 1;
        }
        bool ok = (got == c.shape);
        shape_ok = shape_ok && ok;
        std::cout << "  shape:   set " << c.name << "(" << c.shape << ") -> " << got << "  "
                  << (ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m") << "\n";
    }

    // 4. Injected motion runs the SAME server-side hover arbitration as real
    //    input (input-path unification): a server-owned cursor (17..31) with no
    //    edge/DnD under the pointer is released to the arrow on the next motion.
    //    Before unification, injected motion bypassed this and the shape stuck.
    set_cursor_shape(xbus, 23);  // se_resize (in the server-owned range)
    settle();
    uint32_t before = 0; get_cursor_shape(cs, xbus, before);
    inject_move(xbus, 3, 0);
    settle();
    uint32_t after = 0; get_cursor_shape(cs, xbus, after);
    bool arb_ok = (before == 23) && (after == 1);
    std::cout << "  arbitration: se_resize(23) then move -> " << after
              << "  (released to arrow)  "
              << (arb_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m") << "\n";

    // 4b. Stuck-cursor regression: a client cursor in the 2..11 range (here an
    //     I-beam) left set over the BARE desktop must revert to the arrow. The
    //     server used to only release the 12..14 / 17..31 ranges on motion, so
    //     an app's I-beam/hand/wait cursor stayed stuck on the empty desktop.
    //     Runs before any window exists, so bodyHitTest finds no surface under
    //     the pointer. (settle() only drains — it never moves — so the shape the
    //     client set survives until the next inject_move.)
    inject_move(xbus, 600, 400);  // settle over the bare desktop, clear of surfaces
    settle();
    inject_move(xbus, 4, 4);
    settle();
    set_cursor_shape(xbus, 9);    // client sets text/I-beam, bypassing arbitration
    settle();
    uint32_t stuck_before = 0; get_cursor_shape(cs, xbus, stuck_before);
    inject_move(xbus, 3, 3);      // move again, still on the bare desktop (no crossing)
    settle();
    uint32_t stuck_after = 0; get_cursor_shape(cs, xbus, stuck_after);
    bool stuck_ok = (stuck_before == 9) && (stuck_after == 1);
    std::cout << "  desktop revert: text(9) then move on bare desktop -> " << stuck_after
              << "  (reverts to arrow)  "
              << (stuck_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m") << "\n";

    // 5. Windowed server-side hover: create a window and move the cursor onto
    //    its bottom resize edge; the unified injected-input path must drive the
    //    s_resize cursor (22). This exercises the full loop — surface
    //    registration → injected hover → resizeHitTest → arbitration —
    //    headless, the increment the input-path unification unblocked.
    //    The compositor sizes app windows itself (reported in SurfaceReady) and
    //    opens them centred (WindowStack::addWindow: x = max(24, (W-w)/2),
    //    y = max(48, (H-h)/2)). The test used to assume its requested 400x300
    //    at the old top-left spot (50,78) and missed the window's edges.
    create_window(cs, /*id=*/1, 400, 300);
    settle();
    float cx, cy;   // get_pointer drains the socket, which also reads SurfaceReady
    if (!get_pointer(cs, xbus, cx, cy)) { std::cout << "\033[1;31mFAIL\033[0m: GetPointer timed out\n"; return 1; }
    if (g_ready_w <= 0.0f) { std::cout << "\033[1;31mFAIL\033[0m: no SurfaceReady for the window\n"; return 1; }
    const float kWinW = g_ready_w, kWinH = g_ready_h;
    const float win_x = std::max(24.0f, (static_cast<float>(info.width_)  - kWinW) * 0.5f);
    const float win_y = std::max(48.0f, (static_cast<float>(info.height_) - kWinH) * 0.5f);
    // The bottom edge, at an x inside the window.
    inject_move(xbus, static_cast<int>(win_x + kWinW / 2 - cx), static_cast<int>(win_y + kWinH - cy));
    settle();
    uint32_t edge_shape = 0;
    if (!get_cursor_shape(cs, xbus, edge_shape)) { std::cout << "\033[1;31mFAIL\033[0m: GetCursorShape timed out\n"; return 1; }
    bool hover_ok = (edge_shape == 22);  // s_resize
    std::cout << "  window hover: bottom edge -> " << edge_shape
              << " (expect 22 s_resize)  "
              << (hover_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m") << "\n";

    // 6. Drag-and-drop cursors reflect the operation: over a valid target →
    //    move (or copy with Ctrl); off any target → no_drop. Reuses window 1
    //    (created above, centred) as the drop target.
    start_drag(xbus, /*source=*/999);  // 999 != target id 1
    settle();
    { float x, y; get_pointer(cs, xbus, x, y);
      inject_move(xbus, static_cast<int>(win_x + kWinW / 2 - x), static_cast<int>(win_y + kWinH / 2 - y), 0); }
    settle();
    uint32_t s_move = 0; get_cursor_shape(cs, xbus, s_move);
    inject_move(xbus, 0, 0, /*Ctrl=*/2);  // same spot, Ctrl held
    settle();
    uint32_t s_copy = 0; get_cursor_shape(cs, xbus, s_copy);
    { float x, y; get_pointer(cs, xbus, x, y);
      // A bare-desktop corner well clear of the centred window.
      inject_move(xbus, static_cast<int>(10.0f - x), static_cast<int>(static_cast<float>(info.height_) - 10.0f - y), 0); }
    settle();
    uint32_t s_nodrop = 0; get_cursor_shape(cs, xbus, s_nodrop);
    bool dnd_ok = (s_move == 13) && (s_copy == 12) && (s_nodrop == 14);
    std::cout << "  dnd cursors:  over-target=" << s_move << " (move 13), +Ctrl="
              << s_copy << " (copy 12), off-target=" << s_nodrop << " (no_drop 14)  "
              << (dnd_ok ? "\033[1;32mPASS\033[0m" : "\033[1;31mFAIL\033[0m") << "\n";

    const bool all_ok = move_ok && clamp_ok && shape_ok && arb_ok && stuck_ok && hover_ok && dnd_ok;
    std::cout << (all_ok ? "\033[1;32m=== headless input smoke: PASS ===\033[0m\n"
                         : "\033[1;31m=== headless input smoke: FAIL ===\033[0m\n");
    return all_ok ? 0 : 1;
}
