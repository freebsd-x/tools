// Headless frame grabber: connect to a running (headless) compositor over its
// SOCK_SEQPACKET master socket, request com.freebsdx.Headless/CaptureFrame, receive
// the composited frame as a shared-memory fd (SCM_RIGHTS) + ScreencopyFrameInfo, and
// write it out as a binary PPM (P6) to argv[1]. Captures whatever the compositor is
// currently showing (greeter, shell, app clients) — a real headless "screenshot".
// Convert to PNG with `pnmtopng` afterwards.
//
//   headless_capture OUT.ppm
//
// Deliberately raw (no XBusClient/IpcSocket): a blocking SEQPACKET round-trip, which
// is what the compositor's one-message-per-datagram transport wants. An earlier
// XBusClient + non-blocking-drain version silently dropped the CaptureFrame reply's
// SCM_RIGHTS fd — this avoids that path entirely.
//
// Exit 0 = captured + written, non-zero = failure.

#include "shared/protocol.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include <unistd.h>

using fbx::shared::XBusMessage;
using fbx::shared::ScreencopyFrameInfo;
using fbx::shared::HeadlessOpcode;

static int connect_seqpacket(const char* path) {
    int fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { ::close(fd); return -1; }
    return fd;
}

int main(int argc, char** argv) {
    const char* out_path = argc > 1 ? argv[1] : "/tmp/headless_frame.ppm";
    const char* sock_path = "/tmp/freebsdx.sock";

    int fd = connect_seqpacket(sock_path);
    if (fd < 0) { std::fprintf(stderr, "FAIL: cannot connect %s\n", sock_path); return 1; }

    // Request CaptureFrame (XBusMessage defaults magic_ = XBUS_MAGIC).
    XBusMessage req{};
    req.transaction_id_ = 1;
    std::strncpy(req.service_name_, "com.freebsdx.Headless", sizeof(req.service_name_) - 1);
    req.opcode_ = static_cast<uint32_t>(HeadlessOpcode::CaptureFrame);
    if (::send(fd, &req, sizeof(req), MSG_EOR) != static_cast<ssize_t>(sizeof(req))) {
        std::fprintf(stderr, "FAIL: send CaptureFrame\n"); return 1;
    }

    // Receive the reply datagram + its SCM_RIGHTS fd.
    XBusMessage rep{};
    struct iovec io{ &rep, sizeof(rep) };
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } u{};
    struct msghdr m{};
    m.msg_iov = &io; m.msg_iovlen = 1;
    m.msg_control = u.buf; m.msg_controllen = sizeof(u.buf);
    struct timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = ::recvmsg(fd, &m, MSG_CMSG_CLOEXEC);
    if (n != static_cast<ssize_t>(sizeof(rep))) {
        std::fprintf(stderr, "FAIL: no CaptureFrame reply (n=%zd)\n", n); return 1;
    }

    int shm = -1;
    for (cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            std::memcpy(&shm, CMSG_DATA(c), sizeof(int));
    if (shm < 0) { std::fprintf(stderr, "FAIL: reply carried no shm fd (capture failed server-side)\n"); return 1; }

    ScreencopyFrameInfo info{};
    std::memcpy(&info, rep.payload_, sizeof(info));
    if (info.width_ == 0 || info.height_ == 0) { std::fprintf(stderr, "FAIL: zero frame size\n"); return 1; }
    std::fprintf(stderr, "captured %ux%u stride=%u format=%u\n",
                 info.width_, info.height_, info.stride_, info.format_);

    size_t map_len = static_cast<size_t>(info.height_) * info.stride_;
    void* map = mmap(nullptr, map_len, PROT_READ, MAP_SHARED, shm, 0);
    if (map == MAP_FAILED) { std::fprintf(stderr, "FAIL: mmap\n"); return 1; }

    // XRGB8888 little-endian = bytes [B][G][R][X]; emit RGB for the PPM.
    std::ofstream f(out_path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "FAIL: open %s\n", out_path); munmap(map, map_len); return 1; }
    f << "P6\n" << info.width_ << ' ' << info.height_ << "\n255\n";
    std::vector<uint8_t> row(static_cast<size_t>(info.width_) * 3);
    for (uint32_t y = 0; y < info.height_; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(map) + static_cast<size_t>(y) * info.stride_;
        for (uint32_t x = 0; x < info.width_; ++x) {
            row[x*3+0] = src[x*4+2]; row[x*3+1] = src[x*4+1]; row[x*3+2] = src[x*4+0];
        }
        f.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
    f.close();
    munmap(map, map_len);
    ::close(shm);
    ::close(fd);
    std::fprintf(stderr, "wrote %s\n", out_path);
    return 0;
}
