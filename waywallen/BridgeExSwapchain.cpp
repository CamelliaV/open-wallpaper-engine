#include "BridgeExSwapchain.hpp"

#include <waywallen-bridge/drm_fourcc.h>

#include <cstdio>
#include <unistd.h>
#include <utility>

namespace ww_wescene
{

namespace
{

// fourcc → VkFormat for the producer-side image view.
//
// DRM fourcc names channels MSB→LSB in a little-endian uint32; VkFormat
// names channels in memory byte order. Hence the byte-for-byte equiv:
//   DRM_FORMAT_ABGR8888  (mem: R G B A) ↔ VK_FORMAT_R8G8B8A8_UNORM
//   DRM_FORMAT_ARGB8888  (mem: B G R A) ↔ VK_FORMAT_B8G8R8A8_UNORM
//
// X variants share the byte layout of their A counterpart — the 4th
// byte is "ignored" rather than meaningful alpha. Vulkan render targets
// cannot use non-identity component swizzle to force A=ONE on the view,
// so X is folded onto the A variant here and FinPass's fragment shader
// writes alpha=1.0 unconditionally. That keeps producer output valid
// for both X (consumer ignores alpha) and A (consumer sees fully opaque)
// without needing to rebuild FinPass on X↔A re-negotiation.
VkFormat fourcc_to_vk_format(uint32_t fourcc) {
    switch (fourcc) {
    case WW_DRM_FORMAT_ABGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case WW_DRM_FORMAT_XBGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case WW_DRM_FORMAT_ARGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case WW_DRM_FORMAT_XRGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
    default:                     return VK_FORMAT_UNDEFINED;
    }
}

} // namespace


BridgeExSwapchain::BridgeExSwapchain(ww_pool_t* pool, int sock,
                                     VkPhysicalDevice physical_device,
                                     VkDevice         device,
                                     VkQueue          graphics_queue,
                                     uint32_t         graphics_queue_family)
    : m_pool(pool),
      m_sock(sock),
      m_physical_device(physical_device),
      m_device(device),
      m_queue(graphics_queue),
      m_queue_family(graphics_queue_family) {
    if (!createCopyResources()) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: copy resource init failed; swapchain "
                     "will report not-ready forever\n");
    }
}

BridgeExSwapchain::~BridgeExSwapchain() {
    if (m_device != VK_NULL_HANDLE) {
        // Drain any in-flight copy before tearing down.
        if (m_copy_fence_inflight && m_copy_fence != VK_NULL_HANDLE) {
            vkWaitForFences(m_device, 1, &m_copy_fence, VK_TRUE, UINT64_MAX);
            m_copy_fence_inflight = false;
        }
    }
    destroyIntermediates();
    destroyCopyResources();
}

uint32_t BridgeExSwapchain::pickMemoryType(uint32_t type_bits,
                                           VkMemoryPropertyFlags want) const {
    VkPhysicalDeviceMemoryProperties mp {};
    vkGetPhysicalDeviceMemoryProperties(m_physical_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(type_bits & (1u << i))) continue;
        if ((mp.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
}

bool BridgeExSwapchain::createCopyResources() {
    if (m_resources_ready) return true;
    if (m_device == VK_NULL_HANDLE) return false;

    // Load KHR extension entry points. Required: VK_KHR_external_semaphore_fd
    // (already pulled in via VulkanRender's device_exts).
    m_pfn_import_sem = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        vkGetDeviceProcAddr(m_device, "vkImportSemaphoreFdKHR"));
    m_pfn_get_sem_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        vkGetDeviceProcAddr(m_device, "vkGetSemaphoreFdKHR"));
    if (!m_pfn_import_sem || !m_pfn_get_sem_fd) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: VK_KHR_external_semaphore_fd entry "
                     "points not available (import=%p get=%p)\n",
                     (void*)m_pfn_import_sem, (void*)m_pfn_get_sem_fd);
        return false;
    }

    {
        VkCommandPoolCreateInfo ci {
            .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext            = nullptr,
            .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = m_queue_family,
        };
        if (vkCreateCommandPool(m_device, &ci, nullptr, &m_cmd_pool) != VK_SUCCESS) {
            std::fprintf(stderr, "BridgeExSwapchain: vkCreateCommandPool failed\n");
            return false;
        }
    }
    {
        VkCommandBufferAllocateInfo ai {
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext              = nullptr,
            .commandPool        = m_cmd_pool,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (vkAllocateCommandBuffers(m_device, &ai, &m_cmd) != VK_SUCCESS) {
            std::fprintf(stderr, "BridgeExSwapchain: vkAllocateCommandBuffers failed\n");
            return false;
        }
    }
    {
        VkFenceCreateInfo ci {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };
        if (vkCreateFence(m_device, &ci, nullptr, &m_copy_fence) != VK_SUCCESS) {
            std::fprintf(stderr, "BridgeExSwapchain: vkCreateFence failed\n");
            return false;
        }
    }
    {
        // Importable SYNC_FD semaphore — payload comes from the
        // producer's sync_fd via vkImportSemaphoreFdKHR.
        VkExportSemaphoreCreateInfo exp {
            .sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .pNext       = nullptr,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR,
        };
        VkSemaphoreCreateInfo ci {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &exp,
            .flags = 0,
        };
        if (vkCreateSemaphore(m_device, &ci, nullptr, &m_wait_sem) != VK_SUCCESS) {
            std::fprintf(stderr, "BridgeExSwapchain: vkCreateSemaphore (wait) failed\n");
            return false;
        }
        if (vkCreateSemaphore(m_device, &ci, nullptr, &m_signal_sem) != VK_SUCCESS) {
            std::fprintf(stderr, "BridgeExSwapchain: vkCreateSemaphore (signal) failed\n");
            return false;
        }
    }

    m_resources_ready = true;
    return true;
}

void BridgeExSwapchain::destroyCopyResources() {
    if (m_device == VK_NULL_HANDLE) return;
    if (m_signal_sem != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device, m_signal_sem, nullptr);
        m_signal_sem = VK_NULL_HANDLE;
    }
    if (m_wait_sem != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device, m_wait_sem, nullptr);
        m_wait_sem = VK_NULL_HANDLE;
    }
    if (m_copy_fence != VK_NULL_HANDLE) {
        vkDestroyFence(m_device, m_copy_fence, nullptr);
        m_copy_fence = VK_NULL_HANDLE;
    }
    if (m_cmd_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_cmd_pool, nullptr); // frees m_cmd
        m_cmd_pool = VK_NULL_HANDLE;
        m_cmd      = VK_NULL_HANDLE;
    }
    m_resources_ready = false;
}

bool BridgeExSwapchain::createIntermediate(uint32_t slot, uint32_t w, uint32_t h,
                                           VkFormat fmt) {
    auto& m = m_intermediates[slot];
    VkImageCreateInfo ici {
        .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext                 = nullptr,
        .flags                 = 0,
        .imageType             = VK_IMAGE_TYPE_2D,
        .format                = fmt,
        .extent                = { w, h, 1 },
        .mipLevels             = 1,
        .arrayLayers           = 1,
        .samples               = VK_SAMPLE_COUNT_1_BIT,
        .tiling                = VK_IMAGE_TILING_OPTIMAL,
        .usage                 = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices   = nullptr,
        .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(m_device, &ici, nullptr, &m.image) != VK_SUCCESS) {
        std::fprintf(stderr, "BridgeExSwapchain: vkCreateImage(intermediate) failed\n");
        return false;
    }

    VkMemoryRequirements mr {};
    vkGetImageMemoryRequirements(m_device, m.image, &mr);
    uint32_t type = pickMemoryType(mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        std::fprintf(stderr, "BridgeExSwapchain: no device-local memory type\n");
        return false;
    }
    VkMemoryAllocateInfo mai {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = nullptr,
        .allocationSize  = mr.size,
        .memoryTypeIndex = type,
    };
    if (vkAllocateMemory(m_device, &mai, nullptr, &m.memory) != VK_SUCCESS) {
        std::fprintf(stderr, "BridgeExSwapchain: vkAllocateMemory(intermediate) failed\n");
        return false;
    }
    if (vkBindImageMemory(m_device, m.image, m.memory, 0) != VK_SUCCESS) {
        std::fprintf(stderr, "BridgeExSwapchain: vkBindImageMemory(intermediate) failed\n");
        return false;
    }

    VkImageViewCreateInfo vci {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext    = nullptr,
        .flags    = 0,
        .image    = m.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = fmt,
        .components = {
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (vkCreateImageView(m_device, &vci, nullptr, &m.view) != VK_SUCCESS) {
        std::fprintf(stderr, "BridgeExSwapchain: vkCreateImageView(intermediate) failed\n");
        return false;
    }
    return true;
}

void BridgeExSwapchain::destroyIntermediates() {
    if (m_device == VK_NULL_HANDLE) return;
    for (auto& m : m_intermediates) {
        if (m.view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, m.view, nullptr);
            m.view = VK_NULL_HANDLE;
        }
        if (m.image != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, m.image, nullptr);
            m.image = VK_NULL_HANDLE;
        }
        if (m.memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m.memory, nullptr);
            m.memory = VK_NULL_HANDLE;
        }
    }
}

void BridgeExSwapchain::queueDirective(const ww_pool_directive_t& directive) {
    {
        std::lock_guard<std::mutex> lk(m_pending_mu);
        m_pending_directive = directive;
    }
    m_pending_valid.store(true, std::memory_order_release);
}

void BridgeExSwapchain::drainPendingDirective() {
    if (!m_pending_valid.load(std::memory_order_acquire)) return;

    ww_pool_directive_t d {};
    {
        std::lock_guard<std::mutex> lk(m_pending_mu);
        d = m_pending_directive;
        m_pending_valid.store(false, std::memory_order_release);
    }

    int rc = applyDirective(d);
    if (rc != 0) return; // applyDirective already logged; m_slot_count = 0

    // Snapshot callbacks under the lock and invoke unlocked so the
    // handler can re-enter the swapchain (read width/height) without
    // blocking — only the render thread ever invokes them anyway.
    std::function<void()>                                            first_cb;
    std::function<void(const wallpaper::ExSwapchainReadyEvent&)>     ready_cb;
    {
        std::lock_guard<std::mutex> lk(m_cb_mu);
        if (!m_first_negotiated_done && m_on_first_negotiated) {
            first_cb                = m_on_first_negotiated;
            m_first_negotiated_done = true;
        }
        ready_cb = m_on_ready_changed;
    }
    if (first_cb) first_cb();
    if (ready_cb) {
        wallpaper::ExSwapchainReadyEvent e {
            .ready  = true,
            .width  = m_width,
            .height = m_height,
            .format = m_export_format,
        };
        ready_cb(e);
    }
}

int BridgeExSwapchain::applyDirective(const ww_pool_directive_t& directive) {
    VkFormat picked = fourcc_to_vk_format(directive.fourcc);
    if (picked == VK_FORMAT_UNDEFINED) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: fourcc 0x%08x has no VkFormat mapping\n",
                     directive.fourcc);
        ww_bridge_send_bind_failed(m_sock,
                                   directive.fourcc, directive.modifier,
                                   /*reason*/ 1,
                                   "fourcc unsupported by producer");
        return -EINVAL;
    }
    if (directive.count == 0 || directive.count > kMaxSlots) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: invalid slot count %u (cap=%u)\n",
                     directive.count, kMaxSlots);
        return 1;
    }

    if (!m_resources_ready) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: copy resources never initialized\n");
        return 1;
    }

    // Drain any in-flight copy before tearing down intermediates.
    if (m_copy_fence_inflight) {
        vkWaitForFences(m_device, 1, &m_copy_fence, VK_TRUE, UINT64_MAX);
        vkResetFences(m_device, 1, &m_copy_fence);
        m_copy_fence_inflight = false;
    }

    // Bridge contract (pool.h apply_directive step 2): the call itself
    // tears down the previous slots, so old `vk_image` handles are
    // invalidated synchronously. Our intermediates are independent of
    // bridge slots — destroy them only after the per-frame fence has
    // drained (above), then rebuild for the new geometry.
    destroyIntermediates();
    m_slot_count   = 0;
    m_next_slot    = 0;
    m_have_pending = false;
    // Don't publish the new format/extent yet — wait for apply_directive
    // to succeed. If we publish early and apply_directive fails, the
    // render thread sees format() = picked with no slots available, and
    // drawFrameOffscreen wastes a rebuildPresent on a format we'll
    // never actually render to.

    int rc = ww_bridge_pool_apply_directive(m_pool, m_sock, &directive);
    if (rc < 0) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: apply_directive dry-run failed: %d\n", rc);
        // m_export_format intentionally untouched — keeps last good state.
        return rc;
    }
    if (rc > 0) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: apply_directive system error: %d\n", rc);
        return rc;
    }

    for (uint32_t i = 0; i < directive.count; ++i) {
        if (!createIntermediate(i, directive.width, directive.height, picked)) {
            destroyIntermediates();
            return 1;
        }
    }

    // Publish atomically only after every intermediate succeeded.
    m_width         = directive.width;
    m_height        = directive.height;
    m_fourcc        = directive.fourcc;
    m_export_format = picked;
    m_slot_count    = directive.count;
    return 0;
}

bool BridgeExSwapchain::acquireRenderTarget(wallpaper::vulkan::ImageParameters& out) {
    // Caller must invoke `poll()` first. Splitting drain out of acquire
    // lets the caller inspect format() and rebuild downstream pipelines
    // *before* committing to a slot — a post-acquire failure would
    // otherwise leak the slot (no `cancelRenderTarget` exists).
    if (m_slot_count == 0) return false;

    uint32_t idx = m_next_slot;
    m_next_slot  = (m_next_slot + 1) % m_slot_count;

    // Producer back-pressure on the *bridge* slot, not the
    // intermediate. Keep the timeout tight: bridge contract says
    // non-zero return means "consumer still using; render anyway"
    // (pool.h wait_slot_release). Producer-runs-ahead is documented.
    (void)ww_bridge_pool_wait_slot_release(m_pool, idx, /*timeout_ms*/ 16);

    auto& mid = m_intermediates[idx];
    out.handle       = mid.image;
    out.view         = mid.view;
    out.sampler      = VK_NULL_HANDLE;
    out.extent       = { m_width, m_height, 1 };
    out.mipmap_level = 1;

    m_pending_slot = idx;
    m_have_pending = true;
    return true;
}

void BridgeExSwapchain::submitRendered(int producer_sync_fd) {
    if (!m_have_pending) {
        if (producer_sync_fd >= 0) ::close(producer_sync_fd);
        return;
    }
    uint32_t slot = m_pending_slot;
    m_have_pending = false;

    if (!m_resources_ready) {
        if (producer_sync_fd >= 0) ::close(producer_sync_fd);
        return;
    }

    // Bring the bridge slot's underlying VkImage handle into our cmd
    // — bridge owns lifetime; we just need the VkImage for cmdCopyImage.
    ww_pool_slot_t s {};
    if (int rc = ww_bridge_pool_acquire_slot(m_pool, slot, &s); rc != 0) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: acquire_slot(%u) failed: %d\n", slot, rc);
        if (producer_sync_fd >= 0) ::close(producer_sync_fd);
        return;
    }
    VkImage slot_image = static_cast<VkImage>(s.vk_image);
    if (slot_image == VK_NULL_HANDLE) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: slot %u has no vk_image\n", slot);
        if (producer_sync_fd >= 0) ::close(producer_sync_fd);
        return;
    }

    // Wait the previous copy (if any) so we can reuse cmd buffer +
    // signal semaphore. Spec: SYNC_FD-typed semaphore can only be
    // signaled if its prior signal already retired — i.e. after fence.
    if (m_copy_fence_inflight) {
        vkWaitForFences(m_device, 1, &m_copy_fence, VK_TRUE, UINT64_MAX);
        vkResetFences(m_device, 1, &m_copy_fence);
        m_copy_fence_inflight = false;
    }

    // Import producer's sync_fd into m_wait_sem. Vulkan takes ownership
    // of the fd on success; we MUST NOT close it after.
    bool wait_imported = false;
    if (producer_sync_fd >= 0) {
        VkImportSemaphoreFdInfoKHR isi {
            .sType      = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .pNext      = nullptr,
            .semaphore  = m_wait_sem,
            .flags      = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR,
            .fd         = producer_sync_fd,
        };
        if (m_pfn_import_sem(m_device, &isi) == VK_SUCCESS) {
            wait_imported   = true;
            producer_sync_fd = -1; // ownership transferred to Vulkan
        } else {
            std::fprintf(stderr,
                         "BridgeExSwapchain: vkImportSemaphoreFdKHR failed\n");
            ::close(producer_sync_fd);
            producer_sync_fd = -1;
            // Fall through without a wait — producer has already fenced
            // its own submit in VulkanRender::drawFrameOffscreen, so
            // this is safe in the current architecture (just lose the
            // GPU-side wait).
        }
    }

    // Record copy + release barrier.
    vkResetCommandBuffer(m_cmd, 0);
    VkCommandBufferBeginInfo bi {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext            = nullptr,
        .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    if (vkBeginCommandBuffer(m_cmd, &bi) != VK_SUCCESS) {
        std::fprintf(stderr, "BridgeExSwapchain: vkBeginCommandBuffer failed\n");
        return;
    }

    VkImageSubresourceRange sub {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };

    // Bridge slot: UNDEFINED → TRANSFER_DST_OPTIMAL. The slot may have
    // been previously released to FOREIGN; UNDEFINED layout transition
    // forgets that ownership. No queue-family transfer needed (UNDEFINED
    // is the documented free pass).
    {
        VkImageMemoryBarrier b {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = 0,
            .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = slot_image,
            .subresourceRange    = sub,
        };
        vkCmdPipelineBarrier(m_cmd,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // FinPass left the intermediate in TRANSFER_SRC_OPTIMAL (see
    // ExSwapchain::producerOutputLayout). vkCmdCopyImage requires
    // exactly that layout for the source — no extra transition.
    VkImageCopy region {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffset      = { 0, 0, 0 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffset      = { 0, 0, 0 },
        .extent         = { m_width, m_height, 1 },
    };
    vkCmdCopyImage(m_cmd,
                   m_intermediates[slot].image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   slot_image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region);

    // Release slot to foreign consumer. oldLayout=TRANSFER_DST_OPTIMAL
    // (just-written), newLayout=GENERAL, src=graphics, dst=FOREIGN_EXT.
    // Forces driver cache flush so the consumer (KMS / display server
    // through DMA-BUF) reads coherent pixels.
    {
        VkImageMemoryBarrier b {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask       = 0,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = m_queue_family,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
            .image               = slot_image,
            .subresourceRange    = sub,
        };
        vkCmdPipelineBarrier(m_cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    }

    if (vkEndCommandBuffer(m_cmd) != VK_SUCCESS) {
        std::fprintf(stderr, "BridgeExSwapchain: vkEndCommandBuffer failed\n");
        return;
    }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext                = nullptr,
        .waitSemaphoreCount   = wait_imported ? 1u : 0u,
        .pWaitSemaphores      = wait_imported ? &m_wait_sem : nullptr,
        .pWaitDstStageMask    = wait_imported ? &wait_stage : nullptr,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &m_cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = &m_signal_sem,
    };
    if (vkQueueSubmit(m_queue, 1, &si, m_copy_fence) != VK_SUCCESS) {
        std::fprintf(stderr, "BridgeExSwapchain: vkQueueSubmit (copy) failed\n");
        return;
    }
    m_copy_fence_inflight = true;

    // Export the signal semaphore as SYNC_FD. Spec allows export of a
    // pending semaphore — the resulting fd signals when the GPU finally
    // signals the semaphore. Bridge / consumer wait this fd before
    // scanout / read.
    int new_sync_fd = -1;
    {
        VkSemaphoreGetFdInfoKHR gi {
            .sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .pNext      = nullptr,
            .semaphore  = m_signal_sem,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR,
        };
        VkResult vr = m_pfn_get_sem_fd(m_device, &gi, &new_sync_fd);
        if (vr != VK_SUCCESS || new_sync_fd < 0) {
            std::fprintf(stderr,
                         "BridgeExSwapchain: vkGetSemaphoreFdKHR(signal) "
                         "vr=%d fd=%d\n", (int)vr, new_sync_fd);
            new_sync_fd = -1;
        }
    }

    int rc = ww_bridge_pool_submit_slot(m_pool, m_sock, slot, new_sync_fd);
    if (rc != 0) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: submit_slot(%u) rc=%d\n", slot, rc);
        // Bridge contract: bridge always closes the fd. We don't dup-close.
    }
}

} // namespace ww_wescene
