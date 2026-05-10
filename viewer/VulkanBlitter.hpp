#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "DmaBufFrame.hpp"

struct GLFWwindow;

namespace weweb {

// Vulkan presenter for CEF accelerated-paint frames.
//
// AcceptDmaBuf imports a CEF DMA-BUF as a temporary linear VkImage,
// vkCmdCopyImage's it into a persistent device-local "owned" VkImage,
// waits on a fence (CEF reclaims the buffer the moment the callback
// returns), and tears down the temporaries.
//
// RenderFrame vkCmdBlitImage's owned → swapchain image and presents.
//
// No render pass / pipeline / shaders.
class VulkanBlitter {
public:
    VulkanBlitter();
    ~VulkanBlitter();

    VulkanBlitter(const VulkanBlitter&) = delete;
    VulkanBlitter& operator=(const VulkanBlitter&) = delete;

    // Bring up instance + surface + device + swapchain. Returns false on
    // failure (logged to stderr). After a successful Init, the window
    // hint must have been GLFW_NO_API.
    bool Init(GLFWwindow* window);

    // Tear everything down. Idempotent.
    void Shutdown();

    // Current swapchain extent (= the size CEF should render at).
    std::uint32_t Width()  const { return extent_.width; }
    std::uint32_t Height() const { return extent_.height; }

    // Recreate the swapchain after a window resize. The caller must wait
    // until the new extent is non-zero (window is iconified ⇒ skip).
    bool Resize();

    // Import a CEF DMA-BUF frame, copy it onto our owned image. Must be
    // called synchronously from inside the OnAcceleratedPaint callback —
    // we do a CPU-side fence wait so the GPU is finished reading the
    // imported buffer before this returns (CEF then reclaims the FD).
    // Returns false on any Vulkan error; the frame is dropped.
    bool AcceptDmaBuf(const DmaBufFrame& frame);

    // Render one frame. Blits the latest owned image to the acquired
    // swapchain image. If no owned image yet (no CEF paint received)
    // presents a black frame. Returns false if the swapchain went
    // out-of-date — caller should Resize() before the next frame.
    bool RenderFrame();

private:
    bool CreateInstance();
    bool CreateSurface(GLFWwindow* window);
    bool PickPhysicalDevice();
    bool CreateDevice();
    bool CreateSwapchain();
    bool CreateCommandPool();
    bool CreateSyncObjects();

    bool EnsureOwnedImage(int width, int height);
    void DestroyOwnedImage();

    void DestroySwapchain();

    std::uint32_t FindMemoryType(std::uint32_t type_bits,
                                 VkMemoryPropertyFlags props) const;

    // Instance / device.
    VkInstance       instance_   {VK_NULL_HANDLE};
    VkSurfaceKHR     surface_    {VK_NULL_HANDLE};
    VkPhysicalDevice phys_       {VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties mem_props_{};
    VkDevice         device_     {VK_NULL_HANDLE};
    std::uint32_t    queue_family_{0};
    VkQueue          queue_      {VK_NULL_HANDLE};

    // Swapchain.
    VkSwapchainKHR     swapchain_      {VK_NULL_HANDLE};
    VkFormat           swap_format_    {VK_FORMAT_UNDEFINED};
    VkColorSpaceKHR    swap_colorspace_{VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    VkPresentModeKHR   present_mode_   {VK_PRESENT_MODE_FIFO_KHR};
    VkExtent2D         extent_         {0, 0};
    std::vector<VkImage>     swap_images_;
    std::uint32_t            frame_index_{0};
    static constexpr std::uint32_t kMaxFramesInFlight = 2;

    // Owned device-local image — CEF DMA-BUFs get copied into this. The
    // swapchain blit reads from here. Recreated when CEF's coded size
    // changes.
    VkImage         owned_image_    {VK_NULL_HANDLE};
    VkDeviceMemory  owned_image_mem_{VK_NULL_HANDLE};
    VkImageLayout   owned_layout_   {VK_IMAGE_LAYOUT_UNDEFINED};
    int             owned_width_    {0};
    int             owned_height_   {0};
    bool            owned_has_data_ {false};

    // Function pointers loaded with vkGetDeviceProcAddr at device
    // creation time — these come from VK_KHR_external_memory_fd which
    // is enabled as a device extension.
    PFN_vkGetMemoryFdPropertiesKHR pfn_GetMemoryFdProperties_{nullptr};

    // Commands + sync. One per in-flight frame.
    VkCommandPool   cmd_pool_      {VK_NULL_HANDLE};
    VkCommandBuffer cmd_bufs_      [kMaxFramesInFlight] {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSemaphore     img_avail_sem_ [kMaxFramesInFlight] {VK_NULL_HANDLE, VK_NULL_HANDLE};
    // Per-swapchain-image render-done semaphore, indexed by acquired
    // image index. Reusing one binary semaphore across frames violates
    // VUID-vkQueueSubmit-pSignalSemaphores-00067 because vkQueuePresentKHR
    // keeps the wait semaphore busy until the image is re-acquired.
    std::vector<VkSemaphore> render_done_sem_;
    VkFence         in_flight_fence_[kMaxFramesInFlight]{VK_NULL_HANDLE, VK_NULL_HANDLE};

    GLFWwindow* window_{nullptr};
};

}  // namespace weweb
