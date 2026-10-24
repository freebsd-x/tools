#include "xserver/seat_manager.h"
#include "xserver/drm_device.h"
#include <vulkan/vulkan.h>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

using namespace fbx::xserver;

void print_header(const std::string& title) {
    std::cout << "\n\033[1;35m--- " << title << " ---\033[0m\n";
}

void check_vulkan() {
    print_header("VULKAN SUBSYSTEM");
    
    VkInstance instance;
    VkApplicationInfo appInfo{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "FreeBSD-X Diag", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo instInfo{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &appInfo};
    
    if (vkCreateInstance(&instInfo, nullptr, &instance) != VK_SUCCESS) {
        std::cout << "[\033[1;31mFAIL\033[0m] Failed to create Vulkan instance. Is the driver installed?\n";
        return;
    }
    std::cout << "[\033[1;32m OK \033[0m] Vulkan Instance created.\n";

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        std::cout << "[\033[1;31mFAIL\033[0m] No Vulkan-compatible GPUs found.\n";
        vkDestroyInstance(instance, nullptr);
        return;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    std::cout << "Found " << deviceCount << " GPU(s):\n";

    for (uint32_t i = 0; i < deviceCount; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        std::cout << "  - GPU[" << i << "]: " << props.deviceName << " (Type: " << props.deviceType << ")\n";
        
        // Check extensions
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> availableExts(extCount);
        vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &extCount, availableExts.data());
        
        bool has_ext_mem = false;
        bool has_fd = false;
        for (const auto& ext : availableExts) {
            if (std::strcmp(ext.extensionName, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) == 0) has_ext_mem = true;
            if (std::strcmp(ext.extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0) has_fd = true;
        }
        std::cout << "    External Memory: " << (has_ext_mem ? "\033[1;32mYES\033[0m" : "\033[1;31mNO\033[0m") << "\n";
        std::cout << "    External FD: " << (has_fd ? "\033[1;32mYES\033[0m" : "\033[1;31mNO\033[0m") << "\n";
    }

    vkDestroyInstance(instance, nullptr);
}

int main() {
    std::cout << "\033[1;36m====================================================\n";
    std::cout << " FreeBSD-X 1.0 PHYSICAL HARDWARE SENTINEL\n";
    std::cout << "====================================================\033[0m\n";

    // 1. SEAT CHECK
    print_header("SEAT MANAGEMENT (seatd)");
    auto seat_res = SeatManager::create();
    if (!seat_res) {
        std::cout << "[\033[1;31mFAIL\033[0m] " << seat_res.error().message() << "\n";
        std::cout << "      -> Ensure 'seatd' is running and you are in 'video' group.\n";
    } else {
        auto& seat = seat_res.value();
        std::cout << "[\033[1;32m OK \033[0m] Seat acquired successfully.\n";

        // 2. DRM CHECK
        print_header("DIRECT RENDERING MANAGER (DRM)");
        const std::vector<std::string> nodes = {"/dev/dri/card0", "/dev/dri/card1", "/dev/drm/0"};
        for (const auto& path : nodes) {
            if (access(path.c_str(), F_OK) != 0) {
                std::cout << "  - Node " << path << ": \033[1;33mMISSING\033[0m\n";
                continue;
            }
            std::cout << "  - Node " << path << ": \033[1;32mFOUND\033[0m. Testing access... ";
            
            auto dev_res = seat->openDevice(path);
            if (!dev_res) {
                std::cout << "\033[1;31mFAILED\033[0m (" << dev_res.error().message() << ")\n";
            } else {
                std::cout << "\033[1;32mSUCCESS\033[0m (FD: " << dev_res->fd_.get() << ")\n";
                
                struct stat st;
                if (fstat(dev_res->fd_.get(), &st) == 0 && S_ISCHR(st.st_mode)) {
                    std::cout << "    Character Device: \033[1;32mYES\033[0m\n";
                } else {
                    std::cout << "    Character Device: \033[1;31mNO\033[0m (Major Issue!)\n";
                }
                
                // Try to get resources
                // DrmDevice test_dev(std::move(dev_res->fd_), seat.get(), dev_res->device_id_);
                // In a real diag we'd probe connectors here
                seat->closeDevice(dev_res->device_id_);
            }
        }
    }

    // 3. VULKAN CHECK
    check_vulkan();

    std::cout << "\n\033[1;36m====================================================\033[0m\n";
    return 0;
}
