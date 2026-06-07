// BridgeExSwapchain — `owe::ExSwapchain` adapter over
// BridgeProducerCore for the wescene host. See header notes that used
// to live in the classic .hpp.

module;

#include <waywallen-bridge/pool.h>

export module waywallen.bridge_ex_swapchain;

import rstd.cppstd;
import wescene.vulkan;
import waywallen.bridge_producer_core;

export namespace ww_wescene
{

class BridgeExSwapchain : public owe::ExSwapchain {
public:
    static constexpr uint32_t kMaxSlots = BridgeProducerCore::kMaxSlots;

    BridgeExSwapchain(ww_pool_t* pool, int sock);
    ~BridgeExSwapchain() override;

    void queueDirective(const ww_pool_directive_t& directive) { m_core.queueDirective(directive); }
    bool hasPendingDirective() const { return m_core.hasPendingDirective(); }

    void setOnFirstNegotiated(std::function<void()> cb) {
        m_core.setOnFirstNegotiated(std::move(cb));
    }

    void poll() override { m_core.drainPendingDirective(); }

    bool acquireRenderTarget(owe::vulkan::ImageParameters& out) override;

    void submitRendered(int producer_sync_fd) override { m_core.submitSlot(producer_sync_fd); }

    unsigned width() const override { return m_core.width(); }
    unsigned height() const override { return m_core.height(); }
    VkFormat format() const override { return m_core.format(); }

    VkImageLayout producerOutputLayout() const override { return VK_IMAGE_LAYOUT_GENERAL; }

    uint32_t releaseTargetQueueFamily() const override { return VK_QUEUE_FAMILY_FOREIGN_EXT; }

    bool ready() const override { return m_core.ready(); }

    void setOnReadyChanged(std::function<void(const owe::ExSwapchainReadyEvent&)> cb) override {
        if (! cb) {
            m_core.setOnReadyChanged({});
            return;
        }
        m_core.setOnReadyChanged([cb = std::move(cb)](const BridgeReadyEvent& e) {
            cb(owe::ExSwapchainReadyEvent {
                .ready  = e.ready,
                .width  = e.width,
                .height = e.height,
                .format = e.format,
            });
        });
    }

private:
    BridgeProducerCore m_core;
};

} // namespace ww_wescene
