#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <limits>

#define LOG_TAG "NativeVulkan"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

std::thread renderThread;
std::atomic<bool> running{false};
std::atomic<uint64_t> presentedFrames{0};

std::mutex lifecycleMutex;
std::mutex statusMutex;

std::string rendererStatus = "Renderer not started";

void setRendererStatus(const std::string& message) {
    std::lock_guard<std::mutex> lock(statusMutex);
    rendererStatus = message;
    LOGI("%s", message.c_str());
}

std::string getStatusSnapshot() {
    std::lock_guard<std::mutex> lock(statusMutex);
    return rendererStatus;
}

void logVk(const char* operation, VkResult result) {
    LOGE("%s failed: VkResult=%d", operation,
         static_cast<int>(result));
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
        if (std::string(ext.extensionName) == name) {
            return true;
        }
    }

    return false;
}

bool hasDeviceExtension(
        VkPhysicalDevice device,
        const char* name) {
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
        if (std::string(ext.extensionName) == name) {
            return true;
        }
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
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
        }

        if (frameFence != VK_NULL_HANDLE) {
            vkDestroyFence(device, frameFence, nullptr);
        }

        if (imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, imageAvailable, nullptr);
        }

        if (renderFinished != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, renderFinished, nullptr);
        }

        if (commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, commandPool, nullptr);
        }

        if (swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        }

        if (device != VK_NULL_HANDLE) {
            vkDestroyDevice(device, nullptr);
        }

        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
        }

        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }

        if (window != nullptr) {
            ANativeWindow_release(window);
        }
    };

    auto fail = [&](const std::string& message) {
        setRendererStatus("ERROR: " + message);
        cleanup();
    };

    setRendererStatus("Checking Vulkan surface extensions...");

    if (!hasInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME)) {
        fail("VK_KHR_surface is unavailable.");
        return;
    }

    if (!hasInstanceExtension(
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
        fail("VK_KHR_android_surface is unavailable.");
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
        fail("vkCreateInstance failed: VkResult=" +
             std::to_string(static_cast<int>(result)));
        return;
    }

    setRendererStatus("Vulkan instance: SUCCESS");

    VkAndroidSurfaceCreateInfoKHR surfaceInfo{
        VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR
    };
    surfaceInfo.window = window;

    result = vkCreateAndroidSurfaceKHR(
        instance, &surfaceInfo, nullptr, &surface
    );

    if (result != VK_SUCCESS) {
        fail("Android surface creation failed: VkResult=" +
             std::to_string(static_cast<int>(result)));
        return;
    }

    setRendererStatus("Android surface: SUCCESS");

    uint32_t physicalCount = 0;
    result = vkEnumeratePhysicalDevices(
        instance, &physicalCount, nullptr
    );

    if (result != VK_SUCCESS || physicalCount == 0) {
        fail("No Vulkan physical devices found.");
        return;
    }

    std::vector<VkPhysicalDevice> physicalDevices(physicalCount);

    result = vkEnumeratePhysicalDevices(
        instance, &physicalCount, physicalDevices.data()
    );

    if (result != VK_SUCCESS) {
        fail("Physical-device enumeration failed.");
        return;
    }

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

            result = vkGetPhysicalDeviceSurfaceSupportKHR(
                candidate, i, surface, &canPresent
            );

            if (result == VK_SUCCESS &&
                canPresent == VK_TRUE &&
                (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                physicalDevice = candidate;
                graphicsFamily = i;
                break;
            }
        }

        if (physicalDevice != VK_NULL_HANDLE) {
            break;
        }
    }

    if (physicalDevice == VK_NULL_HANDLE) {
        fail("No graphics queue supports surface presentation.");
        return;
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);

    const std::string gpuName = properties.deviceName;

    setRendererStatus("GPU selected: " + gpuName);

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
        fail("Logical-device creation failed: VkResult=" +
             std::to_string(static_cast<int>(result)));
        return;
    }

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, graphicsFamily, 0, &queue);

    setRendererStatus("Logical device: SUCCESS\nGPU: " + gpuName);

    VkSurfaceCapabilitiesKHR capabilities{};

    result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physicalDevice, surface, &capabilities
    );

    if (result != VK_SUCCESS) {
        fail("Surface capabilities query failed.");
        return;
    }

    if (!(capabilities.supportedUsageFlags &
          VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        fail("Swapchain images do not support transfer-destination usage.");
        return;
    }

    uint32_t formatCount = 0;

    result = vkGetPhysicalDeviceSurfaceFormatsKHR(
        physicalDevice, surface, &formatCount, nullptr
    );

    if (result != VK_SUCCESS || formatCount == 0) {
        fail("No Vulkan surface formats are available.");
        return;
    }

    std::vector<VkSurfaceFormatKHR> formats(formatCount);

    result = vkGetPhysicalDeviceSurfaceFormatsKHR(
        physicalDevice, surface, &formatCount, formats.data()
    );

    if (result != VK_SUCCESS) {
        fail("Could not retrieve surface formats.");
        return;
    }

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
        const int width = std::max(
            1, ANativeWindow_getWidth(window)
        );
        const int height = std::max(
            1, ANativeWindow_getHeight(window)
        );

        extent.width = std::clamp(
            static_cast<uint32_t>(width),
            capabilities.minImageExtent.width,
            capabilities.maxImageExtent.width
        );

        extent.height = std::clamp(
            static_cast<uint32_t>(height),
            capabilities.minImageExtent.height,
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

    bool alphaFound = false;

    for (auto mode : alphaModes) {
        if (capabilities.supportedCompositeAlpha & mode) {
            compositeAlpha = mode;
            alphaFound = true;
            break;
        }
    }

    if (!alphaFound) {
        fail("No supported composite-alpha mode was found.");
        return;
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
        fail("Swapchain creation failed: VkResult=" +
             std::to_string(static_cast<int>(result)));
        return;
    }

    setRendererStatus(
        "Swapchain: SUCCESS\nGPU: " + gpuName +
        "\nResolution: " + std::to_string(extent.width) +
        " x " + std::to_string(extent.height)
    );

    uint32_t swapImageCount = 0;

    result = vkGetSwapchainImagesKHR(
        device, swapchain, &swapImageCount, nullptr
    );

    if (result != VK_SUCCESS || swapImageCount == 0) {
        fail("Could not retrieve swapchain images.");
        return;
    }

    std::vector<VkImage> images(swapImageCount);

    result = vkGetSwapchainImagesKHR(
        device, swapchain, &swapImageCount, images.data()
    );

    if (result != VK_SUCCESS) {
        fail("Could not retrieve swapchain image handles.");
        return;
    }

    VkCommandPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO
    };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsFamily;

    result = vkCreateCommandPool(
        device, &poolInfo, nullptr, &commandPool
    );

    if (result != VK_SUCCESS) {
        fail("Command-pool creation failed.");
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
        fail("Command-buffer allocation failed.");
        return;
    }

    VkSemaphoreCreateInfo semaphoreInfo{
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };

    result = vkCreateSemaphore(
        device, &semaphoreInfo, nullptr, &imageAvailable
    );

    if (result != VK_SUCCESS) {
        fail("Image-available semaphore creation failed.");
        return;
    }

    result = vkCreateSemaphore(
        device, &semaphoreInfo, nullptr, &renderFinished
    );

    if (result != VK_SUCCESS) {
        fail("Render-finished semaphore creation failed.");
        return;
    }

    VkFenceCreateInfo fenceInfo{
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    };
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    result = vkCreateFence(
        device, &fenceInfo, nullptr, &frameFence
    );

    if (result != VK_SUCCESS) {
        fail("Frame fence creation failed.");
        return;
    }

    setRendererStatus(
        "Renderer ready\nGPU: " + gpuName +
        "\nSwapchain: SUCCESS\nWaiting for first frame..."
    );

    while (running.load()) {
        result = vkWaitForFences(
            device, 1, &frameFence, VK_TRUE, UINT64_MAX
        );

        if (result != VK_SUCCESS) {
            logVk("vkWaitForFences", result);
            setRendererStatus("ERROR: Waiting for frame fence failed.");
            break;
        }

        uint32_t imageIndex = 0;

        // Timeout periodically so stop requests are not delayed indefinitely.
        result = vkAcquireNextImageKHR(
            device,
            swapchain,
            1000000000ULL,
            imageAvailable,
            VK_NULL_HANDLE,
            &imageIndex
        );

        if (result == VK_TIMEOUT || result == VK_NOT_READY) {
            continue;
        }

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            setRendererStatus(
                "Swapchain out of date. Surface recreation is needed."
            );
            break;
        }

        if (result != VK_SUCCESS &&
            result != VK_SUBOPTIMAL_KHR) {
            logVk("vkAcquireNextImageKHR", result);
            setRendererStatus(
                "ERROR: Image acquisition failed: VkResult=" +
                std::to_string(static_cast<int>(result))
            );
            break;
        }

        const bool acquireSuboptimal =
            result == VK_SUBOPTIMAL_KHR;

        result = vkResetFences(device, 1, &frameFence);

        if (result != VK_SUCCESS) {
            setRendererStatus("ERROR: Could not reset frame fence.");
            break;
        }

        result = vkResetCommandBuffer(commandBuffer, 0);

        if (result != VK_SUCCESS) {
            setRendererStatus("ERROR: Could not reset command buffer.");
            break;
        }

        VkCommandBufferBeginInfo beginInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
        };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        result = vkBeginCommandBuffer(commandBuffer, &beginInfo);

        if (result != VK_SUCCESS) {
            setRendererStatus("ERROR: Could not begin command buffer.");
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
        toTransfer.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.baseMipLevel = 0;
        toTransfer.subresourceRange.levelCount = 1;
        toTransfer.subresourceRange.baseArrayLayer = 0;
        toTransfer.subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &toTransfer
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
            commandBuffer,
            images[imageIndex],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &clearColor,
            1,
            &range
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
            0,
            0, nullptr,
            0, nullptr,
            1, &toPresent
        );

        result = vkEndCommandBuffer(commandBuffer);

        if (result != VK_SUCCESS) {
            setRendererStatus("ERROR: Could not finish command buffer.");
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
            setRendererStatus(
                "ERROR: GPU submission failed: VkResult=" +
                std::to_string(static_cast<int>(result))
            );
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

        // This is intentionally conservative for the diagnostic version:
        // wait until presentation has consumed its semaphore before reuse.
        VkResult idleResult = vkQueueWaitIdle(queue);

        if (idleResult != VK_SUCCESS) {
            logVk("vkQueueWaitIdle", idleResult);
            setRendererStatus("ERROR: Queue idle wait failed.");
            break;
        }

        if (result == VK_SUCCESS ||
            result == VK_SUBOPTIMAL_KHR) {
            ++presentedFrames;

            std::string message =
                "Vulkan presentation: SUCCESS\nGPU: " + gpuName +
                "\nResolution: " + std::to_string(extent.width) +
                " x " + std::to_string(extent.height);

            if (acquireSuboptimal ||
                result == VK_SUBOPTIMAL_KHR) {
                message += "\nWarning: swapchain is suboptimal.";
            }

            setRendererStatus(message);
        } else if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            setRendererStatus(
                "Swapchain out of date. Surface recreation is needed."
            );
            break;
        } else {
            logVk("vkQueuePresentKHR", result);
            setRendererStatus(
                "ERROR: Presentation failed: VkResult=" +
                std::to_string(static_cast<int>(result))
            );
            break;
        }
    }

    if (running.load()) {
        running.store(false);
    }

    cleanup();
    LOGI("Vulkan renderer stopped.");
}

} // namespace

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_optimalgladius_nativevulkantest_MainActivity_startRendering(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject javaSurface) {

    std::lock_guard<std::mutex> lock(lifecycleMutex);

    if (running.load()) {
        return env->NewStringUTF(
            "Vulkan renderer is already running."
        );
    }

    // Join a previous render thread if it has already exited.
    if (renderThread.joinable()) {
        renderThread.join();
    }

    presentedFrames.store(0);
    setRendererStatus("Obtaining Android native window...");

    ANativeWindow* window =
        ANativeWindow_fromSurface(env, javaSurface);

    if (window == nullptr) {
        setRendererStatus(
            "ERROR: Could not obtain Android native window."
        );

        return env->NewStringUTF(
            "ERROR: Could not obtain Android native window."
        );
    }

    running.store(true);

    renderThread = std::thread([window]() {
        renderLoop(window);
        running.store(false);
    });

    return env->NewStringUTF(
        "Native Vulkan renderer launched.\n"
        "Renderer status is updating below."
    );
}

extern "C"
JNIEXPORT void JNICALL
Java_dev_optimalgladius_nativevulkantest_MainActivity_stopRendering(
        JNIEnv* /*env*/,
        jobject /*thiz*/) {

    std::lock_guard<std::mutex> lock(lifecycleMutex);

    running.store(false);

    if (renderThread.joinable()) {
        renderThread.join();
    }

    setRendererStatus("Renderer stopped.");
}

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_optimalgladius_nativevulkantest_MainActivity_getRendererStatus(
        JNIEnv* env,
        jobject /*thiz*/) {

    std::string message = getStatusSnapshot();

    message += "\nFrames presented: ";
    message += std::to_string(presentedFrames.load());

    return env->NewStringUTF(message.c_str());
}
