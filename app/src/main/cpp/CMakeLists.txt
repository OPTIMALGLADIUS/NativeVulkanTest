cmake_minimum_required(VERSION 3.22.1)

project(NativeVulkanTest LANGUAGES CXX)

add_library(vulkan_probe SHARED native_vulkan.cpp)

find_library(android-lib android)
find_library(log-lib log)
find_library(vulkan-lib vulkan)

target_link_libraries(
    vulkan_probe
    ${android-lib}
    ${log-lib}
    ${vulkan-lib}
)
