#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>

#define LOG_TAG "NativeVulkan"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

std::thread renderThread;
std::atomic<bool> running{false};
std::mutex lifecycleMutex;

void logVk(const char* operation, VkResult result) {
    if (result != VK_SUCCESS) {
        LOGE("%s failed: VkResult=%d", operation, result);
    }
}

bool hasInstanceExtension(const char* name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(
            nullptr, &count, nullptr) != VK_SUCCESS) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateInstanceExtensionProperties(
            nullptr, &count, extensions.data()) != VK_SUCCESS) {
        return false;
    }

    for (const auto& ext : extensions) {
        if (std::string(ext.extensionName) == name) return true;
    }
    return false;
}

bool hasDeviceExtension(VkPhysicalDevice device, const char* name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(
            device, nullptr, &count, nullptr) != VK_SUCCESS) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(
            device, nullptr, &count, extensions.data()) != VK_SUCCESS) {
        return false;
    }

    for (const auto& ext : extensions) {
        if (std::string(ext.extensionName) == name) return true;
    }
    return false;
}

void renderLoop(ANativeWindow* window) {
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkFence frameFence = VK_NULL_HANDLE;

    auto cleanup = [&]() {
        if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
        if (frameFence) vkDestroyFence(device, frameFence, nullptr);
        if (imageAvailable)
            vkDestroySemaphore(device, imageAvailable, nullptr);
        if (renderFinished)
            vkDestroySemaphore(device, renderFinished, nullptr);
        if (commandPool)
            vkDestroyCommandPool(device, commandPool, nullptr);
        if (swapchain)
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
        if (window) ANativeWindow_release(window);
    };

    if (!hasInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME) ||
        !hasInstanceExtension(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
        LOGE("Required Vulkan surface extensions are unavailable.");
        cleanup();
        return;
    }

    const char* instanceExtensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
    };

    VkApplicationInfo appInfo{
        VK_STRUCTURE_TYPE_APPLICATION_INFO
    };
    appInfo.pApplicationName = "NativeVulkanTest";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 2, 0);
    appInfo.pEngineName = "NativeVulkan";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instanceInfo{
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
    };
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;

    VkResult result = vkCreateInstance(
        &instanceInfo, nullptr, &instance
    );
    if (result != VK_SUCCESS) {
        logVk("vkCreateInstance", result);
        cleanup();
        return;
    }

    VkAndroidSurfaceCreateInfoKHR surfaceInfo{
        VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR
    };
    surfaceInfo.window = window;

    result = vkCreateAndroidSurfaceKHR(
        instance, &surfaceInfo, nullptr, &surface
    );
    if (result != VK_SUCCESS) {
        logVk("vkCreateAndroidSurfaceKHR", result);
        cleanup();
        return;
    }

    uint32_t physicalCount = 0;
    result = vkEnumeratePhysicalDevices(
        instance, &physicalCount, nullptr
    );
    if (result != VK_SUCCESS || physicalCount == 0) {
        LOGE("No Vulkan physical devices found.");
        cleanup();
        return;
    }

    std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
    vkEnumeratePhysicalDevices(
        instance, &physicalCount, physicalDevices.data()
    );

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t graphicsFamily = UINT32_MAX;

    for (VkPhysicalDevice candidate : physicalDevices) {
        if (!hasDeviceExtension(
                candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            continue;
        }

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(
            candidate, &familyCount, nullptr
        );
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(
            candidate, &familyCount, families.data()
        );

        for (uint32_t i = 0; i < familyCount; ++i) {
            VkBool32 canPresent = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(
                candidate, i, surface, &canPresent
            );

            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                canPresent) {
                physicalDevice = candidate;
                graphicsFamily = i;
                break;
            }
        }
        if (physicalDevice != VK_NULL_HANDLE) break;
    }

    if (physicalDevice == VK_NULL_HANDLE) {
        LOGE("No graphics queue supporting presentation was found.");
        cleanup();
        return;
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    LOGI("Selected GPU: %s", properties.deviceName);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO
    };
    queueInfo.queueFamilyIndex = graphicsFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    const char* deviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    VkDeviceCreateInfo deviceInfo{
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO
    };
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;

    result = vkCreateDevice(
        physicalDevice, &deviceInfo, nullptr, &device
    );
    if (result != VK_SUCCESS) {
        logVk("vkCreateDevice", result);
        cleanup();
        return;
    }

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, graphicsFamily, 0, &queue);

    VkSurfaceCapabilitiesKHR capabilities{};
    result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physicalDevice, surface, &capabilities
    );
    if (result != VK_SUCCESS) {
        logVk("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
        cleanup();
        return;
    }

    if (!(capabilities.supportedUsageFlags &
          VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        LOGE("Swapchain images do not support transfer-destination usage.");
        cleanup();
        return;
    }

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(
        physicalDevice, surface, &formatCount, nullptr
    );
    if (formatCount == 0) {
        LOGE("No surface formats available.");
        cleanup();
        return;
    }

    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(
        physicalDevice, surface, &formatCount, formats.data()
    );

    VkSurfaceFormatKHR chosenFormat = formats[0];
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosenFormat = format;
            break;
        }
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosenFormat = format;
        }
    }

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = static_cast<uint32_t>(
            std::max(1, ANativeWindow_getWidth(window))
        );
        extent.height = static_cast<uint32_t>(
            std::max(1, ANativeWindow_getHeight(window))
        );
        extent.width = std::clamp(
            extent.width, capabilities.minImageExtent.width,
            capabilities.maxImageExtent.width
        );
        extent.height = std::clamp(
            extent.height, capabilities.minImageExtent.height,
            capabilities.maxImageExtent.height
        );
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 &&
        imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkCompositeAlphaFlagBitsKHR compositeAlpha =
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR alphaModes[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR
    };
    for (auto mode : alphaModes) {
        if (capabilities.supportedCompositeAlpha & mode) {
            compositeAlpha = mode;
            break;
        }
    }

    VkSwapchainCreateInfoKHR swapInfo{
        VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR
    };
    swapInfo.surface = surface;
    swapInfo.minImageCount = imageCount;
    swapInfo.imageFormat = chosenFormat.format;
    swapInfo.imageColorSpace = chosenFormat.colorSpace;
    swapInfo.imageExtent = extent;
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapInfo.preTransform = capabilities.currentTransform;
    swapInfo.compositeAlpha = compositeAlpha;
    swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapInfo.clipped = VK_TRUE;

    result = vkCreateSwapchainKHR(
        device, &swapInfo, nullptr, &swapchain
    );
    if (result != VK_SUCCESS) {
        logVk("vkCreateSwapchainKHR", result);
        cleanup();
        return;
    }

    uint32_t swapImageCount = 0;
    vkGetSwapchainImagesKHR(
        device, swapchain, &swapImageCount, nullptr
    );
    std::vector<VkImage> images(swapImageCount);
    vkGetSwapchainImagesKHR(
        device, swapchain, &swapImageCount, images.data()
    );

    VkCommandPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO
    };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsFamily;

    result = vkCreateCommandPool(
        device, &poolInfo, nullptr, &commandPool
    );
    if (result != VK_SUCCESS) {
        logVk("vkCreateCommandPool", result);
        cleanup();
        return;
    }

    VkCommandBufferAllocateInfo allocInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
    };
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    result = vkAllocateCommandBuffers(
        device, &allocInfo, &commandBuffer
    );
    if (result != VK_SUCCESS) {
        logVk("vkAllocateCommandBuffers", result);
        cleanup();
        return;
    }

    VkSemaphoreCreateInfo semaphoreInfo{
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    if (vkCreateSemaphore(
            device, &semaphoreInfo, nullptr, &imageAvailable) != VK_SUCCESS ||
        vkCreateSemaphore(
            device, &semaphoreInfo, nullptr, &renderFinished) != VK_SUCCESS) {
        LOGE("Failed to create semaphores.");
        cleanup();
        return;
    }

    VkFenceCreateInfo fenceInfo{
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    };
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    result = vkCreateFence(device, &fenceInfo, nullptr, &frameFence);
    if (result != VK_SUCCESS) {
        logVk("vkCreateFence", result);
        cleanup();
        return;
    }

    LOGI("Swapchain created: %ux%u, %u images",
         extent.width, extent.height, swapImageCount);

    while (running.load()) {
        vkWaitForFences(device, 1, &frameFence, VK_TRUE, UINT64_MAX);

        uint32_t imageIndex = 0;
        result = vkAcquireNextImageKHR(
            device, swapchain, UINT64_MAX,
            imageAvailable, VK_NULL_HANDLE, &imageIndex
        );

        if (result == VK_ERROR_OUT_OF_DATE_KHR ||
            result == VK_SUBOPTIMAL_KHR) {
            LOGE("Swapchain needs recreation; stopping this render loop.");
            break;
        }
        if (result != VK_SUCCESS) {
            logVk("vkAcquireNextImageKHR", result);
            break;
        }

        vkResetFences(device, 1, &frameFence);
        vkResetCommandBuffer(commandBuffer, 0);

        VkCommandBufferBeginInfo beginInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
        };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
        if (result != VK_SUCCESS) {
            logVk("vkBeginCommandBuffer", result);
            break;
        }

        VkImageMemoryBarrier toTransfer{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER
        };
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = images[imageIndex];
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.baseMipLevel = 0;
        toTransfer.subresourceRange.levelCount = 1;
        toTransfer.subresourceRange.baseArrayLayer = 0;
        toTransfer.subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer
        );

        VkClearColorValue clearColor{};
        clearColor.float32[0] = 0.035f;
        clearColor.float32[1] = 0.12f;
        clearColor.float32[2] = 0.20f;
        clearColor.float32[3] = 1.0f;

        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;

        vkCmdClearColorImage(
            commandBuffer, images[imageIndex],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &clearColor, 1, &range
        );

        VkImageMemoryBarrier toPresent{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER
        };
        toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toPresent.dstAccessMask = 0;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image = images[imageIndex];
        toPresent.subresourceRange = range;

        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toPresent
        );

        result = vkEndCommandBuffer(commandBuffer);
        if (result != VK_SUCCESS) {
            logVk("vkEndCommandBuffer", result);
            break;
        }

        VkPipelineStageFlags waitStage =
            VK_PIPELINE_STAGE_TRANSFER_BIT;

        VkSubmitInfo submitInfo{
            VK_STRUCTURE_TYPE_SUBMIT_INFO
        };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &imageAvailable;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderFinished;

        result = vkQueueSubmit(
            queue, 1, &submitInfo, frameFence
        );
        if (result != VK_SUCCESS) {
            logVk("vkQueueSubmit", result);
            break;
        }

        VkPresentInfoKHR presentInfo{
            VK_STRUCTURE_TYPE_PRESENT_INFO_KHR
        };
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(queue, &presentInfo);
        if (result != VK_SUCCESS &&
            result != VK_SUBOPTIMAL_KHR &&
            result != VK_ERROR_OUT_OF_DATE_KHR) {
            logVk("vkQueuePresentKHR", result);
            break;
        }
    }

    LOGI("Stopping Vulkan renderer.");
    cleanup();
}

} // namespace

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_optimalgladius_nativevulkantest_MainActivity_startRendering(
    JNIEnv* env, jobject /*thiz*/, jobject javaSurface
) {
    std::lock_guard<std::mutex> lock(lifecycleMutex);

    if (running.load()) {
        return env->NewStringUTF("Vulkan renderer is already running.");
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, javaSurface);
    if (!window) {
        return env->NewStringUTF("ERROR: Could not obtain Android native window.");
    }

    running.store(true);
    renderThread = std::thread([window]() {
        renderLoop(window);
        running.store(false);
    });

    return env->NewStringUTF(
        "Native Vulkan renderer started.\n"
        "Look for the blue-grey test frame.\n"
        "Check logcat if the screen remains unchanged."
    );
}

extern "C"
JNIEXPORT void JNICALL
Java_dev_optimalgladius_nativevulkantest_MainActivity_stopRendering(
    JNIEnv* /*env*/, jobject /*thiz*/
) {
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    running.store(false);

    if (renderThread.joinable()) {
        renderThread.join();
    }
}
