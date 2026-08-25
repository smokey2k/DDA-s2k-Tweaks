#include "imgui_overlay.h"

#include "log.h"
#include "notification_state.h"
#include "addon_ui.h"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <vector>

namespace {
struct FrameResources {
    VkImage image{};
    VkImageView view{};
    VkFramebuffer framebuffer{};
    VkCommandBuffer command_buffer{};
    VkCommandBuffer acquire_graphics_command{};
    VkCommandBuffer acquire_present_command{};
    VkFence fence{};
    VkFence present_fence{};
    VkSemaphore complete{};
    VkSemaphore graphics_ready{};
    VkSemaphore present_ready{};
};

struct OverlayState {
    bool initialized{};
    bool failed{};
    dda::VulkanOverlayInfo info{};
    VkRenderPass render_pass{};
    VkCommandPool command_pool{};
    VkCommandPool ownership_command_pool{};
    std::vector<FrameResources> frames;
    std::chrono::steady_clock::time_point previous{};
};
OverlayState g;

template <typename T>
T device_fn(const char* name) {
    return reinterpret_cast<T>(g.info.get_device_proc_addr(g.info.device, name));
}

PFN_vkVoidFunction imgui_loader(const char* name, void*) {
    if (g.info.device && g.info.get_device_proc_addr) {
        if (auto fn = g.info.get_device_proc_addr(g.info.device, name)) return fn;
    }
    return g.info.get_instance_proc_addr
        ? g.info.get_instance_proc_addr(g.info.instance, name) : nullptr;
}

bool vk_ok(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) return true;
    dda::log(std::format("{} failed ({})", operation, static_cast<int>(result)));
    return false;
}

bool initialize(const dda::VulkanOverlayInfo& info) {
    if ((info.image_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
        dda::log(std::format("Overlay refused: swapchain image usage {} lacks COLOR_ATTACHMENT", info.image_usage));
        return false;
    }
    g.info = info;
    auto get_images = device_fn<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
    auto create_view = device_fn<PFN_vkCreateImageView>("vkCreateImageView");
    auto create_render_pass = device_fn<PFN_vkCreateRenderPass>("vkCreateRenderPass");
    auto create_framebuffer = device_fn<PFN_vkCreateFramebuffer>("vkCreateFramebuffer");
    auto create_pool = device_fn<PFN_vkCreateCommandPool>("vkCreateCommandPool");
    auto allocate_commands = device_fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers");
    auto create_fence = device_fn<PFN_vkCreateFence>("vkCreateFence");
    auto create_semaphore = device_fn<PFN_vkCreateSemaphore>("vkCreateSemaphore");
    if (!get_images || !create_view || !create_render_pass || !create_framebuffer ||
        !create_pool || !allocate_commands || !create_fence || !create_semaphore) {
        dda::log("Required Vulkan functions are unavailable");
        return false;
    }

    uint32_t image_count = 0;
    if (!vk_ok(get_images(info.device, info.swapchain, &image_count, nullptr), "vkGetSwapchainImagesKHR(count)") || !image_count)
        return false;
    std::vector<VkImage> images(image_count);
    if (!vk_ok(get_images(info.device, info.swapchain, &image_count, images.data()), "vkGetSwapchainImagesKHR(images)"))
        return false;
    g.frames.resize(image_count);

    VkAttachmentDescription attachment{};
    attachment.format = info.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo render_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    render_info.attachmentCount = 1;
    render_info.pAttachments = &attachment;
    render_info.subpassCount = 1;
    render_info.pSubpasses = &subpass;
    render_info.dependencyCount = 1;
    render_info.pDependencies = &dependency;
    if (!vk_ok(create_render_pass(info.device, &render_info, nullptr, &g.render_pass), "vkCreateRenderPass")) return false;

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = info.queue_family;
    if (!vk_ok(create_pool(info.device, &pool_info, nullptr, &g.command_pool), "vkCreateCommandPool")) return false;
    std::vector<VkCommandBuffer> commands(image_count);
    VkCommandBufferAllocateInfo alloc_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info.commandPool = g.command_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = image_count;
    if (!vk_ok(allocate_commands(info.device, &alloc_info, commands.data()), "vkAllocateCommandBuffers")) return false;

    const bool needs_ownership_transfer = info.sharing_mode == VK_SHARING_MODE_EXCLUSIVE &&
        info.queue_family != info.present_queue_family;
    std::vector<VkCommandBuffer> ownership_commands;
    if (needs_ownership_transfer) {
        pool_info.queueFamilyIndex = info.present_queue_family;
        if (!vk_ok(create_pool(info.device, &pool_info, nullptr, &g.ownership_command_pool),
                   "vkCreateCommandPool(ownership)")) return false;
        ownership_commands.resize(image_count * 2);
        alloc_info.commandPool = g.ownership_command_pool;
        alloc_info.commandBufferCount = static_cast<uint32_t>(ownership_commands.size());
        if (!vk_ok(allocate_commands(info.device, &alloc_info, ownership_commands.data()),
                   "vkAllocateCommandBuffers(ownership)")) return false;
    }

    for (uint32_t i = 0; i < image_count; ++i) {
        auto& frame = g.frames[i];
        frame.image = images[i];
        frame.command_buffer = commands[i];
        if (needs_ownership_transfer) {
            frame.acquire_graphics_command = ownership_commands[i * 2];
            frame.acquire_present_command = ownership_commands[i * 2 + 1];
        }
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = info.format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        if (!vk_ok(create_view(info.device, &view_info, nullptr, &frame.view), "vkCreateImageView")) return false;
        VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb_info.renderPass = g.render_pass;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments = &frame.view;
        fb_info.width = info.extent.width;
        fb_info.height = info.extent.height;
        fb_info.layers = 1;
        if (!vk_ok(create_framebuffer(info.device, &fb_info, nullptr, &frame.framebuffer), "vkCreateFramebuffer")) return false;
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!vk_ok(create_fence(info.device, &fence_info, nullptr, &frame.fence), "vkCreateFence")) return false;
        if (needs_ownership_transfer &&
            !vk_ok(create_fence(info.device, &fence_info, nullptr, &frame.present_fence),
                   "vkCreateFence(present ownership)")) return false;
        VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!vk_ok(create_semaphore(info.device, &semaphore_info, nullptr, &frame.complete), "vkCreateSemaphore")) return false;
        if (needs_ownership_transfer &&
            (!vk_ok(create_semaphore(info.device, &semaphore_info, nullptr, &frame.graphics_ready),
                    "vkCreateSemaphore(graphics ownership)") ||
             !vk_ok(create_semaphore(info.device, &semaphore_info, nullptr, &frame.present_ready),
                    "vkCreateSemaphore(present ownership)"))) return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.WindowBorderSize = 1.0f;
    HWND window = GetForegroundWindow();
    DWORD window_process{};
    GetWindowThreadProcessId(window, &window_process);
    if (window_process != GetCurrentProcessId() || !dda::initialize_addon_ui(window)) {
        dda::log("Failed to initialize Dear ImGui Win32 input");
        return false;
    }
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, imgui_loader, nullptr)) {
        dda::log("Dear ImGui Vulkan function loading failed");
        return false;
    }
    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_3;
    init.Instance = info.instance;
    init.PhysicalDevice = info.physical_device;
    init.Device = info.device;
    init.QueueFamily = info.queue_family;
    init.Queue = info.queue;
    init.DescriptorPoolSize = 16;
    init.MinImageCount = std::max(2u, info.min_image_count);
    init.ImageCount = image_count;
    init.PipelineInfoMain.RenderPass = g.render_pass;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&init)) {
        dda::log("ImGui_ImplVulkan_Init failed");
        return false;
    }
    g.previous = std::chrono::steady_clock::now();
    g.initialized = true;
    dda::log(std::format("Dear ImGui Vulkan overlay initialized ({} images, ownershipTransfer={})",
        image_count, needs_ownership_transfer));
    return true;
}

void shutdown() {
    if (!g.initialized) return;
    const auto wait_fences = device_fn<PFN_vkWaitForFences>("vkWaitForFences");
    const auto destroy_semaphore = device_fn<PFN_vkDestroySemaphore>("vkDestroySemaphore");
    const auto destroy_fence = device_fn<PFN_vkDestroyFence>("vkDestroyFence");
    const auto destroy_framebuffer = device_fn<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer");
    const auto destroy_view = device_fn<PFN_vkDestroyImageView>("vkDestroyImageView");
    const auto destroy_pool = device_fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool");
    const auto destroy_render_pass = device_fn<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
    if (wait_fences) {
        for (const auto& frame : g.frames) {
            if (frame.fence) wait_fences(g.info.device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
            if (frame.present_fence) wait_fences(g.info.device, 1, &frame.present_fence, VK_TRUE, UINT64_MAX);
        }
    }
    ImGui_ImplVulkan_Shutdown();
    dda::shutdown_addon_ui();
    ImGui::DestroyContext();
    for (auto& frame : g.frames) {
        if (frame.complete && destroy_semaphore) destroy_semaphore(g.info.device, frame.complete, nullptr);
        if (frame.graphics_ready && destroy_semaphore) destroy_semaphore(g.info.device, frame.graphics_ready, nullptr);
        if (frame.present_ready && destroy_semaphore) destroy_semaphore(g.info.device, frame.present_ready, nullptr);
        if (frame.fence && destroy_fence) destroy_fence(g.info.device, frame.fence, nullptr);
        if (frame.present_fence && destroy_fence) destroy_fence(g.info.device, frame.present_fence, nullptr);
        if (frame.framebuffer && destroy_framebuffer) destroy_framebuffer(g.info.device, frame.framebuffer, nullptr);
        if (frame.view && destroy_view) destroy_view(g.info.device, frame.view, nullptr);
    }
    if (g.command_pool && destroy_pool) destroy_pool(g.info.device, g.command_pool, nullptr);
    if (g.ownership_command_pool && destroy_pool)
        destroy_pool(g.info.device, g.ownership_command_pool, nullptr);
    if (g.render_pass && destroy_render_pass) destroy_render_pass(g.info.device, g.render_pass, nullptr);
    g = {};
    dda::log("Dear ImGui Vulkan resources released for swapchain change");
}
} // namespace

namespace dda {
void imgui_overlay_before_swapchain_destroy(VkDevice device, VkSwapchainKHR swapchain) {
    if (g.initialized && g.info.device == device && g.info.swapchain == swapchain) shutdown();
}

bool render_imgui_overlay(const VulkanOverlayInfo& info, const VkPresentInfoKHR& source,
                          VkPresentInfoKHR& patched, VkSemaphore& overlay_complete) {
    if (g.failed || source.swapchainCount != 1 || !source.pImageIndices ||
        source.pSwapchains[0] != info.swapchain) return false;
    if (g.initialized && (g.info.swapchain != info.swapchain || g.info.extent.width != info.extent.width ||
                          g.info.extent.height != info.extent.height || g.info.format != info.format ||
                          g.info.queue != info.queue || g.info.queue_family != info.queue_family ||
                          g.info.present_queue != info.present_queue ||
                          g.info.present_queue_family != info.present_queue_family ||
                          g.info.sharing_mode != info.sharing_mode)) {
        shutdown();
        dda::log("Swapchain change detected at present; overlay will be rebuilt");
    }
    if (!g.initialized && !initialize(info)) {
        g.failed = true;
        return false;
    }
    const uint32_t index = source.pImageIndices[0];
    if (index >= g.frames.size()) return false;
    auto& frame = g.frames[index];
    auto wait_fences = device_fn<PFN_vkWaitForFences>("vkWaitForFences");
    auto reset_fences = device_fn<PFN_vkResetFences>("vkResetFences");
    auto reset_command = device_fn<PFN_vkResetCommandBuffer>("vkResetCommandBuffer");
    auto begin_command = device_fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
    auto begin_render_pass = device_fn<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass");
    auto end_render_pass = device_fn<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass");
    auto pipeline_barrier = device_fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    auto end_command = device_fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
    auto queue_submit = device_fn<PFN_vkQueueSubmit>("vkQueueSubmit");
    if (!wait_fences || !reset_fences || !reset_command || !begin_command || !begin_render_pass ||
        !end_render_pass || !pipeline_barrier || !end_command || !queue_submit) return false;
    const bool needs_ownership_transfer = info.sharing_mode == VK_SHARING_MODE_EXCLUSIVE &&
        info.queue_family != info.present_queue_family;
    if (!vk_ok(wait_fences(info.device, 1, &frame.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences") ||
        !vk_ok(reset_fences(info.device, 1, &frame.fence), "vkResetFences") ||
        !vk_ok(reset_command(frame.command_buffer, 0), "vkResetCommandBuffer")) return false;
    if (needs_ownership_transfer &&
        (!vk_ok(wait_fences(info.device, 1, &frame.present_fence, VK_TRUE, UINT64_MAX),
                "vkWaitForFences(present ownership)") ||
         !vk_ok(reset_fences(info.device, 1, &frame.present_fence),
                "vkResetFences(present ownership)") ||
         !vk_ok(reset_command(frame.acquire_graphics_command, 0),
                "vkResetCommandBuffer(graphics ownership)") ||
         !vk_ok(reset_command(frame.acquire_present_command, 0),
                "vkResetCommandBuffer(present ownership)"))) return false;

    const auto now = std::chrono::steady_clock::now();
    ImGui_ImplVulkan_NewFrame();
    addon_ui_new_frame();
    auto& io = ImGui::GetIO();
    io.DisplaySize = {static_cast<float>(info.extent.width), static_cast<float>(info.extent.height)};
    io.DeltaTime = std::max(1.0f / 1000.0f, std::chrono::duration<float>(now - g.previous).count());
    g.previous = now;
    ImGui::NewFrame();
    const auto notifications = notification_state().snapshots();
    if (!notifications.empty()) {
        const ImVec2 padding(28.0f, 18.0f);
        constexpr float spacing = 8.0f;
        std::vector<ImVec2> sizes;
        sizes.reserve(notifications.size());
        float group_width = 0.0f;
        float group_height = 0.0f;
        for (const auto& notification : notifications) {
            const ImVec2 text_size = ImGui::CalcTextSize(notification.text.c_str());
            const ImVec2 size{text_size.x + padding.x * 2.0f, text_size.y + padding.y * 2.0f};
            sizes.push_back(size);
            group_width = std::max(group_width, size.x);
            group_height += size.y;
        }
        group_height += spacing * static_cast<float>(notifications.size() - 1);
        const float desired_left = io.DisplaySize.x * (addon_notification_x_percent() / 100.0f) - group_width * 0.5f;
        const float desired_top = io.DisplaySize.y * (addon_notification_y_percent() / 100.0f) - sizes.front().y * 0.5f;
        const float group_left = std::clamp(desired_left, 0.0f, std::max(0.0f, io.DisplaySize.x - group_width));
        float current_y = std::clamp(desired_top, 0.0f, std::max(0.0f, io.DisplaySize.y - group_height));
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
        for (std::size_t i = 0; i < notifications.size(); ++i) {
            const float x = group_left + (group_width - sizes[i].x) * 0.5f;
            ImGui::SetNextWindowPos({x, current_y}, ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.88f);
            ImGui::SetNextWindowSize(sizes[i]);
            const std::string id = std::format("##s2kNotification{}", i);
            ImGui::Begin(id.c_str(), nullptr, flags);
            ImGui::SetCursorPos(padding);
            ImGui::TextUnformatted(notifications[i].text.c_str());
            ImGui::End();
            current_y += sizes[i].y + spacing;
        }
    }
    render_addon_ui();
    ImGui::Render();

    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    auto ownership_barrier = [&](VkCommandBuffer command, uint32_t source_family,
                                 uint32_t destination_family, VkAccessFlags source_access,
                                 VkAccessFlags destination_access) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = source_access;
        barrier.dstAccessMask = destination_access;
        barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcQueueFamilyIndex = source_family;
        barrier.dstQueueFamilyIndex = destination_family;
        barrier.image = frame.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        pipeline_barrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    };

    if (needs_ownership_transfer) {
        if (!vk_ok(begin_command(frame.acquire_graphics_command, &begin_info),
                   "vkBeginCommandBuffer(release to graphics)")) return false;
        ownership_barrier(frame.acquire_graphics_command, info.present_queue_family,
            info.queue_family, 0, 0);
        if (!vk_ok(end_command(frame.acquire_graphics_command),
                   "vkEndCommandBuffer(release to graphics)")) return false;
    }

    if (!vk_ok(begin_command(frame.command_buffer, &begin_info), "vkBeginCommandBuffer")) return false;
    if (needs_ownership_transfer) {
        ownership_barrier(frame.command_buffer, info.present_queue_family, info.queue_family,
            0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    }
    VkRenderPassBeginInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass_info.renderPass = g.render_pass;
    pass_info.framebuffer = frame.framebuffer;
    pass_info.renderArea.extent = info.extent;
    begin_render_pass(frame.command_buffer, &pass_info, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.command_buffer);
    end_render_pass(frame.command_buffer);
    if (needs_ownership_transfer) {
        ownership_barrier(frame.command_buffer, info.queue_family, info.present_queue_family,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
    }
    if (!vk_ok(end_command(frame.command_buffer), "vkEndCommandBuffer")) return false;

    if (needs_ownership_transfer) {
        if (!vk_ok(begin_command(frame.acquire_present_command, &begin_info),
                   "vkBeginCommandBuffer(acquire present)")) return false;
        ownership_barrier(frame.acquire_present_command, info.queue_family,
            info.present_queue_family, 0, 0);
        if (!vk_ok(end_command(frame.acquire_present_command),
                   "vkEndCommandBuffer(acquire present)")) return false;
    }

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (needs_ownership_transfer) {
        std::vector<VkPipelineStageFlags> present_stages(
            source.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        submit.waitSemaphoreCount = source.waitSemaphoreCount;
        submit.pWaitSemaphores = source.pWaitSemaphores;
        submit.pWaitDstStageMask = present_stages.empty() ? nullptr : present_stages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame.acquire_graphics_command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &frame.graphics_ready;
        if (!vk_ok(queue_submit(info.present_queue, 1, &submit, VK_NULL_HANDLE),
                   "vkQueueSubmit(release to graphics)")) return false;

        const VkPipelineStageFlags graphics_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        submit.pNext = nullptr;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.graphics_ready;
        submit.pWaitDstStageMask = &graphics_stage;
        submit.pCommandBuffers = &frame.command_buffer;
        submit.pSignalSemaphores = &frame.complete;
        if (!vk_ok(queue_submit(info.queue, 1, &submit, frame.fence),
                   "vkQueueSubmit(overlay graphics)")) return false;

        const VkPipelineStageFlags present_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        submit.pWaitSemaphores = &frame.complete;
        submit.pWaitDstStageMask = &present_stage;
        submit.pCommandBuffers = &frame.acquire_present_command;
        submit.pSignalSemaphores = &frame.present_ready;
        if (!vk_ok(queue_submit(info.present_queue, 1, &submit, frame.present_fence),
                   "vkQueueSubmit(acquire present)")) return false;
        overlay_complete = frame.present_ready;
    } else {
        std::vector<VkPipelineStageFlags> stages(
            source.waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        submit.waitSemaphoreCount = source.waitSemaphoreCount;
        submit.pWaitSemaphores = source.pWaitSemaphores;
        submit.pWaitDstStageMask = stages.empty() ? nullptr : stages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame.command_buffer;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &frame.complete;
        if (!vk_ok(queue_submit(info.queue, 1, &submit, frame.fence),
                   "vkQueueSubmit(overlay)")) return false;
        overlay_complete = frame.complete;
    }
    patched = source;
    patched.waitSemaphoreCount = 1;
    patched.pWaitSemaphores = &overlay_complete;
    return true;
}
} // namespace dda
