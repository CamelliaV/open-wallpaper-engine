export module waywallen.bridge_ex_swapchain;

import rstd.cppstd;
import wescene.vulkan;
import waywallen.bridge_producer_core;

export namespace ww_wescene
{

class BridgeExSwapchain : public owe::ExSwapchain {
public:
    static constexpr uint32_t kMaxSlots = BridgeProducerCore::kMaxSlots;

    explicit BridgeExSwapchain(std::shared_ptr<BridgeSession> session);
    ~BridgeExSwapchain() override;

    void queueDirective(const ww_pool_directive_t& directive) { m_core.queueDirective(directive); }
    bool hasPendingDirective() const { return m_core.hasPendingDirective(); }

    void setOnFirstNegotiated(std::function<void()> cb) {
        m_core.setOnFirstNegotiated(std::move(cb));
    }

    void poll() override { m_core.drainPendingDirective(); }

    owe::FrameSurfaceAcquireResult acquireRenderTarget() override;

    unsigned width() const override { return m_core.width(); }
    unsigned height() const override { return m_core.height(); }
    VkFormat format() const override { return m_core.format(); }

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
    owe::FrameSurfaceCompletionResult CompleteRendered(owe::FrameSurfaceIdentity identity,
                                                       int producer_sync_fd) override;
    owe::FrameSurfaceCompletionResult
    AbortRenderTarget(owe::FrameSurfaceIdentity identity) override;

    BridgeProducerCore                m_core;
    std::optional<BridgeSlotIdentity> m_pending_identity;
};

} // namespace ww_wescene
