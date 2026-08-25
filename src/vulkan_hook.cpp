#include "vulkan_hook.h"

#include "log.h"
#include "imgui_overlay.h"
#include "notification_state.h"
#include "console_unlock.h"
#include "addon_ui.h"
#include "plugin_manager.h"

#include <Windows.h>
#include <MinHook.h>
#include <vulkan/vulkan.h>

#include <cstring>
#include <atomic>
#include <algorithm>
#include <format>
#include <mutex>
#include <vector>

namespace {
using QueuePresentFn = VkResult(VKAPI_PTR*)(VkQueue, const VkPresentInfoKHR*);
using GetDeviceProcAddrFn = PFN_vkVoidFunction(VKAPI_PTR*)(VkDevice, const char*);
using GetInstanceProcAddrFn = PFN_vkVoidFunction(VKAPI_PTR*)(VkInstance, const char*);
using CreateDeviceFn = VkResult(VKAPI_PTR*)(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
using CreateSwapchainFn = VkResult(VKAPI_PTR*)(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
using DestroySwapchainFn = void(VKAPI_PTR*)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
using GetDeviceQueueFn = void(VKAPI_PTR*)(VkDevice, uint32_t, uint32_t, VkQueue*);
using GetDeviceQueue2Fn = void(VKAPI_PTR*)(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);
QueuePresentFn g_original_queue_present{};
GetDeviceProcAddrFn g_original_get_device_proc_addr{};
GetInstanceProcAddrFn g_original_get_instance_proc_addr{};
CreateDeviceFn g_original_create_device{};
CreateSwapchainFn g_original_create_swapchain{};
DestroySwapchainFn g_original_destroy_swapchain{};
GetDeviceQueueFn g_original_get_device_queue{};
GetDeviceQueue2Fn g_original_get_device_queue2{};
std::atomic_uint64_t g_present_calls{};
VkQueue g_first_present_queue{};
bool g_notification_render_ready{};
bool g_missing_state_logged{};
bool g_direct_hooks{};
VkQueue g_logged_overlay_queue{};
VkSwapchainKHR g_incompatible_overlay_swapchain{};
uint32_t g_logged_present_family{UINT32_MAX};

struct CapturedState {
    VkInstance instance{};
    VkPhysicalDevice physical_device{};
};
CapturedState g_state{};

struct DeviceQueueFamily {
    VkDevice device{};
    uint32_t family{};
    uint32_t count{};
};
std::mutex g_device_queue_family_mutex;
std::vector<DeviceQueueFamily> g_device_queue_families;

struct CapturedSwapchain {
    VkSwapchainKHR swapchain{};
    VkDevice device{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkExtent2D extent{};
    VkImageUsageFlags image_usage{};
    VkSharingMode sharing_mode{VK_SHARING_MODE_EXCLUSIVE};
    std::vector<uint32_t> queue_families;
    uint32_t min_image_count{};
};
std::mutex g_swapchain_mutex;
std::vector<CapturedSwapchain> g_swapchains;

void remember_swapchain(VkDevice device, VkSwapchainKHR swapchain,
                        const VkSwapchainCreateInfoKHR& info) {
    if (g_incompatible_overlay_swapchain == swapchain) g_incompatible_overlay_swapchain = VK_NULL_HANDLE;
    std::vector<uint32_t> queue_families;
    if (info.pQueueFamilyIndices && info.queueFamilyIndexCount)
        queue_families.assign(info.pQueueFamilyIndices,
            info.pQueueFamilyIndices + info.queueFamilyIndexCount);
    CapturedSwapchain captured{swapchain, device, info.imageFormat, info.imageExtent,
        info.imageUsage, info.imageSharingMode, std::move(queue_families), info.minImageCount};
    std::scoped_lock lock(g_swapchain_mutex);
    for (auto& item : g_swapchains) {
        if (item.swapchain == swapchain) {
            item = std::move(captured);
            return;
        }
    }
    g_swapchains.push_back(std::move(captured));
}

CapturedSwapchain find_swapchain(VkSwapchainKHR swapchain) {
    std::scoped_lock lock(g_swapchain_mutex);
    for (const auto& item : g_swapchains) {
        if (item.swapchain == swapchain) return item;
    }
    return {};
}

void forget_swapchain(VkSwapchainKHR swapchain) {
    std::scoped_lock lock(g_swapchain_mutex);
    std::erase_if(g_swapchains, [swapchain](const auto& item) { return item.swapchain == swapchain; });
}

VkPhysicalDevice resolve_physical_device() {
    if (g_state.physical_device) return g_state.physical_device;
    if (!g_state.instance || !g_original_get_instance_proc_addr) return VK_NULL_HANDLE;
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        g_original_get_instance_proc_addr(g_state.instance, "vkEnumeratePhysicalDevices"));
    const auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        g_original_get_instance_proc_addr(g_state.instance, "vkGetPhysicalDeviceProperties"));
    if (!enumerate || !get_properties) return VK_NULL_HANDLE;
    uint32_t count = 0;
    if (enumerate(g_state.instance, &count, nullptr) != VK_SUCCESS || !count) return VK_NULL_HANDLE;
    std::vector<VkPhysicalDevice> devices(count);
    if (enumerate(g_state.instance, &count, devices.data()) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkPhysicalDevice selected = devices.front();
    VkPhysicalDeviceProperties selected_properties{};
    get_properties(selected, &selected_properties);
    for (const auto device : devices) {
        VkPhysicalDeviceProperties properties{};
        get_properties(device, &properties);
        dda::log(std::format("Vulkan GPU candidate: {} (type={})",
            properties.deviceName, static_cast<int>(properties.deviceType)));
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            selected = device;
            selected_properties = properties;
            break;
        }
    }
    g_state.physical_device = selected;
    dda::log(std::format("Selected physical device: {}", selected_properties.deviceName));
    return selected;
}

struct CapturedQueue {
    VkQueue queue{};
    VkDevice device{};
    uint32_t family{UINT32_MAX};
    uint32_t index{UINT32_MAX};
};
std::mutex g_queue_mutex;
std::vector<CapturedQueue> g_queues;

void remember_queue(VkDevice device, VkQueue queue, uint32_t family, uint32_t index) {
    std::scoped_lock lock(g_queue_mutex);
    for (auto& item : g_queues) {
        if (item.queue == queue) {
            item = {queue, device, family, index};
            return;
        }
    }
    g_queues.push_back({queue, device, family, index});
}

CapturedQueue find_queue(VkQueue queue) {
    std::scoped_lock lock(g_queue_mutex);
    for (const auto& item : g_queues) {
        if (item.queue == queue) return item;
    }
    return {};
}

VkQueueFlags queue_family_flags(VkPhysicalDevice physical_device, uint32_t family) {
    const auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        g_original_get_instance_proc_addr
            ? g_original_get_instance_proc_addr(g_state.instance, "vkGetPhysicalDeviceQueueFamilyProperties")
            : nullptr);
    if (!get_properties || !physical_device) return 0;
    uint32_t count = 0;
    get_properties(physical_device, &count, nullptr);
    if (family >= count) return 0;
    std::vector<VkQueueFamilyProperties> properties(count);
    get_properties(physical_device, &count, properties.data());
    return properties[family].queueFlags;
}

bool swapchain_allows_family(const CapturedSwapchain& swapchain, uint32_t family,
                             uint32_t presenting_family) {
    if (family == presenting_family) return true;
    if (swapchain.sharing_mode == VK_SHARING_MODE_EXCLUSIVE) return true;
    return std::find(swapchain.queue_families.begin(), swapchain.queue_families.end(), family)
        != swapchain.queue_families.end();
}

CapturedQueue find_overlay_queue(const CapturedQueue& presenting,
                                 const CapturedSwapchain& swapchain,
                                 VkPhysicalDevice physical_device) {
    const auto presenting_flags = queue_family_flags(physical_device, presenting.family);
    if (g_logged_present_family != presenting.family) {
        g_logged_present_family = presenting.family;
        dda::log(std::format("Present queue family capabilities: family={}, flags=0x{:X}",
            presenting.family, presenting_flags));
    }
    if ((presenting_flags & VK_QUEUE_GRAPHICS_BIT) != 0) return presenting;

    std::vector<DeviceQueueFamily> families;
    {
        std::scoped_lock lock(g_device_queue_family_mutex);
        for (const auto& family : g_device_queue_families) {
            if (family.device == presenting.device) families.push_back(family);
        }
    }
    if (!g_original_get_device_queue) return {};

    for (const auto& family : families) {
        const auto flags = queue_family_flags(physical_device, family.family);
        dda::log(std::format("Vulkan queue family capabilities: family={}, flags=0x{:X}",
            family.family, flags));
        if ((flags & VK_QUEUE_GRAPHICS_BIT) == 0 ||
            !swapchain_allows_family(swapchain, family.family, presenting.family))
            continue;
        for (uint32_t index = family.count; index-- > 0;) {
            if (family.family == presenting.family && index == presenting.index) continue;
            VkQueue queue{};
            g_original_get_device_queue(presenting.device, family.family, index, &queue);
            if (queue) {
                remember_queue(presenting.device, queue, family.family, index);
                return {queue, presenting.device, family.family, index};
            }
        }
    }
    return {};
}

VkResult VKAPI_PTR hooked_create_device(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkDevice* device) {
    const VkResult result = g_original_create_device(physical_device, create_info, allocator, device);
    if (result == VK_SUCCESS && device) {
        g_state.physical_device = physical_device;
        if (create_info) {
            std::scoped_lock lock(g_device_queue_family_mutex);
            for (uint32_t i = 0; i < create_info->queueCreateInfoCount; ++i) {
                const auto& queue_info = create_info->pQueueCreateInfos[i];
                g_device_queue_families.push_back(
                    {*device, queue_info.queueFamilyIndex, queue_info.queueCount});
                dda::log(std::format(
                    "Vulkan device queue family requested: family={}, count={}, flags=0x{:X}",
                    queue_info.queueFamilyIndex, queue_info.queueCount, queue_info.flags));
            }
        }
        dda::log(std::format("Vulkan device captured={}, requested queue families={}",
            static_cast<void*>(*device), create_info ? create_info->queueCreateInfoCount : 0));
    }
    return result;
}

VkResult VKAPI_PTR hooked_create_swapchain(
    VkDevice device, const VkSwapchainCreateInfoKHR* create_info,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
    const VkResult result = g_original_create_swapchain(device, create_info, allocator, swapchain);
    if (result == VK_SUCCESS && create_info && swapchain) {
        remember_swapchain(device, *swapchain, *create_info);
        dda::log(std::format(
            "Vulkan swapchain captured={}, format={}, extent={}x{}, usage=0x{:X}, sharing={}, families={}, minImages={}",
            static_cast<void*>(*swapchain), static_cast<int>(create_info->imageFormat),
            create_info->imageExtent.width, create_info->imageExtent.height,
            create_info->imageUsage, static_cast<int>(create_info->imageSharingMode),
            create_info->queueFamilyIndexCount, create_info->minImageCount));
    }
    return result;
}

void VKAPI_PTR hooked_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain,
                                        const VkAllocationCallbacks* allocator) {
    dda::imgui_overlay_before_swapchain_destroy(device, swapchain);
    g_original_destroy_swapchain(device, swapchain, allocator);
    forget_swapchain(swapchain);
}

void VKAPI_PTR hooked_get_device_queue(
    VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    g_original_get_device_queue(device, family, index, queue);
    if (queue && *queue) {
        remember_queue(device, *queue, family, index);
        dda::log(std::format("Vulkan queue captured={}, family={}, index={}",
            static_cast<void*>(*queue), family, index));
    }
}

void VKAPI_PTR hooked_get_device_queue2(
    VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    g_original_get_device_queue2(device, info, queue);
    if (info && queue && *queue) {
        remember_queue(device, *queue, info->queueFamilyIndex, info->queueIndex);
        dda::log(std::format("Vulkan queue2 captured={}, family={}, index={}",
            static_cast<void*>(*queue), info->queueFamilyIndex, info->queueIndex));
    }
}

VkResult VKAPI_PTR hooked_queue_present(VkQueue queue, const VkPresentInfoKHR* present_info) {
    dda::update_plugin();
    dda::poll_addon_ui_hotkey();
    const auto count = ++g_present_calls;
    if (count == 1) g_first_present_queue = queue;
    if (!g_notification_render_ready &&
        ((g_first_present_queue && queue != g_first_present_queue) || count >= 600)) {
        g_notification_render_ready = true;
        dda::notification_state().set_render_ready();
        dda::log("Notification rendering enabled after boot presentation phase");
    }
    if (count == 1) {
        dda::load_addon_preferences();
        const auto result = dda::unlock_console_commands();
        bool success = result != dda::ConsoleUnlockResult::failed;
        const bool preferred_unlocked = dda::addon_console_unlock_preference();
        if (success && !preferred_unlocked) success = dda::set_console_commands_unlocked(false);
        if (!success) dda::log("Saved console state could not be applied");
        dda::notification_state().show_console_state_result(
            success, preferred_unlocked,
            result == dda::ConsoleUnlockResult::already_unlocked,
            dda::addon_notification_duration_seconds());
    }
    if (count == 1) dda::log("Vulkan presentation hook active");
    if (count == 1) {
        const auto presenting_queue = find_queue(queue);
        const VkSwapchainKHR presented = present_info && present_info->swapchainCount
            ? present_info->pSwapchains[0] : VK_NULL_HANDLE;
        const auto presented_state = find_swapchain(presented);
        dda::log(std::format(
            "Presentation state queue={}, family={}, index={}, device={}, presented={}, format={}, extent={}x{}",
            static_cast<void*>(queue), presenting_queue.family, presenting_queue.index, static_cast<void*>(presenting_queue.device),
            static_cast<void*>(presented), static_cast<int>(presented_state.format),
            presented_state.extent.width, presented_state.extent.height));
    }

    if (present_info && (dda::addon_ui_wants_render() || dda::notification_state().visible())) {
        const auto presenting_queue = find_queue(queue);
        const auto physical_device = resolve_physical_device();
        const VkSwapchainKHR presented = present_info->swapchainCount
            ? present_info->pSwapchains[0] : VK_NULL_HANDLE;
        const auto presented_state = find_swapchain(presented);
        if (presented == g_incompatible_overlay_swapchain)
            return g_original_queue_present(queue, present_info);
        if (presenting_queue.device && presenting_queue.family != UINT32_MAX &&
            g_state.instance && physical_device && presented_state.swapchain) {
            const auto overlay_queue = find_overlay_queue(
                presenting_queue, presented_state, physical_device);
            if (!overlay_queue.queue) {
                g_incompatible_overlay_swapchain = presented;
                dda::close_addon_ui();
                if (!g_missing_state_logged) {
                    g_missing_state_logged = true;
                    dda::log("Overlay refused: no compatible graphics queue is available");
                }
                return g_original_queue_present(queue, present_info);
            }
            dda::VulkanOverlayInfo overlay_info{};
            overlay_info.instance = g_state.instance;
            overlay_info.physical_device = physical_device;
            overlay_info.device = presenting_queue.device;
            overlay_info.queue = overlay_queue.queue;
            overlay_info.queue_family = overlay_queue.family;
            overlay_info.present_queue = queue;
            overlay_info.present_queue_family = presenting_queue.family;
            overlay_info.swapchain = presented_state.swapchain;
            overlay_info.format = presented_state.format;
            overlay_info.extent = presented_state.extent;
            overlay_info.image_usage = presented_state.image_usage;
            overlay_info.sharing_mode = presented_state.sharing_mode;
            overlay_info.min_image_count = presented_state.min_image_count;
            overlay_info.get_instance_proc_addr = g_original_get_instance_proc_addr;
            overlay_info.get_device_proc_addr = g_original_get_device_proc_addr;
            if (g_logged_overlay_queue != overlay_queue.queue) {
                g_logged_overlay_queue = overlay_queue.queue;
                dda::log(std::format("Overlay submission queue selected: family={}, index={}",
                    overlay_queue.family, overlay_queue.index));
            }
            VkPresentInfoKHR patched{};
            VkSemaphore complete{};
            if (dda::render_imgui_overlay(overlay_info, *present_info, patched, complete))
                return g_original_queue_present(queue, &patched);
        } else if (!g_missing_state_logged) {
            g_missing_state_logged = true;
            dda::log(std::format(
                "Overlay prerequisites missing: instance={}, physicalDevice={}, device={}, family={}, presentedSwapchain={}",
                static_cast<void*>(g_state.instance), static_cast<void*>(physical_device),
                static_cast<void*>(presenting_queue.device), presenting_queue.family,
                static_cast<void*>(presented_state.swapchain)));
        }
    }
    return g_original_queue_present(queue, present_info);
}

PFN_vkVoidFunction VKAPI_PTR hooked_get_device_proc_addr(VkDevice device, const char* name) {
    const auto function = g_original_get_device_proc_addr(device, name);
    if (name && std::strcmp(name, "vkQueuePresentKHR") == 0 && function) {
        if (!g_direct_hooks) g_original_queue_present = reinterpret_cast<QueuePresentFn>(function);
        return reinterpret_cast<PFN_vkVoidFunction>(&hooked_queue_present);
    }
    if (name && std::strcmp(name, "vkCreateSwapchainKHR") == 0 && function) {
        if (!g_direct_hooks) g_original_create_swapchain = reinterpret_cast<CreateSwapchainFn>(function);
        return reinterpret_cast<PFN_vkVoidFunction>(&hooked_create_swapchain);
    }
    if (name && std::strcmp(name, "vkDestroySwapchainKHR") == 0 && function) {
        if (!g_direct_hooks) g_original_destroy_swapchain = reinterpret_cast<DestroySwapchainFn>(function);
        return reinterpret_cast<PFN_vkVoidFunction>(&hooked_destroy_swapchain);
    }
    if (name && std::strcmp(name, "vkGetDeviceQueue") == 0 && function) {
        if (!g_direct_hooks) g_original_get_device_queue = reinterpret_cast<GetDeviceQueueFn>(function);
        return reinterpret_cast<PFN_vkVoidFunction>(&hooked_get_device_queue);
    }
    if (name && std::strcmp(name, "vkGetDeviceQueue2") == 0 && function) {
        if (!g_direct_hooks) g_original_get_device_queue2 = reinterpret_cast<GetDeviceQueue2Fn>(function);
        return reinterpret_cast<PFN_vkVoidFunction>(&hooked_get_device_queue2);
    }
    return function;
}

PFN_vkVoidFunction VKAPI_PTR hooked_get_instance_proc_addr(VkInstance instance, const char* name) {
    const auto function = g_original_get_instance_proc_addr(instance, name);
    if (instance) g_state.instance = instance;
    if (name && std::strcmp(name, "vkQueuePresentKHR") == 0 && function) {
        if (!g_direct_hooks) g_original_queue_present = reinterpret_cast<QueuePresentFn>(function);
        dda::log("vkQueuePresentKHR captured through vkGetInstanceProcAddr");
        return reinterpret_cast<PFN_vkVoidFunction>(&hooked_queue_present);
    }
    if (name && std::strcmp(name, "vkGetDeviceProcAddr") == 0 && function) {
        return reinterpret_cast<PFN_vkVoidFunction>(&hooked_get_device_proc_addr);
    }
    if (name && std::strcmp(name, "vkCreateDevice") == 0 && function) {
        if (!g_direct_hooks) g_original_create_device = reinterpret_cast<CreateDeviceFn>(function);
        return reinterpret_cast<PFN_vkVoidFunction>(&hooked_create_device);
    }
    return function;
}
}

namespace dda {
bool install_vulkan_hook() {
    if (MH_Initialize() != MH_OK) {
        log("MinHook initialization failed");
        return false;
    }

    const auto vulkan = GetModuleHandleW(L"vulkan-1.dll");
    if (!vulkan) {
        log("vulkan-1.dll is not loaded");
        return false;
    }

    const auto get_device_proc_addr = GetProcAddress(vulkan, "vkGetDeviceProcAddr");
    const auto get_instance_proc_addr = GetProcAddress(vulkan, "vkGetInstanceProcAddr");
    const auto queue_present = GetProcAddress(vulkan, "vkQueuePresentKHR");
    const auto create_device = GetProcAddress(vulkan, "vkCreateDevice");
    const auto create_swapchain = GetProcAddress(vulkan, "vkCreateSwapchainKHR");
    const auto destroy_swapchain = GetProcAddress(vulkan, "vkDestroySwapchainKHR");
    const auto get_device_queue = GetProcAddress(vulkan, "vkGetDeviceQueue");
    const auto get_device_queue2 = GetProcAddress(vulkan, "vkGetDeviceQueue2");
    if (!get_device_proc_addr || !get_instance_proc_addr || !queue_present || !create_device ||
        !create_swapchain || !destroy_swapchain || !get_device_queue) {
        log("Required Vulkan loader exports were not found");
        return false;
    }

    if (MH_CreateHook(get_device_proc_addr, &hooked_get_device_proc_addr,
            reinterpret_cast<void**>(&g_original_get_device_proc_addr)) != MH_OK ||
        MH_CreateHook(get_instance_proc_addr, &hooked_get_instance_proc_addr,
            reinterpret_cast<void**>(&g_original_get_instance_proc_addr)) != MH_OK ||
        MH_CreateHook(queue_present, &hooked_queue_present,
            reinterpret_cast<void**>(&g_original_queue_present)) != MH_OK ||
        MH_CreateHook(create_device, &hooked_create_device,
            reinterpret_cast<void**>(&g_original_create_device)) != MH_OK ||
        MH_CreateHook(create_swapchain, &hooked_create_swapchain,
            reinterpret_cast<void**>(&g_original_create_swapchain)) != MH_OK ||
        MH_CreateHook(destroy_swapchain, &hooked_destroy_swapchain,
            reinterpret_cast<void**>(&g_original_destroy_swapchain)) != MH_OK ||
        MH_CreateHook(get_device_queue, &hooked_get_device_queue,
            reinterpret_cast<void**>(&g_original_get_device_queue)) != MH_OK ||
        (get_device_queue2 && MH_CreateHook(get_device_queue2, &hooked_get_device_queue2,
            reinterpret_cast<void**>(&g_original_get_device_queue2)) != MH_OK)) {
        log("Direct Vulkan hook creation failed");
        MH_Uninitialize();
        return false;
    }
    g_direct_hooks = true;
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        log("Vulkan hook activation failed");
        g_direct_hooks = false;
        MH_Uninitialize();
        return false;
    }

    log("Direct Vulkan and proc-address hooks installed");
    return true;
}

}
