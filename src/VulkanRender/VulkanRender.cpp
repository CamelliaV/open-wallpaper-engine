module;

#include "Utils/Logging.h"
#include "Swapchain/ExSwapchain.hpp"

#include <glslang/Public/ShaderLang.h>

#include "Core/ArrayHelper.hpp"
#include "Core/Literals.hpp"
#include "Core/MapSet.hpp"
#include "Type.hpp"
#include "vvk/macros.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <unistd.h>
#include <utility>
#include <vector>

#if ENABLE_RENDERDOC_API
#    include "RenderDoc.h"
#endif

module wescene.vulkan_render;
import wescene.vulkan;
import wescene.utils;
import wescene.scene;

import wescene.rgraph;

using namespace wallpaper::vulkan;

constexpr uint64_t vk_wait_time { 10u * 1000u * 1000000u };
constexpr uint32_t vk_command_num { 2 };

constexpr std::array base_inst_exts {
    Extension { false, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME },
};
constexpr std::array base_device_exts {
    Extension { false, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME },
    Extension { true, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME },
    Extension { true, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME },
    Extension { true, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME },
    Extension { true, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME },
    Extension { true, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME },
    // Optional. When present we can report the picked physical device's
    // DRM render-node major/minor via `getDrmRenderNode()` so the
    // waywallen daemon can match it against each connected display's
    // GPU. When absent the accessor returns false and callers report
    // (0, 0); the daemon then conservatively assumes cross-GPU.
    Extension { false, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME },
};

struct VulkanRender::Impl {
    Impl()  = default;
    ~Impl() = default;

    bool init(RenderInitInfo);
    void destroy();

    void drawFrame(Scene&);

    bool CreateRenderingResource(RenderingResources&);
    void DestroyRenderingResource(RenderingResources&);

    void clearLastRenderGraph();
    void compileRenderGraph(Scene&, rg::RenderGraph&);
    void UpdateCameraFillMode(Scene&, wallpaper::FillMode);

    bool initRes();
    void drawFrameSwapchain();
    void drawFrameOffscreen();
    void setRenderTargetSize(Scene&, rg::RenderGraph&);
    bool onSwapchainReady(unsigned width, unsigned height);

    Instance                m_instance;
    std::unique_ptr<Device> m_device;

    std::unique_ptr<PrePass> m_prepass { nullptr };
    std::unique_ptr<FinPass> m_finpass { nullptr };

    std::unique_ptr<FinPass> m_testpass { nullptr };
    ReDrawCB                 m_redraw_cb;

    std::unique_ptr<StagingBuffer> m_vertex_buf { nullptr };
    std::unique_ptr<StagingBuffer> m_dyn_buf { nullptr };

    vvk::CommandBuffers m_cmds;
    vvk::CommandBuffer  m_upload_cmd;
    vvk::CommandBuffer  m_render_cmd;

    bool m_with_surface { false };
    bool m_inited { false };
    bool m_pass_loaded { false };

    std::unique_ptr<ExSwapchain> m_ex_swapchain;
    RenderingResources           m_rendering_resources;

    std::vector<VulkanPass*> m_passes;
};

VulkanRender::VulkanRender(): pImpl(std::make_unique<Impl>()) {}
VulkanRender::~VulkanRender() {};

bool VulkanRender::inited() const { return pImpl->m_inited; }

int VulkanRender::takeLastFrameSyncFd() {
    return pImpl->m_ex_swapchain ? pImpl->m_ex_swapchain->takeLastFrameSyncFd() : -1;
}

bool VulkanRender::getDrmRenderNode(uint32_t& out_major,
                                    uint32_t& out_minor) const {
    if (!pImpl->m_inited || !pImpl->m_device) return false;
    VkPhysicalDeviceDrmPropertiesEXT drm {};
    drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2KHR props {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
    props.pNext = &drm;
    pImpl->m_device->gpu().GetProperties2KHR(props);
    if (!drm.hasRender) return false;
    if (drm.renderMajor < 0 || drm.renderMinor < 0
        || (uint64_t)drm.renderMajor > UINT32_MAX
        || (uint64_t)drm.renderMinor > UINT32_MAX) {
        return false;
    }
    out_major = static_cast<uint32_t>(drm.renderMajor);
    out_minor = static_cast<uint32_t>(drm.renderMinor);
    return true;
}

VkInstance VulkanRender::vkInstance() const {
    if (!pImpl->m_inited) return VK_NULL_HANDLE;
    return *pImpl->m_instance.inst();
}

VkPhysicalDevice VulkanRender::vkPhysicalDevice() const {
    if (!pImpl->m_inited || !pImpl->m_device) return VK_NULL_HANDLE;
    return *pImpl->m_device->gpu();
}

VkDevice VulkanRender::vkDevice() const {
    if (!pImpl->m_inited || !pImpl->m_device) return VK_NULL_HANDLE;
    return *pImpl->m_device->handle();
}

VkQueue VulkanRender::vkGraphicsQueue() const {
    if (!pImpl->m_inited || !pImpl->m_device) return VK_NULL_HANDLE;
    return *pImpl->m_device->graphics_queue().handle;
}

uint32_t VulkanRender::vkGraphicsQueueFamily() const {
    if (!pImpl->m_inited || !pImpl->m_device) return 0;
    return pImpl->m_device->graphics_queue().family_index;
}

void VulkanRender::deviceUuid(uint8_t out[16]) const {
    std::memset(out, 0, 16);
    if (!pImpl->m_inited || !pImpl->m_device) return;
    VkPhysicalDeviceIDPropertiesKHR id {};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2KHR props {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
    props.pNext = &id;
    pImpl->m_device->gpu().GetProperties2KHR(props);
    std::memcpy(out, id.deviceUUID, 16);
}

void VulkanRender::driverUuid(uint8_t out[16]) const {
    std::memset(out, 0, 16);
    if (!pImpl->m_inited || !pImpl->m_device) return;
    VkPhysicalDeviceIDPropertiesKHR id {};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2KHR props {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
    props.pNext = &id;
    pImpl->m_device->gpu().GetProperties2KHR(props);
    std::memcpy(out, id.driverUUID, 16);
}

bool VulkanRender::init(RenderInitInfo info) { return pImpl->init(std::move(info)); }
void VulkanRender::destroy() { pImpl->destroy(); }
void VulkanRender::drawFrame(Scene& scene) { pImpl->drawFrame(scene); };
void VulkanRender::clearLastRenderGraph() { pImpl->clearLastRenderGraph(); };
void VulkanRender::compileRenderGraph(Scene& scene, rg::RenderGraph& rg) {
    pImpl->compileRenderGraph(scene, rg);
};
void VulkanRender::UpdateCameraFillMode(Scene& scene, wallpaper::FillMode fill) {
    pImpl->UpdateCameraFillMode(scene, fill);
};

bool VulkanRender::onSwapchainReady(unsigned width, unsigned height) {
    return pImpl->onSwapchainReady(width, height);
}

wallpaper::ExSwapchain* VulkanRender::exSwapchain() const { return pImpl->m_ex_swapchain.get(); };

bool VulkanRender::Impl::init(RenderInitInfo info) {
    if (m_inited) return true;

    m_redraw_cb = info.redraw_callback;
    VkExtent2D extent { info.width, info.height };
    if (extent.width * extent.height < 500 * 500) {
        LOG_ERROR("too small swapchain image size: %dx%d", extent.width, extent.height);
    } else {
        LOG_INFO("set swapchain image size: %dx%d", extent.width, extent.height);
    }

    std::vector<Extension> inst_exts { base_inst_exts.begin(), base_inst_exts.end() };
    std::vector<Extension> device_exts { base_device_exts.begin(), base_device_exts.end() };

    if (! info.offscreen) {
        std::transform(info.surface_info.instanceExts.begin(),
                       info.surface_info.instanceExts.end(),
                       std::back_inserter(inst_exts),
                       [](const auto& s) {
                           return Extension { true, s.c_str() };
                       });
        device_exts.push_back({ true, VK_KHR_SWAPCHAIN_EXTENSION_NAME });
    } else {
        // Iteration 1a: offscreen FDs are real Linux DMA-BUFs so they can be
        // imported by arbitrary external consumers. These extensions are
        // strictly required on the offscreen path; if a driver lacks them
        // we fail fast in Device::CheckGPU.
        device_exts.push_back({ true, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME });
        // Required by VK_EXT_image_drm_format_modifier on Vulkan 1.1 (promoted
        // to core in 1.2). Validation layer rejects the device otherwise.
        device_exts.push_back({ true, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME });
        device_exts.push_back({ true, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME });
        device_exts.push_back({ true, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME });
    }

    std::vector<InstanceLayer> inst_layers;
    // valid layer
    if (info.enable_valid_layer) {
        inst_layers.push_back({ true, VALIDATION_LAYER_NAME });
        LOG_INFO("vulkan valid layer \"%s\" enabled", VALIDATION_LAYER_NAME.data());
    }

    if (! Instance::Create(m_instance, inst_exts, inst_layers)) {
        LOG_ERROR("init vulkan failed");
        return false;
    }
    if (! info.offscreen) {
        VkSurfaceKHR surface;
        VVK_CHECK_ACT(
            {
                LOG_ERROR("create vulkan surface failed");
                return false;
            },
            info.surface_info.createSurfaceOp(*m_instance.inst(), &surface));
        m_instance.setSurface(VkSurfaceKHR(surface));
        m_with_surface = true;
    }
    {
        auto surface   = *m_instance.surface();
        auto check_gpu = [&device_exts, surface](const vvk::PhysicalDevice& gpu) {
            return Device::CheckGPU(gpu, device_exts, surface);
        };
        if (! m_instance.ChoosePhysicalDevice(check_gpu, info.uuid)) return false;
    }

    {
        m_device = std::make_unique<Device>();
        if (! Device::Create(m_instance, device_exts, extent, *m_device)) {
            LOG_ERROR("init vulkan device failed");
            return false;
        }
    }

    if (info.offscreen) {
        if (info.ex_swapchain_factory) {
            RenderInitInfo::ExSwapchainHandles h {
                *m_instance.inst(),
                *m_device->gpu(),
                *m_device->handle(),
                *m_device->graphics_queue().handle,
                m_device->graphics_queue().family_index,
            };
            m_ex_swapchain = info.ex_swapchain_factory(h);
            if (!m_ex_swapchain) {
                LOG_ERROR("ex_swapchain_factory returned null");
                return false;
            }
        } else {
            m_ex_swapchain = CreateLocalExSwapchain(*m_device,
                                                    extent.width,
                                                    extent.height,
                                                    (info.offscreen_tiling == TexTiling::OPTIMAL
                                                         ? VK_IMAGE_TILING_OPTIMAL
                                                         : VK_IMAGE_TILING_LINEAR));
        }
        m_with_surface = false;
    }

    if (! initRes()) return false;
    ;

    m_inited = true;
    return m_inited;
}

bool VulkanRender::Impl::initRes() {
    m_prepass = std::make_unique<PrePass>(PrePass::Desc {});
    m_finpass = std::make_unique<FinPass>(FinPass::Desc {});
    if (m_with_surface) {
        // Surface mode: FinPass blits into the swapchain image, ending
        // it in PRESENT_SRC_KHR. Queue-family transfer to the present
        // queue is needed only when graphics != present family.
        m_finpass->setPresentLayout(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        m_finpass->setPresentQueueIndex(m_device->present_queue().family_index);
    } else {
        // Offscreen: ExSwapchain implementation chooses both. Bridge
        // returns (GENERAL, FOREIGN_EXT); local returns (GENERAL,
        // IGNORED). Translate IGNORED to graphics_family so FinPass's
        // release-barrier branch (`!= graphics_family`) skips cleanly.
        m_finpass->setPresentLayout(m_ex_swapchain->producerOutputLayout());
        uint32_t qf = m_ex_swapchain->releaseTargetQueueFamily();
        if (qf == VK_QUEUE_FAMILY_IGNORED) {
            qf = m_device->graphics_queue().family_index;
        }
        m_finpass->setPresentQueueIndex(qf);
    }

    m_vertex_buf = std::make_unique<StagingBuffer>(*m_device,
                                                   2 * 1024 * 1024,
                                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    m_dyn_buf    = std::make_unique<StagingBuffer>(*m_device,
                                                2 * 1024 * 1024,
                                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (! m_vertex_buf->allocate()) return false;
    if (! m_dyn_buf->allocate()) return false;
    {
        auto& pool = m_device->cmd_pool();
        VVK_CHECK_BOOL_RE(pool.Allocate(vk_command_num, VK_COMMAND_BUFFER_LEVEL_PRIMARY, m_cmds));
        m_upload_cmd = vvk::CommandBuffer(m_cmds[0], m_device->handle().Dispatch());
        m_render_cmd = vvk::CommandBuffer(m_cmds[1], m_device->handle().Dispatch());
    }
    if (! CreateRenderingResource(m_rendering_resources)) return false;

#if ENABLE_RENDERDOC_API
    load_renderdoc_api();
#endif
    return true;
}

void VulkanRender::Impl::destroy() {
    if (! m_inited) return;
    if (m_device && m_device->handle()) {
        VVK_CHECK(m_device->handle().WaitIdle());

        // res
        for (auto& p : m_passes) {
            p->destory(*m_device, m_rendering_resources);
        }
        m_vertex_buf->destroy();
        m_dyn_buf->destroy();

        m_device->Destroy();
    }
    m_instance.Destroy();
}

bool VulkanRender::Impl::CreateRenderingResource(RenderingResources& rr) {
    rr.command = m_render_cmd;
    VVK_CHECK_BOOL_RE(m_device->handle().CreateFence(
        VkFenceCreateInfo {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        },
        rr.fence_frame));

    rr.fence_frame.Reset();

    if (m_with_surface) {
        VkSemaphoreCreateInfo ci { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                   .pNext = nullptr };
        VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, rr.sem_swap_finish));
        VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, rr.sem_swap_wait_image));
    }

    // Exportable SYNC_FD semaphore used by the waywallen-renderer host
    // to ship a dma_fence sync_file to display clients on each
    // FrameReady event. Created in both offscreen and surface modes —
    // only the offscreen drawFrame path currently signals it, but
    // having it always present keeps the lifetime simple.
    {
        VkExportSemaphoreCreateInfo export_info {
            .sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .pNext       = nullptr,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR,
        };
        VkSemaphoreCreateInfo ci {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &export_info,
            .flags = 0,
        };
        VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, rr.sem_export));
    }

    rr.vertex_buf = m_vertex_buf.get();
    rr.dyn_buf    = m_dyn_buf.get();
    return true;
}

void VulkanRender::Impl::DestroyRenderingResource(RenderingResources& rr) {}


void VulkanRender::Impl::drawFrame(Scene& scene) {
    if (! (m_inited && m_pass_loaded)) return;

        // LOG_INFO("used ram: %fm", (m_device->GetUsage()/1024.0f)/1024.0f);

#if ENABLE_RENDERDOC_API
    if (rdoc_api)
        rdoc_api->StartFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE((VkInstance)m_instance.inst()), NULL);
#endif

    if (m_instance.offscreen()) {
        drawFrameOffscreen();
    } else {
        drawFrameSwapchain();
    }

    if (m_redraw_cb) m_redraw_cb();

#if ENABLE_RENDERDOC_API
    if (rdoc_api)
        rdoc_api->EndFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE((VkInstance)m_instance.inst()), NULL);
#endif
}

void VulkanRender::Impl::drawFrameSwapchain() {
    static size_t resource_index = 0;

    RenderingResources& rr = m_rendering_resources;
    resource_index         = (resource_index + 1) % 3;
    uint32_t image_index   = 0;
    {
        VVK_CHECK_VOID_RE(m_device->handle().AcquireNextImageKHR(*m_device->swapchain().handle(),
                                                                 vk_wait_time,
                                                                 *rr.sem_swap_wait_image,
                                                                 {},
                                                                 &image_index));
    }
    const auto& image = m_device->swapchain().images()[image_index];

    m_finpass->setPresent(image);

    (void)rr.command.Begin(VkCommandBufferBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    });
    m_dyn_buf->recordUpload(rr.command);
    for (auto* p : m_passes) {
        if (p->prepared()) {
            p->execute(*m_device, rr);
        }
    }
    (void)rr.command.End();

    VkPipelineStageFlags wait_dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo         sub_info {
                .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext                = nullptr,
                .waitSemaphoreCount   = 1,
                .pWaitSemaphores      = rr.sem_swap_wait_image.address(),
                .pWaitDstStageMask    = &wait_dst_stage,
                .commandBufferCount   = 1,
                .pCommandBuffers      = rr.command.address(),
                .signalSemaphoreCount = 1,
                .pSignalSemaphores    = rr.sem_swap_finish.address(),
    };

    VVK_CHECK_VOID_RE(m_device->present_queue().handle.Submit(sub_info, *rr.fence_frame));
    VkPresentInfoKHR present_info {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext              = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = rr.sem_swap_finish.address(),
        .swapchainCount     = 1,
        .pSwapchains        = m_device->swapchain().handle().address(),
        .pImageIndices      = &image_index,
    };
    VVK_CHECK_VOID_RE(m_device->present_queue().handle.Present(present_info));

    VVK_CHECK_VOID_RE(rr.fence_frame.Wait(vk_wait_time));
    VVK_CHECK_VOID_RE(rr.fence_frame.Reset());
}
void VulkanRender::Impl::drawFrameOffscreen() {
    if (!m_ex_swapchain) return;

    // Drain any pending bridge directive *before* committing to a slot.
    // Previous frame's GPU work has fenced at the tail of the last
    // drawFrameOffscreen, so the cmd pool is idle.
    m_ex_swapchain->poll();

    // Skip until both the swapchain has slots and the scene has loaded
    // (FinPass.prepare runs from compileRenderGraph). FinPass itself is
    // format-agnostic now — vkCmdBlitImage handles cross-format channel
    // mapping, no rebuild needed on renegotiation.
    if (!m_ex_swapchain->ready() || !m_finpass->prepared()) {
        return;
    }

    RenderingResources& rr = m_rendering_resources;
    ImageParameters     image;
    if (!m_ex_swapchain->acquireRenderTarget(image)) {
        return;
    }

    m_finpass->setPresent(image);

    (void)rr.command.Begin(VkCommandBufferBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    });
    m_dyn_buf->recordUpload(rr.command);

    for (auto* p : m_passes) {
        if (p->prepared()) {
            p->execute(*m_device, rr);
        }
    }

    (void)rr.command.End();

    VkSubmitInfo sub_info {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext                = nullptr,
        .commandBufferCount   = 1,
        .pCommandBuffers      = rr.command.address(),
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = rr.sem_export.address(),
    };
    VVK_CHECK_VOID_RE(m_device->graphics_queue().handle.Submit(sub_info, *rr.fence_frame));

    VVK_CHECK_VOID_RE(rr.fence_frame.Wait(vk_wait_time));
    VVK_CHECK_VOID_RE(rr.fence_frame.Reset());

    // Export the signaled semaphore as a dma_fence sync_file fd and hand
    // it to the swapchain along with the slot. LocalExSwapchain stashes
    // it for the host's takeLastFrameSyncFd; BridgeExSwapchain forwards
    // it to ww_bridge_pool_submit_slot.
    //
    // Diagnostics: sync_fd export failure is silent in production but
    // the result is the consumer reading a buffer the producer hasn't
    // finished writing — exactly the "blank frame" symptom. We log the
    // first failure loudly and rate-limit subsequent ones.
    int sync_fd = -1;
    {
        VkSemaphoreGetFdInfoKHR gi {
            .sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .pNext      = nullptr,
            .semaphore  = *rr.sem_export,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR,
        };
        VkResult vr = m_device->handle().GetSemaphoreFdKHR(gi, &sync_fd);
        if (vr != VK_SUCCESS) {
            static std::atomic<uint64_t> n_failed { 0 };
            uint64_t k = n_failed.fetch_add(1, std::memory_order_relaxed);
            if (k == 0 || (k & (k - 1)) == 0) { // 1st, 2nd, 4th, 8th...
                LOG_ERROR("VulkanRender: vkGetSemaphoreFdKHR failed (vr=%d, count=%llu)",
                          (int)vr, (unsigned long long)(k + 1));
            }
            sync_fd = -1;
        } else if (sync_fd < 0) {
            // The driver returned VK_SUCCESS with fd=-1 — spec says this
            // means "the semaphore was unsignaled". Should not happen
            // because we just waited the fence; flag loudly.
            static std::atomic<uint64_t> n_unsig { 0 };
            uint64_t k = n_unsig.fetch_add(1, std::memory_order_relaxed);
            if (k == 0 || (k & (k - 1)) == 0) {
                LOG_ERROR("VulkanRender: GetSemaphoreFdKHR returned fd=-1 "
                          "(semaphore not signaled? count=%llu)",
                          (unsigned long long)(k + 1));
            }
        }
    }

    m_ex_swapchain->submitRendered(sync_fd);
}

bool VulkanRender::Impl::onSwapchainReady(unsigned width, unsigned height) {
    if (!m_inited || !m_device) return false;
    auto& cur = m_device->out_extent();
    bool extent_changed = (width != cur.width) || (height != cur.height);
    if (!extent_changed) {
        // Format-only changes flow through ExSwapchain::format() and
        // are handled by drawFrameOffscreen's head check.
        return false;
    }
    // Drain GPU work for the previous extent's resources before tearing
    // down the texture cache + render passes inside compileRenderGraph
    // (which the caller will run next).
    VVK_CHECK(m_device->handle().WaitIdle());
    m_device->set_out_extent(VkExtent2D { width, height });
    return true;
}

void VulkanRender::Impl::setRenderTargetSize(Scene& scene, rg::RenderGraph& rg) {
    auto& ext = m_device->out_extent();
    for (auto& item : scene.renderTargets) {
        auto& rt = item.second;
        if (rt.bind.enable && rt.bind.screen) {
            rt.width  = (i32)(rt.bind.scale * ext.width);
            rt.height = (i32)(rt.bind.scale * ext.height);
        }
    }
    for (auto& item : scene.renderTargets) {
        auto& rt = item.second;
        if (rt.bind.screen || ! rt.bind.enable) continue;
        auto bind_rt = scene.renderTargets.find(rt.bind.name);
        if (rt.bind.name.empty() || bind_rt == scene.renderTargets.end()) {
            LOG_ERROR("unknonw render target bind: %s", rt.bind.name.c_str());
            continue;
        }
        rt.width  = (i32)(rt.bind.scale * bind_rt->second.width);
        rt.height = (i32)(rt.bind.scale * bind_rt->second.height);
    }
    for (auto& item : scene.renderTargets) {
        auto& rt = item.second;
        if (! item.first.empty() && (rt.width * rt.height <= 4)) {
            LOG_ERROR("wrong size for render target: %s", item.first.c_str());
        } else if (rt.has_mipmap) {
            rt.mipmap_level =
                std::max(3u,
                         static_cast<uint>(std::floor(std::log2(std::min(rt.width, rt.height))))) -
                2u;
        }
    }
    scene.shaderValueUpdater->SetScreenSize((i32)ext.width, (i32)ext.height);
}

void VulkanRender::Impl::UpdateCameraFillMode(wallpaper::Scene&   scene,
                                              wallpaper::FillMode fillmode) {
    using namespace wallpaper;
    auto width  = m_device->out_extent().width;
    auto height = m_device->out_extent().height;

    if (width == 0) return;
    double sw = scene.ortho[0], sh = scene.ortho[1];
    double fboAspect = width / (double)height, sAspect = sw / sh;
    auto&  gCam    = *scene.cameras.at("global");
    auto&  gPerCam = *scene.cameras.at("global_perspective");
    // assum cam
    switch (fillmode) {
    case FillMode::STRETCH:
        gCam.SetWidth(sw);
        gCam.SetHeight(sh);
        gPerCam.SetAspect(sAspect);
        gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    case FillMode::ASPECTFIT:
        if (fboAspect < sAspect) {
            // scale height
            gCam.SetWidth(sw);
            gCam.SetHeight(sw / fboAspect);
        } else {
            gCam.SetWidth(sh * fboAspect);
            gCam.SetHeight(sh);
        }
        gPerCam.SetAspect(fboAspect);
        gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    case FillMode::ASPECTCROP:
    default:
        if (fboAspect > sAspect) {
            // scale height
            gCam.SetWidth(sw);
            gCam.SetHeight(sw / fboAspect);
        } else {
            gCam.SetWidth(sh * fboAspect);
            gCam.SetHeight(sh);
        }
        gPerCam.SetAspect(fboAspect);
        gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    }
    gCam.Update();
    gPerCam.Update();
    scene.UpdateLinkedCamera("global");
}

void VulkanRender::Impl::clearLastRenderGraph() {
    for (auto& p : m_passes) {
        p->destory(*m_device, m_rendering_resources);
    }
    m_passes.clear();
    m_device->tex_cache().Clear();

    m_vertex_buf->destroy();
    m_dyn_buf->destroy();

    m_vertex_buf->allocate();
    m_dyn_buf->allocate();
}

void VulkanRender::Impl::compileRenderGraph(Scene& scene, rg::RenderGraph& rg) {
    if (! m_inited) return;
    m_pass_loaded = false;

    auto nodes             = rg.topologicalOrder();
    auto node_release_texs = rg.getLastReadTexs(nodes);

    m_passes.clear();
    m_passes.resize(nodes.size());

    std::transform(nodes.begin(),
                   nodes.end(),
                   node_release_texs.begin(),
                   m_passes.begin(),
                   [&rg](auto& id, auto& texs) {
                       auto* pass = rg.getPass(id);
                       assert(pass != nullptr);
                       VulkanPass* vpass = static_cast<VulkanPass*>(pass);
                       // LOG_INFO("----release tex");
                       for (auto& tex : texs) {
                           vpass->addReleaseTexs(spanone<const std::string_view> { tex->key() });
                           //    LOG_INFO("%s", tex->key().data());
                       }
                       return vpass;
                   });

    m_passes.insert(m_passes.begin(), m_prepass.get());
    m_passes.push_back(m_finpass.get());

    setRenderTargetSize(scene, rg);

    glslang::InitializeProcess();
    for (auto* p : m_passes) {
        if (! p->prepared()) {
            p->prepare(scene, *m_device, m_rendering_resources);
        }
    }
    glslang::FinalizeProcess();

    VVK_CHECK_VOID_RE(m_upload_cmd.Begin(VkCommandBufferBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    }));
    m_vertex_buf->recordUpload(m_upload_cmd);
    VVK_CHECK_VOID_RE(m_upload_cmd.End());
    {
        VkSubmitInfo sub_info {
            .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext              = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers    = m_upload_cmd.address(),
        };
        VVK_CHECK_VOID_RE(m_device->graphics_queue().handle.Submit(sub_info, {}));
        VVK_CHECK_VOID_RE(m_device->handle().WaitIdle());
    }
    m_pass_loaded = true;
};
