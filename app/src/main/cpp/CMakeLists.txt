cmake_minimum_required(VERSION 3.22.1)

project(NativeVulkanTest LANGUAGES CXX)

add_library(
    vulkan_probe
    SHARED
    native_vulkan.cpp
)

find_library(android-lib android REQUIRED)
find_library(log-lib log REQUIRED)
find_library(vulkan-lib vulkan REQUIRED)

target_compile_features(
    vulkan_probe PRIVATE cxx_std_17
)

target_compile_definitions(
    vulkan_probe PRIVATE
    VK_USE_PLATFORM_ANDROID_KHR
)

target_compile_options(
    vulkan_probe PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

target_link_libraries(
    vulkan_probe
    ${android-lib}
    ${log-lib}
    ${vulkan-lib}
)
