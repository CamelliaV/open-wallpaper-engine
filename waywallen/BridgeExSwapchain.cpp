#include "BridgeExSwapchain.hpp"

namespace ww_wescene
{

BridgeExSwapchain::BridgeExSwapchain(ww_pool_t* pool, int sock)
    : m_core(pool, sock) {}

BridgeExSwapchain::~BridgeExSwapchain() = default;

bool BridgeExSwapchain::acquireRenderTarget(owe::vulkan::ImageParameters& out) {
    VkImage  image = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    if (!m_core.acquireSlot(&image, &w, &h)) return false;

    out.handle       = image;
    out.view         = VK_NULL_HANDLE; // FinPass blit needs only the handle.
    out.sampler      = VK_NULL_HANDLE;
    out.extent       = { w, h, 1 };
    out.mipmap_level = 1;
    return true;
}

} // namespace ww_wescene
