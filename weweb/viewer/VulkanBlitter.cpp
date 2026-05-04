#include "VulkanBlitter.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#include <GLFW/glfw3.h>

namespace weweb {

namespace {

#define VK_CHECK(expr)                                                       \
    do {                                                                     \
        VkResult _r = (expr);                                                \
        if (_r != VK_SUCCESS) {                                              \
            std::fprintf(stderr,                                             \
                         "weweb: %s failed (VkResult=%d) at %s:%d\n",        \
                         #expr, static_cast<int>(_r), __FILE__, __LINE__);   \
            return false;                                                    \
        }                                                                    \
    } while (0)

#define VK_CHECK_VOID(expr)                                                  \
    do {                                                                     \
        VkResult _r = (expr);                                                \
        if (_r != VK_SUCCESS) {                                              \
            std::fprintf(stderr,                                             \
                         "weweb: %s failed (VkResult=%d) at %s:%d\n",        \
                         #expr, static_cast<int>(_r), __FILE__, __LINE__);   \
        }                                                                    \
    } while (0)

constexpr std::uint64_t kFenceTimeoutNs = 5'000'000'000ull;  // 5s

}  // namespace

VulkanBlitter::VulkanBlitter() = default;

VulkanBlitter::~VulkanBlitter() {
    Shutdown();
}

bool VulkanBlitter::Init(GLFWwindow* window) {
    window_ = window;
    return CreateInstance()
        && CreateSurface(window)
        && PickPhysicalDevice()
        && CreateDevice()
        && CreateCommandPool()
        && CreateSwapchain()
        && CreateStagingBuffer()
        && CreateSyncObjects();
}

void VulkanBlitter::Shutdown() {
    if (device_) vkDeviceWaitIdle(device_);

    for (auto& s : img_avail_sem_)  if (s) { vkDestroySemaphore(device_, s, nullptr); s = VK_NULL_HANDLE; }
    for (auto& s : render_done_sem_) if (s) { vkDestroySemaphore(device_, s, nullptr); s = VK_NULL_HANDLE; }
    for (auto& f : in_flight_fence_) if (f) { vkDestroyFence(device_, f, nullptr); f = VK_NULL_HANDLE; }

    if (cmd_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, cmd_pool_, nullptr);
        cmd_pool_ = VK_NULL_HANDLE;
    }
    DestroyStagingBuffer();
    DestroySwapchain();
    if (device_)   { vkDestroyDevice(device_, nullptr);                device_   = VK_NULL_HANDLE; }
    if (surface_ && instance_) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (instance_) { vkDestroyInstance(instance_, nullptr);            instance_ = VK_NULL_HANDLE; }
}

bool VulkanBlitter::CreateInstance() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "weweb";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName        = "weweb-blitter";
    app.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion         = VK_API_VERSION_1_1;

    std::uint32_t glfw_count = 0;
    const char** glfw_exts = glfwGetRequiredInstanceExtensions(&glfw_count);
    if (!glfw_exts) {
        std::fprintf(stderr,
                     "weweb: glfwGetRequiredInstanceExtensions failed "
                     "(no Vulkan-capable windowing backend)\n");
        return false;
    }

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &app;
    ci.enabledExtensionCount   = glfw_count;
    ci.ppEnabledExtensionNames = glfw_exts;

    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));
    return true;
}

bool VulkanBlitter::CreateSurface(GLFWwindow* window) {
    VK_CHECK(glfwCreateWindowSurface(instance_, window, nullptr, &surface_));
    return true;
}

bool VulkanBlitter::PickPhysicalDevice() {
    std::uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
    if (count == 0) {
        std::fprintf(stderr, "weweb: no Vulkan physical devices\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, devs.data()));

    // Pick the first device that has a queue family supporting both
    // graphics and surface present + the swapchain extension.
    for (auto pd : devs) {
        std::uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qfp(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, qfp.data());

        for (std::uint32_t i = 0; i < qcount; ++i) {
            if (!(qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface_, &present);
            if (!present) continue;

            std::uint32_t ecount = 0;
            vkEnumerateDeviceExtensionProperties(pd, nullptr, &ecount, nullptr);
            std::vector<VkExtensionProperties> exts(ecount);
            vkEnumerateDeviceExtensionProperties(pd, nullptr, &ecount, exts.data());
            bool has_swapchain = false;
            for (auto& e : exts) {
                if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                    has_swapchain = true;
                    break;
                }
            }
            if (!has_swapchain) continue;

            phys_ = pd;
            queue_family_ = i;
            vkGetPhysicalDeviceMemoryProperties(phys_, &mem_props_);
            return true;
        }
    }
    std::fprintf(stderr, "weweb: no suitable Vulkan device\n");
    return false;
}

bool VulkanBlitter::CreateDevice() {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = queue_family_;
    qi.queueCount       = 1;
    qi.pQueuePriorities = &prio;

    const char* dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount    = 1;
    ci.pQueueCreateInfos       = &qi;
    ci.enabledExtensionCount   = 1;
    ci.ppEnabledExtensionNames = dev_exts;

    VK_CHECK(vkCreateDevice(phys_, &ci, nullptr, &device_));
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return true;
}

bool VulkanBlitter::CreateCommandPool() {
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = queue_family_;
    VK_CHECK(vkCreateCommandPool(device_, &ci, nullptr, &cmd_pool_));

    VkCommandBufferAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = cmd_pool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kMaxFramesInFlight;
    VK_CHECK(vkAllocateCommandBuffers(device_, &ai, cmd_bufs_));
    return true;
}

bool VulkanBlitter::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps));

    // Pick BGRA8_UNORM if available — matches CEF's byte layout 1:1 with
    // no implicit SRGB conversion. Otherwise take the surface default.
    std::uint32_t fcount = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fcount, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(fcount);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fcount, formats.data()));

    swap_format_     = formats[0].format;
    swap_colorspace_ = formats[0].colorSpace;
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) {
            swap_format_     = f.format;
            swap_colorspace_ = f.colorSpace;
            break;
        }
    }

    // FIFO is universally supported; gives us vsync, perfect for a
    // wallpaper. No need for MAILBOX.
    present_mode_ = VK_PRESENT_MODE_FIFO_KHR;

    // Resolve extent.
    if (caps.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        extent_ = caps.currentExtent;
    } else {
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        extent_.width  = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(w),
                            caps.minImageExtent.width,  caps.maxImageExtent.width);
        extent_.height = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(h),
                            caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent_.width == 0 || extent_.height == 0) {
        // Window iconified — caller will retry.
        return true;
    }

    std::uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = surface_;
    ci.minImageCount    = image_count;
    ci.imageFormat      = swap_format_;
    ci.imageColorSpace  = swap_colorspace_;
    ci.imageExtent      = extent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                        | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = present_mode_;
    ci.clipped          = VK_TRUE;
    ci.oldSwapchain     = VK_NULL_HANDLE;

    VK_CHECK(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_));

    std::uint32_t scount = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(device_, swapchain_, &scount, nullptr));
    swap_images_.resize(scount);
    VK_CHECK(vkGetSwapchainImagesKHR(device_, swapchain_, &scount, swap_images_.data()));
    return true;
}

void VulkanBlitter::DestroySwapchain() {
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    swap_images_.clear();
}

bool VulkanBlitter::CreateStagingBuffer() {
    const VkDeviceSize need =
        static_cast<VkDeviceSize>(extent_.width)
      * static_cast<VkDeviceSize>(extent_.height) * 4u;
    if (need == 0) return true;          // window iconified, defer
    if (need <= staging_size_) return true;

    DestroyStagingBuffer();

    // Allocate ~1.5x to absorb small grow-shrink cycles.
    VkDeviceSize alloc = need + need / 2;

    VkBufferCreateInfo bi{};
    bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size        = alloc;
    bi.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device_, &bi, nullptr, &staging_buf_));

    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device_, staging_buf_, &mr);

    VkMemoryAllocateInfo mi{};
    mi.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mi.allocationSize  = mr.size;
    mi.memoryTypeIndex = FindMemoryType(
        mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mi.memoryTypeIndex == UINT32_MAX) {
        std::fprintf(stderr, "weweb: no host-visible/coherent memory type\n");
        return false;
    }
    VK_CHECK(vkAllocateMemory(device_, &mi, nullptr, &staging_mem_));
    VK_CHECK(vkBindBufferMemory(device_, staging_buf_, staging_mem_, 0));
    VK_CHECK(vkMapMemory(device_, staging_mem_, 0, mr.size, 0, &staging_mapped_));
    staging_size_ = alloc;
    return true;
}

void VulkanBlitter::DestroyStagingBuffer() {
    if (staging_mapped_) {
        vkUnmapMemory(device_, staging_mem_);
        staging_mapped_ = nullptr;
    }
    if (staging_buf_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, staging_buf_, nullptr);
        staging_buf_ = VK_NULL_HANDLE;
    }
    if (staging_mem_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, staging_mem_, nullptr);
        staging_mem_ = VK_NULL_HANDLE;
    }
    staging_size_ = 0;
}

bool VulkanBlitter::CreateSyncObjects() {
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo     fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &img_avail_sem_[i]));
        VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &render_done_sem_[i]));
        VK_CHECK(vkCreateFence    (device_, &fi, nullptr, &in_flight_fence_[i]));
    }
    return true;
}

bool VulkanBlitter::Resize() {
    if (!device_) return false;
    vkDeviceWaitIdle(device_);
    DestroySwapchain();
    if (!CreateSwapchain())     return false;
    if (extent_.width == 0 || extent_.height == 0) return true;
    if (!CreateStagingBuffer()) return false;
    return true;
}

std::uint32_t VulkanBlitter::FindMemoryType(std::uint32_t type_bits,
                                            VkMemoryPropertyFlags props) const {
    for (std::uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (mem_props_.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool VulkanBlitter::RenderFrame(const std::uint8_t* pixels) {
    if (!swapchain_ || extent_.width == 0 || extent_.height == 0) {
        return false;  // caller will Resize
    }

    const std::uint32_t fi = frame_index_;
    VkFence         fence    = in_flight_fence_[fi];
    VkSemaphore     img_sem  = img_avail_sem_[fi];
    VkSemaphore     done_sem = render_done_sem_[fi];
    VkCommandBuffer cmd      = cmd_bufs_[fi];

    vkWaitForFences(device_, 1, &fence, VK_TRUE, kFenceTimeoutNs);

    std::uint32_t img_idx = 0;
    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_,
                                         kFenceTimeoutNs, img_sem,
                                         VK_NULL_HANDLE, &img_idx);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) return false;  // caller resizes
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr,
                     "weweb: vkAcquireNextImageKHR=%d\n", static_cast<int>(acq));
        return true;
    }

    vkResetFences(device_, 1, &fence);

    if (pixels) {
        const std::size_t bytes =
            static_cast<std::size_t>(extent_.width)
          * static_cast<std::size_t>(extent_.height) * 4u;
        std::memcpy(staging_mapped_, pixels, bytes);
    }

    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    VkImage swap_img = swap_images_[img_idx];

    // UNDEFINED → TRANSFER_DST_OPTIMAL.
    VkImageMemoryBarrier to_dst{};
    to_dst.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image                       = swap_img;
    to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_dst.subresourceRange.levelCount = 1;
    to_dst.subresourceRange.layerCount = 1;
    to_dst.srcAccessMask               = 0;
    to_dst.dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_dst);

    if (pixels) {
        VkBufferImageCopy cp{};
        cp.bufferOffset      = 0;
        cp.bufferRowLength   = 0;        // tightly packed
        cp.bufferImageHeight = 0;
        cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cp.imageSubresource.layerCount = 1;
        cp.imageOffset = {0, 0, 0};
        cp.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyBufferToImage(cmd, staging_buf_, swap_img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &cp);
    } else {
        VkClearColorValue clear{};   // {0,0,0,0} = transparent black
        VkImageSubresourceRange r{};
        r.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.levelCount = 1;
        r.layerCount = 1;
        vkCmdClearColorImage(cmd, swap_img,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear, 1, &r);
    }

    // TRANSFER_DST_OPTIMAL → PRESENT_SRC_KHR.
    VkImageMemoryBarrier to_present = to_dst;
    to_present.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_present);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{};
    submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount   = 1;
    submit.pWaitSemaphores      = &img_sem;
    submit.pWaitDstStageMask    = &wait_stage;
    submit.commandBufferCount   = 1;
    submit.pCommandBuffers      = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores    = &done_sem;
    VK_CHECK(vkQueueSubmit(queue_, 1, &submit, fence));

    VkPresentInfoKHR present{};
    present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores    = &done_sem;
    present.swapchainCount     = 1;
    present.pSwapchains        = &swapchain_;
    present.pImageIndices      = &img_idx;

    VkResult pres = vkQueuePresentKHR(queue_, &present);
    frame_index_ = (frame_index_ + 1) % kMaxFramesInFlight;

    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
        return false;
    }
    return true;
}

}  // namespace weweb
