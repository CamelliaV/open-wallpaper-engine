#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace weweb {

// Minimal Vulkan presenter for OSR pixels. Per frame the caller hands us
// a CPU BGRA8 buffer matching the current swapchain extent; we
// memcpy → mapped host-visible buffer, vkCmdCopyBufferToImage straight
// into the acquired swapchain image, transition to PRESENT_SRC, present.
//
// Format: VK_FORMAT_B8G8R8A8_UNORM (matches CEF's OnPaint byte layout
// exactly, no SRGB conversion). Fallback to A8B8G8R8 if BGRA8_UNORM is
// not in the surface's supported format list.
//
// No render pass / pipeline / shaders. v1 OSR — when we move to
// OnAcceleratedPaint + DMA-BUF import, this whole class gets replaced.
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

    // Render one frame. `pixels` is BGRA8 row-tight, exactly w*h*4 bytes,
    // matching the current swapchain extent. If `pixels` is null we
    // present a black frame (used before CEF delivers its first paint).
    // Returns false if the swapchain went out-of-date and the caller
    // should call Resize() before next frame.
    bool RenderFrame(const std::uint8_t* pixels);

private:
    bool CreateInstance();
    bool CreateSurface(GLFWwindow* window);
    bool PickPhysicalDevice();
    bool CreateDevice();
    bool CreateSwapchain();
    bool CreateStagingBuffer();
    bool CreateCommandPool();
    bool CreateSyncObjects();

    void DestroySwapchain();
    void DestroyStagingBuffer();

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

    // Staging buffer (host-visible, persistently mapped). Resized to
    // match the current extent.
    VkBuffer        staging_buf_   {VK_NULL_HANDLE};
    VkDeviceMemory  staging_mem_   {VK_NULL_HANDLE};
    VkDeviceSize    staging_size_  {0};
    void*           staging_mapped_{nullptr};

    // Commands + sync. One per in-flight frame.
    VkCommandPool   cmd_pool_      {VK_NULL_HANDLE};
    VkCommandBuffer cmd_bufs_      [kMaxFramesInFlight] {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSemaphore     img_avail_sem_ [kMaxFramesInFlight] {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSemaphore     render_done_sem_[kMaxFramesInFlight]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFence         in_flight_fence_[kMaxFramesInFlight]{VK_NULL_HANDLE, VK_NULL_HANDLE};

    GLFWwindow* window_{nullptr};
};

}  // namespace weweb
