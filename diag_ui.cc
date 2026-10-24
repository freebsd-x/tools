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
#include <vector>

using namespace fbx::xkit;
using namespace fbx::xserver;
using namespace fbx::shared;

int main() {
    std::cout << " FreeBSD-X 1.0 RUNTIME UI DIAGNOSTIC TOOL\n";

    const char* path = "/tmp/freebsd-x.sock";
    int sock = -1;
    bool connected = false;
    
    for (int i = 0; i < 20; i++) {
        sock = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

        if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            connected = true;
            break;
        }
        ::close(sock);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (!connected) {
        std::cerr << "FAIL: Could not connect to " << path << "\n";
        return 1;
    }

    auto client_socket = std::make_shared<IpcSocket>(UniqueFd(sock));
    XBusClient xbus(client_socket);
    int flags = fcntl(sock, F_GETFL, 0); fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    std::cout << "OK: Connected to XBus.\n";

    uint32_t shell_id = 0;
    bool tree_done = false;
    xbus.Call("com.freebsdx.Automation", 0x01, {}, [&](const XBusMessage& response) {
        uint32_t count;
        std::memcpy(&count, response.payload_ + 252, 4);
        std::cout << "Tree Query DONE. Found " << count << " surfaces.\n";
        if (count > 0) std::memcpy(&shell_id, response.payload_, 4);
        tree_done = true;
    });

    for (int i = 0; i < 200 && !tree_done; i++) {
        auto m = client_socket->receiveMessage();
        if (m) xbus.HandleIncoming(m.value().first);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (shell_id != 0) {
        std::cout << "Inspecting Surface " << shell_id << "... ";
        std::vector<uint8_t> p(4); std::memcpy(p.data(), &shell_id, 4);
        bool inspect_done = false;
        xbus.Call("com.freebsdx.Automation", 0x06, p, [&](const XBusMessage& resp) {
            if (std::strcmp(reinterpret_cast<const char*>(resp.payload_), "PIXELS_CAPTURED") == 0) {
                std::cout << "SUCCESS. Shell is rendering on GPU.\n";
            }
            inspect_done = true;
        });

        for (int i = 0; i < 200 && !inspect_done; i++) {
            auto m = client_socket->receiveMessage();
            if (m) xbus.HandleIncoming(m.value().first);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    return shell_id != 0 ? 0 : 1;
}
