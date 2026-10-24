#include "shared/logger.h"
#include "xkit/engine.h"
#include "xkit/application.h"
#include "xshell/app_discovery.h"
#include "xserver/seat_manager.h"
#include <iostream>
#include <vector>
#include <filesystem>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <cstdio>
#include <grp.h>
#include <pwd.h>

using namespace fbx::shared;
using namespace fbx::xkit;
using namespace fbx::xshell;
using namespace fbx::xserver;

void print_header(const std::string& title) {
    std::cout << "\n\033[1;34m[ " << title << " ]\033[0m\n";
}

bool check_kld(const std::string& module) {
    std::cout << "Checking for " << module << "... ";
    std::string cmd = "kldstat -m " + module + " >/dev/null 2>&1";
    if (system(cmd.c_str()) == 0) {
        std::cout << "\033[1;32mLOADED\033[0m\n";
        return true;
    }
    std::cout << "\033[1;31mMISSING\033[0m\n";
    return false;
}

bool check_file(const std::string& path) {
    std::cout << "Checking " << path << "... ";
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        if (S_ISCHR(st.st_mode)) {
            std::cout << "\033[1;32mREADY\033[0m (Character Device)\n";
            return true;
        } else {
            std::cout << "\033[1;31mFAILED\033[0m (Regular File/Mock detected! Hardware driver is not active)\n";
            return false;
        }
    }
    std::cout << "\033[1;31mNOT FOUND\033[0m\n";
    return false;
}

bool check_group(const std::string& group_name) {
    std::cout << "Checking group membership (" << group_name << ")... ";
    gid_t groups[NGROUPS_MAX];
    int ngroups = getgroups(NGROUPS_MAX, groups);
    struct group* gr = getgrnam(group_name.c_str());
    if (!gr) {
        std::cout << "\033[1;31mNOT FOUND\033[0m\n";
        return false;
    }
    for (int i = 0; i < ngroups; i++) {
        if (groups[i] == gr->gr_gid) {
            std::cout << "\033[1;32mOK\033[0m\n";
            return true;
        }
    }
    std::cout << "\033[1;31mMISSING\033[0m (Run: pw groupmod " << group_name << " -m your_user)\n";
    return false;
}

bool check_service(const std::string& service_name) {
    std::cout << "Checking service status (" << service_name << ")... ";
    std::string cmd = "service " + service_name + " onestatus >/dev/null 2>&1";
    if (system(cmd.c_str()) == 0) {
        std::cout << "\033[1;32mRUNNING\033[0m\n";
        return true;
    }
    std::cout << "\033[1;31mNOT RUNNING\033[0m (Run: service " << service_name << " onestart)\n";
    return false;
}

bool check_tty() {
    std::cout << "Checking TTY ownership... ";
    char* tty = ttyname(STDIN_FILENO);
    if (!tty) {
        std::cout << "\033[1;31mFAILED (Not a TTY)\033[0m. Ensure you are not in tmux/ssh.\n";
        return false;
    }
    struct stat st;
    if (fstat(STDIN_FILENO, &st) == 0) {
        uid_t me = getuid();
        if (st.st_uid == me) {
            std::cout << "\033[1;32mOK\033[0m (" << tty << " owned by you)\n";
            return true;
        } else {
            std::cout << "\033[1;31mFAILED\033[0m (" << tty << " owned by UID " << st.st_uid << ", you are " << me << ")\n";
            std::cout << " -> Try running: sudo chown $(whoami) " << tty << "\n";
            return false;
        }
    }
    return false;
}

int main() {
    Logger::init("verify");
    std::cout << "\033[1;36m====================================================\n";
    std::cout << " FreeBSD-X 1.0 AUTOMATED PRE-FLIGHT VERIFICATION\n";
    std::cout << "====================================================\033[0m\n";

    int warnings  = 0; // non-blocking issues
    int failures  = 0; // hard failures that prevent startup

    // 0. Identity & Permissions
    print_header("LAYER 0: PERMISSIONS");
    if (!check_tty())          ++warnings;  // SSH/tmux is a warning, not fatal
    if (!check_group("video")) ++failures;  // required for DRM access
    if (!check_group("wheel")) ++warnings;

    // 1. Kernel Layer
    print_header("LAYER 1: KERNEL DRIVERS");
    bool has_gpu = check_kld("i915kms") || check_kld("amdgpu")
                || check_kld("radeonkms") || check_kld("nvidia-drm");
    if (!has_gpu) {
        std::cout << "\033[1;33m[!] No hardware-accelerated GPU driver found."
                     " System will use headless/software mode.\033[0m\n";
        ++warnings;
    }

    // 2. Hardware Access Layer
    print_header("LAYER 2: HARDWARE DEVICE NODES");
    if (!check_file("/dev/dri/card0"))       ++failures;
    if (!check_file("/dev/dri/renderD128"))  ++failures;
    check_file("/dev/dsp");  // audio missing is a warning, not fatal

    // 3. Gatekeeper Layer
    print_header("LAYER 3: SEAT MANAGEMENT");
    if (!check_service("seatd")) ++failures;
    std::cout << "Checking seatd socket... ";
    if (access("/var/run/seatd.sock", R_OK | W_OK) == 0) {
        std::cout << "\033[1;32mREADY\033[0m\n";
    } else {
        std::cout << "\033[1;31mINACCESSIBLE\033[0m"
                     " (check seatd is running and permissions are correct)\n";
        ++failures;
    }

    std::cout << "Initializing SeatManager via seatd... ";
    {
        auto seat_res = SeatManager::create();
        if (seat_res) {
            std::cout << "\033[1;32mOK\033[0m\n";
        } else {
            std::cout << "\033[1;31mFAILED\033[0m (Is seatd running?)\n";
            ++failures;
        }
        // Scope exit: SeatManager released, DRM master dropped
    }

    // 4. Graphics Engine Layer
    print_header("LAYER 4: VULKAN & GBM PIPELINE");
    {
        VulkanEngine engine;
        if (engine.init(100, 100)) {
            std::cout << "\033[1;32mSUCCESS\033[0m:"
                         " GPU DMABUF allocation and export functional.\n";
        } else {
            std::cout << "\033[1;31mFAILED\033[0m:"
                         " GPU pipeline failed. Check 'video' group membership.\n";
            ++failures;
        }
    } // engine destroyed here — releases Vulkan device before next stage

    // 5. App Ecosystem Layer
    print_header("LAYER 5: APPLICATION BUNDLES");
    AppDiscoveryService discovery;
    auto apps = discovery.discoverApps("./apps");
    if (apps.empty()) apps = discovery.discoverApps("/usr/local/apps");
    if (apps.empty()) {
        std::cout << "\033[1;33m[!] No application bundles found"
                     " (checked ./apps and /usr/local/apps).\033[0m\n";
        ++warnings;
    } else {
        std::cout << "Discovered " << apps.size() << " application bundle(s):\n";
        for (const auto& app : apps) {
            std::cout << "  -> " << app.name << " (" << app.identifier
                      << ")  [\033[1;32mVALID\033[0m]\n";
        }
    }

    // 6. Server Connectivity
    print_header("LAYER 6: SERVER SOCKET");
    std::cout << "Checking /tmp/freebsdx.sock... ";
    if (access("/tmp/freebsdx.sock", R_OK | W_OK) == 0) {
        std::cout << "\033[1;32mLIVE\033[0m (compositor is running)\n";
    } else {
        std::cout << "\033[1;33mNOT FOUND\033[0m"
                     " (compositor not running — start freebsd-x-server first)\n";
        ++warnings; // warning only: verify can run before the server starts
    }

    // Summary
    std::cout << "\n\033[1;36m====================================================\033[0m\n";
    if (failures == 0 && warnings == 0) {
        std::cout << "\033[1;32m ALL CHECKS PASSED — System ready to launch.\033[0m\n";
    } else {
        if (failures > 0)
            std::cout << "\033[1;31m " << failures << " FAILURE(S) — System cannot start.\033[0m\n";
        if (warnings > 0)
            std::cout << "\033[1;33m " << warnings << " WARNING(S) — System may start with reduced functionality.\033[0m\n";
    }
    std::cout << "\033[1;36m====================================================\033[0m\n";

    // Exit 0 = all clear, 1 = warnings only, 2 = hard failures
    return failures > 0 ? 2 : (warnings > 0 ? 1 : 0);
}
