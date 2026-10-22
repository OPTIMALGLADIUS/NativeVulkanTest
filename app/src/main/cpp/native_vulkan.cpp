#include <jni.h>
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>

static std::string versionString(uint32_t version) {
    std::ostringstream out;
    out << VK_VERSION_MAJOR(version) << "."
        << VK_VERSION_MINOR(version) << "."
        << VK_VERSION_PATCH(version);
    return out.str();
}

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_optimalgladius_nativevulkantest_MainActivity_getVulkanReport(
        JNIEnv* env, jobject) {

    std::ostringstream report;
    report << "Native Vulkan Diagnostic\n"
           << "========================\n\n"
           << "CPU ABI: arm64-v8a\n"
           << "Implementation: Native C++\n\n";

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "NativeVulkanTest";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "VulkanProbe";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result =
        vkCreateInstance(&instanceInfo, nullptr, &instance);

    if (result != VK_SUCCESS) {
        report << "vkCreateInstance FAILED: " << result << "\n";
        report << "\nThis app could not create a Vulkan instance.";
        return env->NewStringUTF(report.str().c_str());
    }

    report << "Vulkan instance: SUCCESS\n\n";

    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(
        instance, &deviceCount, nullptr);

    if (result != VK_SUCCESS || deviceCount == 0) {
        report << "Physical GPU enumeration FAILED\n"
               << "VkResult: " << result << "\n"
               << "Device count: " << deviceCount << "\n";
        vkDestroyInstance(instance, nullptr);
        return env->NewStringUTF(report.str().c_str());
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    result = vkEnumeratePhysicalDevices(
        instance, &deviceCount, devices.data());

    if (result != VK_SUCCESS) {
        report << "Could not retrieve physical devices: "
               << result << "\n";
        vkDestroyInstance(instance, nullptr);
        return env->NewStringUTF(report.str().c_str());
    }

    for (uint32_t i = 0; i < deviceCount; ++i) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(devices[i], &properties);

        report << "GPU " << i + 1 << "\n"
               << "------------------------\n"
               << "Name: " << properties.deviceName << "\n"
               << "Vendor ID: 0x" << std::hex
               << properties.vendorID << std::dec << "\n"
               << "Device ID: 0x" << std::hex
               << properties.deviceID << std::dec << "\n"
               << "Device type: " << properties.deviceType << "\n"
               << "API version: "
               << versionString(properties.apiVersion) << "\n"
               << "Driver version: "
               << properties.driverVersion << "\n"
               << "Max 2D image dimension: "
               << properties.limits.maxImageDimension2D << "\n";

        uint32_t extensionCount = 0;
        VkResult extResult =
            vkEnumerateDeviceExtensionProperties(
                devices[i], nullptr, &extensionCount, nullptr);

        if (extResult == VK_SUCCESS) {
            report << "Device extensions: "
                   << extensionCount << "\n";

            std::vector<VkExtensionProperties> extensions(
                extensionCount);

            extResult = vkEnumerateDeviceExtensionProperties(
                devices[i], nullptr, &extensionCount,
                extensions.data());

            if (extResult == VK_SUCCESS) {
                const char* important[] = {
                    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
                    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
                    VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                    VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME
                };

                report << "\nSelected extensions:\n";

                for (const char* name : important) {
                    bool found = false;

                    for (const auto& ext : extensions) {
                        if (std::string(ext.extensionName) == name) {
                            found = true;
                            break;
                        }
                    }

                    report << (found ? "[YES] " : "[NO]  ")
                           << name << "\n";
                }
            }
        }

        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(
            devices[i], &queueCount, nullptr);

        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(
            devices[i], &queueCount, queues.data());

        report << "\nQueue families: " << queueCount << "\n";

        for (uint32_t q = 0; q < queueCount; ++q) {
            report << "  Queue " << q
                   << ": count=" << queues[q].queueCount
                   << ", graphics="
                   << ((queues[q].queueFlags &
                        VK_QUEUE_GRAPHICS_BIT) != 0 ? "yes" : "no")
                   << ", compute="
                   << ((queues[q].queueFlags &
                        VK_QUEUE_COMPUTE_BIT) != 0 ? "yes" : "no")
                   << ", transfer="
                   << ((queues[q].queueFlags &
                        VK_QUEUE_TRANSFER_BIT) != 0 ? "yes" : "no")
                   << "\n";
        }

        // Try creating a logical device with a graphics queue.
        uint32_t graphicsQueue = UINT32_MAX;

        for (uint32_t q = 0; q < queueCount; ++q) {
            if (queues[q].queueCount > 0 &&
                (queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                graphicsQueue = q;
                break;
            }
        }

        if (graphicsQueue != UINT32_MAX) {
            float priority = 1.0f;

            VkDeviceQueueCreateInfo queueInfo{};
            queueInfo.sType =
                VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = graphicsQueue;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &priority;

            VkDeviceCreateInfo deviceInfo{};
            deviceInfo.sType =
                VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            deviceInfo.queueCreateInfoCount = 1;
            deviceInfo.pQueueCreateInfos = &queueInfo;

            VkDevice logicalDevice = VK_NULL_HANDLE;
            VkResult deviceResult = vkCreateDevice(
                devices[i], &deviceInfo, nullptr, &logicalDevice);

            report << "\nLogical device creation: "
                   << (deviceResult == VK_SUCCESS
                       ? "SUCCESS" : "FAILED")
                   << " (VkResult=" << deviceResult << ")\n";

            if (deviceResult == VK_SUCCESS) {
                VkQueue queue = VK_NULL_HANDLE;
                vkGetDeviceQueue(
                    logicalDevice, graphicsQueue, 0, &queue);

                report << "Graphics queue retrieval: "
                       << (queue != VK_NULL_HANDLE
                           ? "SUCCESS" : "FAILED") << "\n";

                vkDestroyDevice(logicalDevice, nullptr);
            }
        } else {
            report << "\nNo graphics-capable queue found.\n";
        }

        report << "\n";
    }

    vkDestroyInstance(instance, nullptr);

    report << "========================\n"
           << "Diagnostic completed.\n";

    return env->NewStringUTF(report.str().c_str());
}
