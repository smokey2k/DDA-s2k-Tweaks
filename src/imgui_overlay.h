#pragma once

#include <vulkan/vulkan.h>

namespace dda {

struct VulkanOverlayInfo {
    VkInstance instance{};
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t queue_family{};
    VkQueue present_queue{};
    uint32_t present_queue_family{};
    VkSwapchainKHR swapchain{};
    VkFormat format{};
    VkExtent2D extent{};
    VkImageUsageFlags image_usage{};
    VkSharingMode sharing_mode{VK_SHARING_MODE_EXCLUSIVE};
    uint32_t min_image_count{};
    PFN_vkGetInstanceProcAddr get_instance_proc_addr{};
    PFN_vkGetDeviceProcAddr get_device_proc_addr{};
};

// Renders before present and replaces the present wait semaphores when successful.
bool render_imgui_overlay(const VulkanOverlayInfo& info,
                          const VkPresentInfoKHR& source,
                          VkPresentInfoKHR& patched,
                          VkSemaphore& overlay_complete);
void imgui_overlay_before_swapchain_destroy(VkDevice device, VkSwapchainKHR swapchain);

} // namespace dda
